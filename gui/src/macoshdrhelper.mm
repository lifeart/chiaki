// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "macoshdrhelper.h"

#ifdef Q_OS_MACOS
#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>

// Helper to run a block on the main thread synchronously
// NSScreen access must be done from the main thread
static void runOnMainThread(void (^block)(void))
{
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

bool macosIsHDRSupported()
{
    __block bool supported = false;
    runOnMainThread(^{
        // Check if the main screen supports EDR (Extended Dynamic Range)
        NSScreen *mainScreen = [NSScreen mainScreen];
        if (@available(macOS 10.15, *)) {
            // Check multiple EDR values:
            // - maximumExtendedDynamicRangeColorComponentValue: current EDR headroom
            // - maximumPotentialExtendedDynamicRangeColorComponentValue: max possible EDR (requires HDR content)
            // - maximumReferenceExtendedDynamicRangeColorComponentValue: reference white EDR
            CGFloat maxEDR = mainScreen.maximumExtendedDynamicRangeColorComponentValue;
            CGFloat potentialEDR = mainScreen.maximumPotentialExtendedDynamicRangeColorComponentValue;
            CGFloat refEDR = mainScreen.maximumReferenceExtendedDynamicRangeColorComponentValue;
            NSLog(@"Chiaki HDR: maxEDR=%.2f, potentialEDR=%.2f, refEDR=%.2f",
                  maxEDR, potentialEDR, refEDR);
            // Display supports HDR if potential EDR > 1.0 (even if current is 1.0)
            supported = (potentialEDR > 1.0) || (maxEDR > 1.0);
        }
    });
    return supported;
}

float macosGetMaxEDR()
{
    __block float maxEDR = 1.0f;
    runOnMainThread(^{
        NSScreen *mainScreen = [NSScreen mainScreen];
        if (@available(macOS 10.15, *)) {
            // Try potential EDR first (what the display can do), then current max
            CGFloat potential = mainScreen.maximumPotentialExtendedDynamicRangeColorComponentValue;
            CGFloat current = mainScreen.maximumExtendedDynamicRangeColorComponentValue;
            // Use potential if available, otherwise current
            maxEDR = (float)(potential > 1.0 ? potential : current);
            NSLog(@"Chiaki: Using maxEDR=%.2f (potential=%.2f, current=%.2f)",
                  maxEDR, potential, current);
        }
    });
    return maxEDR;
}

void macosSetWindowColorSpace(void *nsViewPtr, bool hdr)
{
    if (!nsViewPtr)
        return;

    runOnMainThread(^{
        // Note: QWindow::winId() returns NSView* on macOS, not NSWindow*
        NSView *view = (__bridge NSView *)nsViewPtr;
        NSWindow *window = view.window;

        if (!window)
            return;

        if (@available(macOS 10.12, *)) {
            if (hdr) {
                // Use extended sRGB for HDR content
                window.colorSpace = [NSColorSpace extendedSRGBColorSpace];
            } else {
                window.colorSpace = [NSColorSpace sRGBColorSpace];
            }
        }
    });
}

void macosEnableEDR(void *nsViewPtr)
{
    if (!nsViewPtr)
        return;

    runOnMainThread(^{
        NSView *view = (__bridge NSView *)nsViewPtr;

        // Enable Extended Dynamic Range for OpenGL surface
        if (@available(macOS 10.11, *)) {
            if ([view respondsToSelector:@selector(setWantsExtendedDynamicRangeOpenGLSurface:)]) {
                [(id)view setWantsExtendedDynamicRangeOpenGLSurface:YES];
                NSLog(@"Chiaki: Enabled wantsExtendedDynamicRangeOpenGLSurface on view");
            }
        }

        // Also enable on the window if possible
        NSWindow *window = view.window;
        if (window) {
            // Set window to extended sRGB color space for HDR
            if (@available(macOS 10.12, *)) {
                window.colorSpace = [NSColorSpace extendedSRGBColorSpace];
                NSLog(@"Chiaki: Set window colorSpace to extendedSRGBColorSpace");
            }

            // For layer-backed views, enable EDR on the layer
            if (view.layer) {
                if (@available(macOS 10.11, *)) {
                    view.layer.wantsExtendedDynamicRangeContent = YES;
                    NSLog(@"Chiaki: Enabled wantsExtendedDynamicRangeContent on layer");
                }
            }
        }
    });
}

#else
// Stub implementations for non-macOS platforms
bool macosIsHDRSupported() { return false; }
float macosGetMaxEDR() { return 1.0f; }
void macosSetWindowColorSpace(void *, bool) {}
void macosEnableEDR(void *) {}
#endif
