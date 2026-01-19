// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_MACOSHDRHELPER_H
#define CHIAKI_MACOSHDRHELPER_H

#ifdef __cplusplus
extern "C" {
#endif

// Check if the current display supports HDR (EDR on macOS)
bool macosIsHDRSupported();

// Get the maximum EDR value supported by the display (>1.0 for HDR displays)
float macosGetMaxEDR();

// Set the window's color space for HDR or SDR content
// Note: Pass NSView* (from QWindow::winId()) - the function will get NSWindow from it
void macosSetWindowColorSpace(void *nsViewPtr, bool hdr);

// Enable Extended Dynamic Range (EDR) for OpenGL surface
// This allows values > 1.0 to be displayed as HDR
void macosEnableEDR(void *nsViewPtr);

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_MACOSHDRHELPER_H
