// Pointer position tracking. See AKDMPointer.h.
//
// Unity's `Input.mousePosition` is absolute, but GameController only reports
// relative motion and UIKit's hover gesture does not fire while the pointer is
// captured. The position is therefore polled from the window system.
//
// A Catalyst app still owns an NSWindow, so `NSEvent.mouseLocation` yields the
// absolute cursor position in screen coordinates even while the pointer is
// locked. That is converted into the game's render space (origin bottom-left,
// which matches Unity) and published for the input hooks to read.

#include "AKDMPointer.h"
#include "AKDMCompat.h"
#include "AKDMInput.h"

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

// Render resolution reported by the game; updated from the view when known.
static std::atomic<float> gRenderWidth{0.0f};
static std::atomic<float> gRenderHeight{0.0f};

@interface AKDMHoverTarget : NSObject
@end

@implementation AKDMHoverTarget
- (void)handleHover:(UIHoverGestureRecognizer *)recognizer {
    // Fallback only: publishes a position when the pointer is not captured.
    // The polling path in PollPointerPosition is the primary source.
    UIView *view = recognizer.view;
    if (view == nil) return;
    const CGPoint point = [recognizer locationInView:view];
    const CGRect bounds = view.bounds;
    const float renderWidth = gRenderWidth.load();
    const float renderHeight = gRenderHeight.load();
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return;
    if (renderWidth <= 0.0f || renderHeight <= 0.0f) return;

    const float x = (float)((point.x - bounds.origin.x) * renderWidth /
                            bounds.size.width);
    // UIKit's origin is top-left, Unity's is bottom-left.
    const float y = (float)(renderHeight -
                            (point.y - bounds.origin.y) * renderHeight /
                                bounds.size.height);
    const bool inside = x >= -1.0f && y >= -1.0f && x <= renderWidth + 1.0f &&
                        y <= renderHeight + 1.0f;
    akdm::SetPointerPosition(x, y, inside);
}
@end

namespace akdm {
namespace {

// Depth-first search for the Unity rendering view. The hierarchy is deeper than
// root-plus-one-level once the game adds its own overlay views.
UIView *FindUnityViewIn(UIView *view, int depth) {
    if (view == nil || depth > 8) return nil;
    if ([NSStringFromClass(view.class) containsString:@"UnityView"]) return view;
    for (UIView *child in view.subviews) {
        UIView *found = FindUnityViewIn(child, depth + 1);
        if (found != nil) return found;
    }
    return nil;
}

UIView *FindUnityView() {
    @autoreleasepool {
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            UIWindowScene *windowScene = (UIWindowScene *)scene;
            for (UIWindow *window in windowScene.windows) {
                UIView *found = FindUnityViewIn(window.rootViewController.view, 0);
                if (found != nil) return found;
            }
        }
    }
    return nil;
}

// The window whose geometry the render size should follow.
UIWindow *FindKeyWindow() {
    @autoreleasepool {
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            UIWindowScene *windowScene = (UIWindowScene *)scene;
            for (UIWindow *window in windowScene.windows) {
                if (window.isKeyWindow) return window;
            }
            if (windowScene.windows.count > 0) return windowScene.windows.firstObject;
        }
    }
    return nil;
}

AKDMHoverTarget *HoverTarget() {
    static AKDMHoverTarget *target = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        target = [[AKDMHoverTarget alloc] init];
    });
    return target;
}

} // namespace

void SetRenderSize(float width, float height) {
    if (width <= 0.0f || height <= 0.0f) return;
    if (width == gRenderWidth.load() && height == gRenderHeight.load()) return;
    gRenderWidth.store(width);
    gRenderHeight.store(height);
    AKDM_LOG("render size %.0fx%.0f", width, height);
}

void UpdateRenderSize() {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ UpdateRenderSize(); });
        return;
    }
    @autoreleasepool {
        // Prefer the game's own render size (set from Screen.width/height by the
        // plugin). Only fall back to the window geometry when that is unknown.
        if (gRenderWidth.load() > 0.0f && gRenderHeight.load() > 0.0f) return;
        UIWindow *window = FindKeyWindow();
        if (window == nil) return;
        // The reference implementation scales by Unity's own render width, but
        // reading that requires an IL2CPP call. The window's point size is
        // stable across window rebuilds (unlike contentScaleFactor, which
        // flipped 2.0 -> 1.0 mid-session and made the mapping jump), so the
        // mapping is expressed directly in window points and the render size
        // only supplies the upper bound.
        const CGSize points = window.frame.size;
        if (points.width <= 0 || points.height <= 0) return;

        const float width = (float)points.width;
        const float height = (float)points.height;
        if (width == gRenderWidth.load() && height == gRenderHeight.load()) return;
        gRenderWidth.store(width);
        gRenderHeight.store(height);
        AKDM_LOG("pointer space %.0fx%.0f points", width, height);
    }
}

// Sends a selector only when the receiver actually implements it.
//
// Every AppKit call here is dispatched dynamically because the headers mark
// these members unavailable on Catalyst. Sending an unsupported selector raises
// doesNotRecognizeSelector, which terminates the app, so every send is guarded.
bool SafeResponds(id object, const char *selectorName) {
    if (object == nil) return false;
    SEL selector = sel_registerName(selectorName);
    return [object respondsToSelector:selector];
}

// Reads the cursor position in *window* coordinates, which avoids mixing
// AppKit screen space (bottom-left) with UIKit geometry (top-left).
//
// This mirrors how the reference implementation obtains it:
//   NSApplication.sharedApplication -> valueForKey:@"windows" -> firstObject
//   -> mouseLocationOutsideOfEventStream
// `mouseLocationOutsideOfEventStream` is a method on NSWindow that reports the
// cursor relative to the window's own content, so no origin correction is
// needed. NSEvent.mouseLocation (screen space) would require one.
bool ReadCursorInWindowImpl(NSPoint &point) {
    Class appClass = objc_getClass("NSApplication");
    if (appClass == Nil) return false;
    using IdFn = id (*)(id, SEL);
    if (!SafeResponds((id)appClass, "sharedApplication")) return false;
    id app = ((IdFn)objc_msgSend)((id)appClass,
                                  sel_registerName("sharedApplication"));
    if (app == nil) return false;

    if (!SafeResponds(app, "windows")) return false;
    id windows = ((IdFn)objc_msgSend)(app, sel_registerName("windows"));
    if (windows == nil) return false;
    if (!SafeResponds(windows, "firstObject")) return false;
    id window = ((IdFn)objc_msgSend)(windows, sel_registerName("firstObject"));
    if (window == nil) return false;

    SEL locationSelector = sel_registerName("mouseLocationOutsideOfEventStream");
    if (!SafeResponds(window, "mouseLocationOutsideOfEventStream")) return false;
    using PointFn = NSPoint (*)(id, SEL);
    point = ((PointFn)objc_msgSend)(window, locationSelector);
    return true;
}

// Window geometry in the same space as the cursor: content bounds plus whether
// the window draws a title bar.
struct WindowGeometry {
    CGFloat width = 0.0;    // content size in window points
    CGFloat height = 0.0;
    CGFloat originX = 0.0;  // content origin in window space (usually 0, titleBarHeight)
    CGFloat originY = 0.0;
};

bool ReadWindowGeometryImpl(WindowGeometry &geometry) {
    Class appClass = objc_getClass("NSApplication");
    if (appClass == Nil) return false;
    using IdFn = id (*)(id, SEL);
    if (!SafeResponds((id)appClass, "sharedApplication")) return false;
    id app = ((IdFn)objc_msgSend)((id)appClass,
                                  sel_registerName("sharedApplication"));
    if (app == nil) return false;
    if (!SafeResponds(app, "windows")) return false;
    id windows = ((IdFn)objc_msgSend)(app, sel_registerName("windows"));
    if (windows == nil) return false;
    if (!SafeResponds(windows, "firstObject")) return false;
    id window = ((IdFn)objc_msgSend)(windows, sel_registerName("firstObject"));
    if (window == nil) return false;

    // NSWindow has `frame` but not `bounds` (that is an NSView method), and
    // sending an unsupported selector raises doesNotRecognizeSelector, which
    // terminates the app. Probe first.
    using RectFn = NSRect (*)(id, SEL);
    if (!SafeResponds(window, "contentLayoutRect")) return false;
    // contentLayoutRect is the area below the title bar, expressed in the
    // window's own coordinate space (bottom-left origin). The cursor from
    // mouseLocationOutsideOfEventStream lives in that same space, so
    // subtracting the content origin puts both axes in content coordinates.
    const NSRect content =
        ((RectFn)objc_msgSend)(window, sel_registerName("contentLayoutRect"));
    if (content.size.width <= 0 || content.size.height <= 0) return false;
    geometry.width = content.size.width;
    geometry.height = content.size.height;
    geometry.originX = content.origin.x;
    geometry.originY = content.origin.y;
    return true;
}

// These wrappers make a failure non-fatal: AppKit on Catalyst can raise for
// reasons that cannot be predicted from the headers, and an uncaught exception
// terminates the app. Returning "no position" simply leaves the game's own
// behaviour in place.
bool ReadCursorInWindow(NSPoint &point) {
    @try {
        return ReadCursorInWindowImpl(point);
    } @catch (NSException *exception) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3)
            AKDM_LOG("cursor read raised: %s", exception.reason.UTF8String);
        return false;
    }
}

bool ReadWindowGeometry(WindowGeometry &geometry) {
    @try {
        return ReadWindowGeometryImpl(geometry);
    } @catch (NSException *exception) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3)
            AKDM_LOG("window geometry raised: %s", exception.reason.UTF8String);
        return false;
    }
}

// Converts the cursor position into Unity screen pixels.
//
// Everything happens inside one coordinate system:
//
//   1. `mouseLocationOutsideOfEventStream` gives the cursor relative to the
//      window's *bottom-left*, in window points.
//   2. `contentLayoutRect` gives the content area in the same window space;
//      on Catalyst the game window has a title bar, so the cursor must be
//      measured from the *content* bottom-left, not the window's.
//   3. The content size (points) maps linearly onto Screen.width/height.
//
// MAA's AKInterface (PlayTools' own control bridge) uses exactly this chain
// (locationInWindow + contentLayoutRect + pointPixelScale), and the reference
// plugin reads the cursor from the window for the same reason: screen-space
// coordinates from NSEvent cannot be compared with window-space geometry
// without knowing the screen height, which is unavailable on Catalyst.
bool ComputePointerPosition(float &x, float &y, bool &inside) {
    const float renderWidth = gRenderWidth.load();
    const float renderHeight = gRenderHeight.load();
    if (renderWidth <= 0.0f || renderHeight <= 0.0f) return false;

    WindowGeometry geometry;
    if (!ReadWindowGeometry(geometry)) return false;
    if (geometry.width <= 0 || geometry.height <= 0) return false;

    NSPoint cursor = NSZeroPoint;
    if (!ReadCursorInWindow(cursor)) return false;

    // mouseLocationOutsideOfEventStream measures from the window's bottom-left,
    // including the title bar band. The content starts above that band, so
    // subtract its height; contentLayoutRect already provides the offset.
    const double contentX = cursor.x - geometry.originX;
    const double contentY = cursor.y - geometry.originY;

    const double scaleX = (double)renderWidth / (double)geometry.width;
    const double scaleY = (double)renderHeight / (double)geometry.height;

    x = (float)(contentX * scaleX);
    y = (float)(contentY * scaleY);

    inside = x >= -1.0f && y >= -1.0f && x <= renderWidth + 1.0f &&
             y <= renderHeight + 1.0f;

    // Log whenever the result moves: confirms the mapping is live and shows the
    // exact numbers needed to diagnose a mismatch.
    static std::atomic<int> lastX{INT32_MIN}, lastY{INT32_MIN};
    const int32_t rx = (int32_t)x, ry = (int32_t)y;
    const int32_t px = lastX.exchange(rx);
    const int32_t py = lastY.exchange(ry);
    if (abs(px - rx) > 40 || abs(py - ry) > 40) {
        AKDM_LOG("ptr cursor=(%.0f,%.0f) content=(%.0f,%.0f) "
                 "contentSize=%.0fx%.0f render=%.0fx%.0f -> (%.0f,%.0f)",
                 (double)cursor.x, (double)cursor.y, (double)contentX,
                 (double)contentY, (double)geometry.width,
                 (double)geometry.height, (double)renderWidth,
                 (double)renderHeight, (double)x, (double)y);
    }
    return true;
}

bool ReadPointerPositionNow(float &x, float &y) {
    if (![NSThread isMainThread]) {
        // Reading window geometry off the main thread is not safe. Fall back to
        // the most recent polled sample instead.
        bool valid = false;
        MousePosition(x, y);
        valid = PointerPositionValid();
        return valid;
    }
    @autoreleasepool {
        bool inside = false;
        if (!ComputePointerPosition(x, y, inside)) return false;
        return inside;
    }
}

void PollPointerPosition() {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ PollPointerPosition(); });
        return;
    }
    @autoreleasepool {
        float x = 0.0f, y = 0.0f;
        bool inside = false;
        if (!ComputePointerPosition(x, y, inside)) return;
        SetPointerPosition(x, y, inside);

        // Log the first few samples: evidence that polling produces positions.
        static std::atomic<int> logged{0};
        if (inside && logged.fetch_add(1) < 5) {
            AKDM_LOG("pointer poll render=(%.0f,%.0f) window=%.0fx%.0f",
                     (double)x, (double)y, (double)FindKeyWindow().frame.size.width,
                     (double)FindKeyWindow().frame.size.height);
        }
    }
}

void AttachPointerObserver() {
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            UIView *view = FindUnityView();
            if (view == nil) {
                AKDM_LOG("pointer observer: Unity view not found yet");
                return;
            }
            if (objc_getAssociatedObject(view, "akdm.hover") != nil) return;

            UIHoverGestureRecognizer *hover = [[UIHoverGestureRecognizer alloc]
                initWithTarget:HoverTarget()
                        action:@selector(handleHover:)];
            hover.cancelsTouchesInView = NO;
            hover.delaysTouchesBegan = NO;
            hover.delaysTouchesEnded = NO;
            [view addGestureRecognizer:hover];
            objc_setAssociatedObject(view, "akdm.hover", hover,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);

            UpdateRenderSize();
            AKDM_LOG("pointer observer attached to %s",
                     NSStringFromClass(view.class).UTF8String);
        }
    });
}

} // namespace akdm
