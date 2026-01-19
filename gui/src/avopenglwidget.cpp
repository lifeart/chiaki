// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <avopenglwidget.h>
#include <avopenglframeuploader.h>
#include <streamsession.h>
#include <chiaki/common.h>

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLDebugLogger>
#include <QThread>
#include <QTimer>
#include <QResizeEvent>
#include <QWindow>

#ifdef Q_OS_MACOS
#include "macoshdrhelper.h"
#endif

//#define MOUSE_TIMEOUT_MS 1000

//#define DEBUG_OPENGL

static const char *shader_vert_glsl = R"glsl(
#version 150 core

in vec2 pos_attr;

out vec2 uv_var;

void main()
{
	uv_var = pos_attr;
	gl_Position = vec4(pos_attr * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)glsl";

static const char *yuv420p_shader_frag_glsl = R"glsl(
#version 150 core

uniform sampler2D plane1; // Y
uniform sampler2D plane2; // U
uniform sampler2D plane3; // V

in vec2 uv_var;
out vec4 out_color;

const float yScale = 255.0 / (235.0 - 16.0);
const float uvScale = 255.0 / (240.0 - 16.0);

void main() {
    float y = texture2D(plane1, uv_var).r;
    float u = texture2D(plane2, uv_var).r - 0.5;
    float v = texture2D(plane3, uv_var).r - 0.5;

    y = y - 16.0/255.0;

    float r = y*yScale +                          v*uvScale*1.5748;
    float g = y*yScale - u*uvScale*1.8556*0.101 - v*uvScale*1.5748*0.2973;
    float b = y*yScale + u*uvScale*1.8556;

    r = clamp(r, 0.0, 1.0);
    g = clamp(g, 0.0, 1.0);
    b = clamp(b, 0.0, 1.0);

	out_color = vec4(r,g,b,1.0);
}
)glsl";

static const char *nv12_shader_frag_glsl = R"glsl(
#version 150 core

uniform sampler2D plane1; // Y
uniform sampler2D plane2; // interlaced UV

in vec2 uv_var;

out vec4 out_color;

void main()
{
	vec3 yuv = vec3(
		(texture(plane1, uv_var).r - (16.0 / 255.0)) / ((235.0 - 16.0) / 255.0),
		(texture(plane2, uv_var).r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5,
		(texture(plane2, uv_var).g - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5
	);
	vec3 rgb = mat3(
		1.0,		1.0,		1.0,
		0.0,		-0.21482,	2.12798,
		1.28033,	-0.38059,	0.0) * yuv;
	out_color = vec4(rgb, 1.0);
}
)glsl";

// P010LE: 10-bit YUV stored in 16-bit containers (little-endian, values in upper 10 bits)
// Used for HDR content with BT.2020 color space and PQ (ST.2084) transfer function
static const char *p010le_shader_frag_glsl = R"glsl(
#version 150 core

uniform sampler2D plane1; // Y plane (16-bit, 10-bit value in upper bits)
uniform sampler2D plane2; // interleaved UV plane (16-bit per component)
uniform float max_edr;    // Maximum EDR value for the display (1.0 = SDR, >1.0 = HDR)

in vec2 uv_var;

out vec4 out_color;

// PQ (Perceptual Quantizer / ST.2084) EOTF constants
const float PQ_m1 = 0.1593017578125;  // 2610/16384
const float PQ_m2 = 78.84375;          // 2523/32 * 128
const float PQ_c1 = 0.8359375;         // 3424/4096
const float PQ_c2 = 18.8515625;        // 2413/128
const float PQ_c3 = 18.6875;           // 2392/128

// Apply PQ EOTF to convert from PQ signal to linear light (0-10000 nits normalized to 0-1)
vec3 pq_eotf(vec3 pq)
{
	vec3 p = pow(max(pq, vec3(0.0)), vec3(1.0 / PQ_m2));
	vec3 num = max(p - PQ_c1, vec3(0.0));
	vec3 den = PQ_c2 - PQ_c3 * p;
	return pow(max(num / max(den, vec3(0.00001)), vec3(0.0)), vec3(1.0 / PQ_m1));
}

// BT.2020 to BT.709 gamut conversion matrix (column-major for GLSL)
// This maps linear BT.2020 RGB to linear BT.709 RGB
const mat3 bt2020_to_bt709 = mat3(
	 1.6605, -0.1246, -0.0182,   // column 0
	-0.5876,  1.1329, -0.1006,   // column 1
	-0.0728, -0.0083,  1.1187    // column 2
);

// Attempt basic BT.2390 style tone mapping (attempt similar output to mpv/other players)
vec3 tonemap_bt2390(vec3 rgb, float peak)
{
	// Simple attempt at BT.2390 style curve
	float maxRGB = max(max(rgb.r, rgb.g), rgb.b);
	if(maxRGB <= 0.0) return rgb;

	// Attempt soft-knee curve
	float ks = 1.5 * peak - 0.5;
	float tb = (1.0 - ks) / (peak - ks);

	float t;
	if(maxRGB < ks) {
		t = maxRGB;
	} else {
		float t1 = (maxRGB - ks) / (peak - ks);
		float t2 = t1 * t1 * (3.0 - 2.0 * t1);  // smoothstep
		t = ks + (1.0 - ks) * t2;
	}

	return rgb * (t / maxRGB);
}

void main()
{
	// Read Y and UV values (OpenGL normalizes 16-bit to [0,1])
	float y_raw = texture(plane1, uv_var).r;
	vec2 uv_raw = texture(plane2, uv_var).rg;

	// Convert from limited range (10-bit: 64-940 for Y, 64-960 for UV)
	// Values are in upper 10 bits of 16-bit, so effectively /65535 then *1023
	float y = clamp((y_raw - 0.0625) / 0.8555, 0.0, 1.0);
	float u = (uv_raw.r - 0.0625) / 0.875 - 0.5;
	float v = (uv_raw.g - 0.0625) / 0.875 - 0.5;

	// BT.2020 YUV to RGB conversion (NCL - non-constant luminance)
	vec3 rgb;
	rgb.r = y + 1.4746 * v;
	rgb.g = y - 0.1646 * u - 0.5714 * v;
	rgb.b = y + 1.8814 * u;
	// Don't clamp here - PQ signal can have slight out-of-range values
	rgb = max(rgb, vec3(0.0));  // Only clip negative (invalid signal)

	// Apply PQ EOTF to get linear light values (normalized to 10000 nits = 1.0)
	vec3 linear_bt2020 = pq_eotf(rgb);

	// Convert from BT.2020 to BT.709/sRGB gamut
	vec3 linear_rgb = bt2020_to_bt709 * linear_bt2020;

	vec3 final_rgb;
	if(max_edr > 1.0)
	{
		// HDR display with EDR support (macOS extendedSRGB color space)
		// PQ EOTF output: 0.0 = black, 1.0 = 10000 nits (linear light)
		// macOS EDR with extendedSRGB: sRGB gamma curve, values >1.0 for HDR
		//
		// Scale linear light so 203 nits (HDR reference white) = 1.0
		// This is the standard HDR reference for PQ content
		// 203 nits = 0.0203 in PQ linear (10000 nit scale)
		vec3 scaled = linear_rgb * (1.0 / 0.0203);  // ~49.26x, maps 203 nits to 1.0

		// Clip negative values from gamut conversion (out-of-gamut colors)
		scaled = max(scaled, vec3(0.0));

		// Apply sRGB gamma (extendedSRGB expects gamma-encoded values)
		final_rgb = pow(scaled, vec3(1.0 / 2.2));

		// Clamp to max EDR
		final_rgb = clamp(final_rgb, vec3(0.0), vec3(max_edr));
	}
	else
	{
		// SDR display: tone map HDR to SDR
		// Clip negative values from gamut conversion first
		linear_rgb = max(linear_rgb, vec3(0.0));

		// Scale: assume content mastered for ~1000 nits peak (0.1 in PQ linear output)
		// We want to map 0.1 (1000 nits) -> ~1.0 for SDR, so multiply by 10
		vec3 scaled = linear_rgb * 10.0;

		// Apply tone mapping (attempt soft roll-off, similar to bt.2390)
		final_rgb = tonemap_bt2390(scaled, 10.0);

		// Apply gamma for SDR display (sRGB approximation)
		final_rgb = pow(clamp(final_rgb, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
	}

	out_color = vec4(final_rgb, 1.0);
}
)glsl";

ConversionConfig conversion_configs[] = {
	{
		AV_PIX_FMT_YUV420P,
		shader_vert_glsl,
		yuv420p_shader_frag_glsl,
		3,
		{
			{ 1, 1, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
			{ 2, 2, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
			{ 2, 2, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE }
		}
	},
	{
		AV_PIX_FMT_NV12,
		shader_vert_glsl,
		nv12_shader_frag_glsl,
		2,
		{
			{ 1, 1, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
			{ 2, 2, 2, GL_RG8, GL_RG, GL_UNSIGNED_BYTE }
		}
	},
	{
		AV_PIX_FMT_P010LE,
		shader_vert_glsl,
		p010le_shader_frag_glsl,
		2,
		{
			{ 1, 1, 2, GL_R16, GL_RED, GL_UNSIGNED_SHORT },   // 10-bit Y in 16-bit container (2 bytes per pixel)
			{ 2, 2, 4, GL_RG16, GL_RG, GL_UNSIGNED_SHORT }    // 10-bit UV interleaved (4 bytes per pixel pair)
		}
	}
};

static const float vert_pos[] = {
	0.0f, 0.0f,
	0.0f, 1.0f,
	1.0f, 0.0f,
	1.0f, 1.0f
};

QSurfaceFormat AVOpenGLWidget::CreateSurfaceFormat()
{
	QSurfaceFormat format;
	format.setDepthBufferSize(0);
	format.setStencilBufferSize(0);
	format.setVersion(3, 2);
	format.setProfile(QSurfaceFormat::CoreProfile);
	// M1 Mac optimizations: enable vsync and triple buffering for smooth playback
	format.setSwapInterval(1);  // VSync enabled - reduces tearing and power consumption
	format.setSwapBehavior(QSurfaceFormat::TripleBuffer);  // Smooth frame delivery
#ifdef DEBUG_OPENGL
	format.setOption(QSurfaceFormat::DebugContext, true);
#endif
	return format;
}

AVOpenGLWidget::AVOpenGLWidget(StreamSession *session, QWidget *parent, ResolutionMode resolution_mode)
	: QOpenGLWidget(parent),
	session(session), resolution_mode(resolution_mode)
{
	enum AVPixelFormat pixel_format = chiaki_ffmpeg_decoder_get_pixel_format(session->GetFfmpegDecoder());

	CHIAKI_LOGI(session->GetChiakiLog(), "AVOpenGLWidget: Creating with pixel format %d", pixel_format);

	conversion_config = nullptr;
	for(auto &cc : conversion_configs)
	{
		CHIAKI_LOGV(session->GetChiakiLog(), "AVOpenGLWidget: Checking conversion config for format %d", cc.pixel_format);
		if(pixel_format == cc.pixel_format)
		{
			conversion_config = &cc;
			CHIAKI_LOGI(session->GetChiakiLog(), "AVOpenGLWidget: Found matching conversion config for format %d with %d planes",
				cc.pixel_format, cc.planes);
			break;
		}
	}

	if(!conversion_config)
	{
		CHIAKI_LOGE(session->GetChiakiLog(), "AVOpenGLWidget: No matching conversion config for pixel format %d", pixel_format);
		throw Exception("No matching video conversion config can be found");
	}

	setFormat(CreateSurfaceFormat());

	frame_uploader_context = nullptr;
	frame_uploader = nullptr;
	frame_uploader_thread = nullptr;
	frame_fg = 0;
	first_frame_received = false;
	max_edr_uniform = -1;

	// Check if we're in HDR mode (P010LE format)
	is_hdr_mode = (pixel_format == AV_PIX_FMT_P010LE);

	// Get max EDR value for the display
	max_edr_value = 1.0f;  // Default to SDR
#ifdef Q_OS_MACOS
	{
		bool hdrSupported = macosIsHDRSupported();
		float detectedEDR = macosGetMaxEDR();
		CHIAKI_LOGI(session->GetChiakiLog(), "AVOpenGLWidget: Display HDR check - supported=%d, maxEDR=%.2f, is_hdr_mode=%d",
			hdrSupported, detectedEDR, is_hdr_mode);

		if(is_hdr_mode && hdrSupported)
		{
			max_edr_value = detectedEDR;
			CHIAKI_LOGI(session->GetChiakiLog(), "AVOpenGLWidget: HDR mode enabled, display max EDR: %.2f", max_edr_value);
		}
		else if(is_hdr_mode)
		{
			CHIAKI_LOGI(session->GetChiakiLog(), "AVOpenGLWidget: HDR content but display doesn't support HDR (EDR=%.2f), using tone mapping", detectedEDR);
		}
	}
#endif

	// Create loading indicator label
	loading_label = new QLabel(tr("Loading video stream..."), this);
	loading_label->setAlignment(Qt::AlignCenter);
	loading_label->setStyleSheet(
		"QLabel {"
		"  color: white;"
		"  font-size: 18px;"
		"  background-color: transparent;"
		"}"
	);
	loading_label->setAttribute(Qt::WA_TransparentForMouseEvents);
	loading_label->show();

	// Create stats overlay label (top-left corner)
	stats_label = new QLabel(this);
	stats_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	stats_label->setStyleSheet(
		"QLabel {"
		"  color: #00ff00;"
		"  font-size: 14px;"
		"  font-family: monospace;"
		"  background-color: rgba(0, 0, 0, 180);"
		"  padding: 8px;"
		"  border-radius: 4px;"
		"}"
	);
	stats_label->setAttribute(Qt::WA_TransparentForMouseEvents);
	stats_label->setText("Connecting...");
	stats_label->adjustSize();
	stats_label->move(10, 10);
	stats_label->raise();  // Ensure it's on top of OpenGL content
	stats_label->show();

	// Initialize FPS tracking
	frame_count = 0;
	current_fps = 0.0f;
	fps_timer.start();

	// Create timer to update stats overlay every 500ms
	stats_update_timer = new QTimer(this);
	connect(stats_update_timer, &QTimer::timeout, this, &AVOpenGLWidget::UpdateStatsOverlay);
	stats_update_timer->start(500);
}

AVOpenGLWidget::~AVOpenGLWidget()
{
	if(stats_update_timer)
	{
		stats_update_timer->stop();
		delete stats_update_timer;
	}

	if(frame_uploader_thread)
	{
		frame_uploader_thread->quit();
		frame_uploader_thread->wait();
		delete frame_uploader_thread;
	}
	delete frame_uploader;
	delete frame_uploader_context;
	delete frame_uploader_surface;

	// Clean up OpenGL resources
	makeCurrent();
	auto f = QOpenGLContext::currentContext()->extraFunctions();
	if(f)
	{
		f->glDeleteProgram(program);
		f->glDeleteBuffers(1, &vbo);
		f->glDeleteVertexArrays(1, &vao);
		for(int i = 0; i < 2; i++)
		{
			f->glDeleteTextures(conversion_config->planes, frames[i].tex);
			f->glDeleteBuffers(conversion_config->planes, frames[i].pbo);
		}
	}
	doneCurrent();

	delete loading_label;
	delete stats_label;
}

/*
void AVOpenGLWidget::mouseMoveEvent(QMouseEvent *event)
{
	QOpenGLWidget::mouseMoveEvent(event);
	ResetMouseTimeout();
}

void AVOpenGLWidget::ResetMouseTimeout()
{
	//unsetCursor();
	mouse_timer->start(MOUSE_TIMEOUT_MS);
}
*/
void AVOpenGLWidget::HideMouse()
{
	setCursor(Qt::BlankCursor);
}

void AVOpenGLWidget::ToggleZoom()
{
	if( resolution_mode == Zoom )
		resolution_mode = Normal;
	else
		resolution_mode = Zoom;
}

void AVOpenGLWidget::ToggleStretch()
{
	if( resolution_mode == Stretch )
		resolution_mode = Normal;
	else
		resolution_mode = Stretch;
}

void AVOpenGLWidget::SwapFrames()
{
	QMutexLocker lock(&frames_mutex);
	frame_fg = 1 - frame_fg;

	// Track frame count for FPS calculation
	frame_count++;

	// Hide loading indicator on first frame
	if(!first_frame_received)
	{
		first_frame_received = true;
		QMetaObject::invokeMethod(loading_label, "hide", Qt::QueuedConnection);
		QMetaObject::invokeMethod(stats_label, "raise", Qt::QueuedConnection);
	}

	QMetaObject::invokeMethod(this, "update");
}

bool AVOpenGLFrame::Update(AVFrame *frame, ChiakiLog *log)
{
	auto f = QOpenGLContext::currentContext()->extraFunctions();

    if(frame->format != conversion_config->pixel_format)
	{
		CHIAKI_LOGE(log, "AVOpenGLFrame got AVFrame with invalid format %d (expected %d)",
			frame->format, conversion_config->pixel_format);
		return false;
	}

	width = frame->width;
	height = frame->height;

	for(int i=0; i<conversion_config->planes; i++)
	{
		int plane_width = frame->width / conversion_config->plane_configs[i].width_divider;
		int plane_height = frame->height / conversion_config->plane_configs[i].height_divider;
		int row_bytes = plane_width * conversion_config->plane_configs[i].data_per_pixel;
		int size = plane_height * row_bytes;

		f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
		f->glBufferData(GL_PIXEL_UNPACK_BUFFER, size, nullptr, GL_STREAM_DRAW);

		auto buf = reinterpret_cast<uint8_t *>(f->glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
		if(!buf)
		{
			CHIAKI_LOGE(log, "AVOpenGLFrame failed to map PBO for plane %d", i);
			continue;
		}

		// Check if source stride matches destination stride
		if(frame->linesize[i] == row_bytes)
		{
			memcpy(buf, frame->data[i], size);
		}
		else
		{
			// Row-by-row copy when strides differ
			for(int l=0; l<plane_height; l++)
				memcpy(buf + row_bytes * l, frame->data[i] + frame->linesize[i] * l, row_bytes);
		}

		f->glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

		f->glBindTexture(GL_TEXTURE_2D, tex[i]);
		f->glTexImage2D(GL_TEXTURE_2D, 0, conversion_config->plane_configs[i].internal_format, plane_width, plane_height, 0, conversion_config->plane_configs[i].format, conversion_config->plane_configs[i].data_type, nullptr);
	}

	f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	f->glFinish();

	return true;
}

void AVOpenGLWidget::initializeGL()
{
	auto f = QOpenGLContext::currentContext()->extraFunctions();

	const char *gl_version = (const char *)f->glGetString(GL_VERSION);
	CHIAKI_LOGI(session->GetChiakiLog(), "OpenGL initialized with version \"%s\"", gl_version ? gl_version : "(null)");

#ifdef DEBUG_OPENGL
	auto logger = new QOpenGLDebugLogger(this);
	logger->initialize();
	connect(logger, &QOpenGLDebugLogger::messageLogged, this, [](const QOpenGLDebugMessage &msg) {
		qDebug() << msg;
	});
	logger->startLogging();
#endif

	auto CheckShaderCompiled = [&](GLuint shader) -> bool {
		GLint compiled = 0;
		f->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if(compiled == GL_TRUE)
			return true;
		GLint info_log_size = 0;
		f->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_size);
		QVector<GLchar> info_log(info_log_size);
		f->glGetShaderInfoLog(shader, info_log_size, &info_log_size, info_log.data());
		f->glDeleteShader(shader);
		CHIAKI_LOGE(session->GetChiakiLog(), "Failed to Compile Shader:\n%s", info_log.data());
		return false;
	};

	GLuint shader_vert = f->glCreateShader(GL_VERTEX_SHADER);
	f->glShaderSource(shader_vert, 1, &conversion_config->shader_vert_glsl, nullptr);
	f->glCompileShader(shader_vert);
	if(!CheckShaderCompiled(shader_vert))
		return;

	GLuint shader_frag = f->glCreateShader(GL_FRAGMENT_SHADER);
	f->glShaderSource(shader_frag, 1, &conversion_config->shader_frag_glsl, nullptr);
	f->glCompileShader(shader_frag);
	if(!CheckShaderCompiled(shader_frag))
	{
		f->glDeleteShader(shader_vert);
		return;
	}

	program = f->glCreateProgram();
	f->glAttachShader(program, shader_vert);
	f->glAttachShader(program, shader_frag);
	f->glBindAttribLocation(program, 0, "pos_attr");
	f->glLinkProgram(program);

	GLint linked = 0;
	f->glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE)
	{
		GLint info_log_size = 0;
		f->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_log_size);
		QVector<GLchar> info_log(info_log_size);
		f->glGetProgramInfoLog(program, info_log_size, &info_log_size, info_log.data());
		f->glDeleteProgram(program);
		f->glDeleteShader(shader_vert);
		f->glDeleteShader(shader_frag);
		CHIAKI_LOGE(session->GetChiakiLog(), "Failed to Link Shader Program:\n%s", info_log.data());
		return;
	}

	// Shaders can be deleted after linking - they remain valid as part of the program
	f->glDeleteShader(shader_vert);
	f->glDeleteShader(shader_frag);

	for(int i=0; i<2; i++)
	{
		frames[i].conversion_config = conversion_config;
		f->glGenTextures(conversion_config->planes, frames[i].tex);
		f->glGenBuffers(conversion_config->planes, frames[i].pbo);
		// Default UV values for neutral chroma (8-bit and 16-bit versions)
		uint8_t uv_default_8[] = {0x80, 0x80};
		uint16_t uv_default_16[] = {0x8000, 0x8000};
		for(int j=0; j<conversion_config->planes; j++)
		{
			f->glBindTexture(GL_TEXTURE_2D, frames[i].tex[j]);
			f->glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			f->glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			f->glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			f->glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			// Use appropriate default data based on data type
			void *default_data = nullptr;
			if(j > 0)  // UV planes need neutral chroma default
				default_data = (conversion_config->plane_configs[j].data_type == GL_UNSIGNED_SHORT) ? (void*)uv_default_16 : (void*)uv_default_8;
			f->glTexImage2D(GL_TEXTURE_2D, 0, conversion_config->plane_configs[j].internal_format, 1, 1, 0, conversion_config->plane_configs[j].format, conversion_config->plane_configs[j].data_type, default_data);
		}
		frames[i].width = 0;
		frames[i].height = 0;
	}

	f->glUseProgram(program);

	// bind only as many planes as we need
	const char *plane_names[] = {"plane1", "plane2", "plane3"};
	for(int i=0; i<sizeof(plane_names)/sizeof(char *); i++)
	{
		f->glUniform1i(f->glGetUniformLocation(program, plane_names[i]), i);
	}

	// Get max_edr uniform location for HDR shader
	if(is_hdr_mode)
	{
		max_edr_uniform = f->glGetUniformLocation(program, "max_edr");
		if(max_edr_uniform >= 0)
		{
			f->glUniform1f(max_edr_uniform, max_edr_value);
			CHIAKI_LOGI(session->GetChiakiLog(), "HDR shader max_edr uniform set to %.2f", max_edr_value);
		}

		// Enable HDR on macOS
#ifdef Q_OS_MACOS
		if(max_edr_value > 1.0f)
		{
			// Enable EDR on the OpenGL widget itself
			WId widgetId = winId();
			if(widgetId)
			{
				macosEnableEDR((void*)widgetId);
				CHIAKI_LOGI(session->GetChiakiLog(), "macOS EDR enabled on OpenGL widget");
			}

			// Also enable HDR color space on the window
			QWidget *parentWindow = window();
			if(parentWindow)
			{
				QWindow *win = parentWindow->windowHandle();
				if(win)
				{
					void *viewPtr = (void*)win->winId();
					macosSetWindowColorSpace(viewPtr, true);
					macosEnableEDR(viewPtr);
					CHIAKI_LOGI(session->GetChiakiLog(), "macOS HDR/EDR enabled for window (max_edr=%.2f)", max_edr_value);
				}
			}
		}
#endif
	}

	f->glGenVertexArrays(1, &vao);
	f->glBindVertexArray(vao);

	f->glGenBuffers(1, &vbo);
	f->glBindBuffer(GL_ARRAY_BUFFER, vbo);
	f->glBufferData(GL_ARRAY_BUFFER, sizeof(vert_pos), vert_pos, GL_STATIC_DRAW);

	f->glBindBuffer(GL_ARRAY_BUFFER, vbo);
	f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	f->glEnableVertexAttribArray(0);

	f->glCullFace(GL_BACK);
	f->glEnable(GL_CULL_FACE);
	f->glClearColor(0.0, 0.0, 0.0, 1.0);

	frame_uploader_context = new QOpenGLContext(nullptr);
	frame_uploader_context->setFormat(context()->format());
	frame_uploader_context->setShareContext(context());
	if(!frame_uploader_context->create())
	{
		CHIAKI_LOGE(session->GetChiakiLog(), "Failed to create upload OpenGL context");
		return;
	}

	frame_uploader_surface = new QOffscreenSurface();
	frame_uploader_surface->setFormat(context()->format());
	frame_uploader_surface->create();
	frame_uploader = new AVOpenGLFrameUploader(session, this, frame_uploader_context, frame_uploader_surface);
	frame_fg = 0;

	frame_uploader_thread = new QThread(this);
	frame_uploader_thread->setObjectName("Frame Uploader");
	frame_uploader_context->moveToThread(frame_uploader_thread);
	frame_uploader->moveToThread(frame_uploader_thread);
	frame_uploader_thread->start();
}

void AVOpenGLWidget::paintGL()
{
	auto f = QOpenGLContext::currentContext()->extraFunctions();

	f->glClear(GL_COLOR_BUFFER_BIT);

	int widget_width = (int)(width() * devicePixelRatioF());
	int widget_height = (int)(height() * devicePixelRatioF());

	QMutexLocker lock(&frames_mutex);
	AVOpenGLFrame *frame = &frames[frame_fg];

	GLsizei vp_width, vp_height;
	if(!frame->width || !frame->height)
	{
		vp_width = widget_width;
		vp_height = widget_height;
	}
	// Optimized for normal most often, followed by zoom, followed by stretch
	else if(resolution_mode == Normal)
	{
		float aspect = (float)frame->width / (float)frame->height;
		if(aspect < (float)widget_width / (float)widget_height)
		{
			vp_height = widget_height;
			vp_width = (GLsizei)(vp_height * aspect);
		}
		else
		{
			vp_width = widget_width;
			vp_height = (GLsizei)(vp_width / aspect);
		}
	}
	else if(resolution_mode == Zoom)
	{
		float aspect = (float)frame->width / (float)frame->height;
		if(aspect < (float)widget_width / (float)widget_height)
		{
			vp_width = widget_width;
			vp_height = (GLsizei)(vp_width / aspect);
		}
		else
		{
			vp_height = widget_height;
			vp_width = (GLsizei)(vp_height * aspect);
		}
	}
	// Stretch if not Normal or Zoom (least likely so least optimized)
	else
	{
		float aspect = (float)frame->width / (float)frame->height;
		if(aspect < (float)widget_width / (float)widget_height)
		{
			vp_height = widget_height;
			vp_width = widget_width;
		}
		else
		{
			vp_width = widget_width;
			vp_height = widget_height;
		}
	}

	f->glViewport((widget_width - vp_width) / 2, (widget_height - vp_height) / 2, vp_width, vp_height);

	f->glUseProgram(program);
	f->glBindVertexArray(vao);

	for(int i=0; i<conversion_config->planes; i++)
	{
		f->glActiveTexture(GL_TEXTURE0 + i);
		f->glBindTexture(GL_TEXTURE_2D, frame->tex[i]);
	}

	f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	f->glFinish();
}

void AVOpenGLWidget::resizeEvent(QResizeEvent *event)
{
	QOpenGLWidget::resizeEvent(event);

	// Center the loading label
	if(loading_label)
	{
		loading_label->setGeometry(0, 0, width(), height());
	}

	// Position stats label in top-left corner with margin
	if(stats_label)
	{
		stats_label->adjustSize();
		stats_label->move(10, 10);
	}
}

void AVOpenGLWidget::UpdateStatsOverlay()
{
	if(!stats_label)
		return;

	// Get codec info
	ChiakiFfmpegDecoder *decoder = session->GetFfmpegDecoder();
	const char *codec_name = decoder ? chiaki_codec_name(decoder->codec) : "Unknown";

	// Build stats text
	QString mode_str = is_hdr_mode ? "HDR" : "SDR";
	if(is_hdr_mode && max_edr_value > 1.0f)
		mode_str += QString(" (EDR: %1)").arg(max_edr_value, 0, 'f', 1);
	else if(is_hdr_mode)
		mode_str += " (Tone mapped)";

	QString stats_text;

	if(!first_frame_received)
	{
		stats_text = QString(
			"Mode: %1\n"
			"Codec: %2\n"
			"Resolution: --\n"
			"FPS: --"
		).arg(mode_str)
		 .arg(codec_name);
	}
	else
	{
		// Calculate FPS
		qint64 elapsed = fps_timer.elapsed();
		if(elapsed > 0)
		{
			current_fps = (frame_count * 1000.0f) / elapsed;
		}

		// Reset FPS counter every update
		frame_count = 0;
		fps_timer.restart();

		// Get resolution from current frame
		QMutexLocker lock(&frames_mutex);
		AVOpenGLFrame *frame = &frames[frame_fg];
		unsigned int res_width = frame->width;
		unsigned int res_height = frame->height;
		lock.unlock();

		stats_text = QString(
			"Mode: %1\n"
			"Codec: %2\n"
			"Resolution: %3x%4\n"
			"FPS: %5"
		).arg(mode_str)
		 .arg(codec_name)
		 .arg(res_width)
		 .arg(res_height)
		 .arg(current_fps, 0, 'f', 1);
	}

	stats_label->setText(stats_text);
	stats_label->adjustSize();
	stats_label->raise();
}