# chiaki
This fork of Chiaki combines the works of Alvaromunoz, Jbaiters, Egoistically, and Streetpea to deliver an optimized gaming experience. It includes features such as haptic vibrations and adaptive triggers for a more immersive gaming experience, as well as improved colorimetry. Although haptic vibrations only work with USB and may not function, an emulation function for vibrations has been added for use with both USB and Bluetooth. The software is built using QT6.

## Features:

- Adaptive Triggers (Bluetooth and USB)
- Haptic rumble (only USB, may not work)
- Emulated haptic rumble (works with USB and Bluetooth)
- Built with QT6
- Improved colorimetry
- **Full HDR support on macOS** with EDR (Extended Dynamic Range) for HDR displays
- Automatic HDR/SDR detection (P010LE for HDR, NV12/YUV420P for SDR)
- PQ (ST.2084) transfer function and BT.2020 color space conversion
- Automatic tone mapping for SDR displays when playing HDR content
- Optimized for Apple Silicon (M1/M2/M3) with VideoToolbox hardware acceleration
- Loading indicator for better user experience during connection
- **Real-time stats overlay** showing mode (HDR/SDR), codec, resolution, and FPS

## Not Working:

- Controller microphone
- Controller speaker.

## Build
The program has been compiled for Mac (Apple Silicon) and can be built from source using CMake, Qt6, QtOpenGL and QtSvg, FFMPEG (libavcodec with H264 is enough), libopus, OpenSSL 1.1, SDL 2, protoc and the protobuf Python library (only used during compilation for Nanopb). To build the program, follow these instructions:

    bash
    Copy code
    git submodule update --init
    mkdir build && cd build
    cmake ..
    make

Enjoy playing games like Astrobots, now fully playable.

## Changelog

### 2026-01-19 - HDR Support & Video Pipeline Fixes

**New Features:**
- Added automatic detection of video stream pixel format from decoded frames
- HDR streams now correctly use P010LE (10-bit) pixel format with proper shader
- SDR streams use NV12 (hardware) or YUV420P (software) pixel format
- Added discovery ping to 192.168.88.0/24 subnet in addition to local broadcast
- Added "Loading video stream..." indicator while waiting for first frame
- Optimized video pipeline for Apple Silicon (M1/M2/M3) Macs
- Added real-time stats overlay displaying:
  - Mode (HDR/SDR with EDR value or tone mapping status)
  - Video codec (H264, H265, H265_HDR)
  - Stream resolution
  - Real-time FPS counter (updated every 500ms)

**HDR Display Support (macOS):**
- Full HDR output using macOS EDR (Extended Dynamic Range)
- Automatic detection of HDR-capable displays (checks `maximumExtendedDynamicRangeColorComponentValue`)
- PQ (ST.2084/Perceptual Quantizer) EOTF for proper HDR decoding
- BT.2020 to BT.709/sRGB color space conversion matrix
- Extended sRGB color space for HDR window output
- Automatic Reinhard tone mapping for SDR displays when playing HDR content
- Max EDR value passed to shader for proper highlight rendering
- Thread-safe macOS API calls (dispatched to main thread)

**Bug Fixes:**
- Fixed black screen issue caused by incorrect linesize comparison in texture upload
- Fixed SDR streams incorrectly requesting HDR dynamic range from PlayStation
- Fixed deprecated `avcodec_close()` call for newer FFmpeg versions (8.x)
- Fixed thread safety issues in pixel format detection
- Fixed P010LE shader to use consistent uniform names (`plane1`, `plane2`)
- Fixed texture upload to use correct data type (GL_UNSIGNED_SHORT for 16-bit formats)
- Fixed `winId()` type mismatch in macOS HDR setup (was passing NSView* as NSWindow*)
- Fixed potential null pointer dereference when setting HDR color space
- Fixed missing mutex cleanup in FFmpeg decoder destructor
- Added proper OpenGL resource cleanup in widget destructor
- Added early return on shader compilation/linking failure
- **Fixed HDR color saturation** - removed aggressive RGB clamping after YUV→RGB conversion that was destroying saturated colors; now only clips invalid negative values

**Performance Optimizations (M1 Mac):**
- Enabled VSync (swap interval = 1) for tear-free rendering and reduced power consumption
- Enabled triple buffering for smoother frame delivery
- VideoToolbox hardware acceleration for H.264/H.265 decoding
- Efficient PBO (Pixel Buffer Objects) for texture uploads

**Technical Changes:**
- Added `detected_pix_fmt` field to decoder struct to track actual frame format
- Added `chiaki_ffmpeg_decoder_is_format_detected()` thread-safe helper function
- Added `data_type` field to PlaneConfig for 8-bit vs 16-bit texture support
- Added macOS HDR helper (`macoshdrhelper.mm`) for native HDR support
- Pixel format detection uses mutex protection for thread safety
- Improved texture upload code with proper stride (row_bytes) comparison
- P010LE shader implements full HDR pipeline: YUV→RGB (no saturation clamp) → PQ EOTF → BT.2020→BT.709 gamut → EDR output
- Added loading indicator UI that hides automatically when video starts
- Added stats overlay with FPS tracking using QElapsedTimer
