// Host cursor tracking.
//
// The game draws its own pointer in PC mode from Input.mousePosition. On iOS
// that value only changes when a touch is synthesised, so the in-game cursor
// would move only on click. This polls the real host cursor instead and
// publishes it in Unity screen pixels, which the get_mousePosition hook returns.
//
// Unity's screen space has the origin at the bottom-left of the render surface,
// which matches AppKit's screen coordinates, so the mapping is a translation by
// the window origin followed by a scale from window points to render pixels.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include <cmath>

#include "AKDMCompat.h"
#include "AKDMMapping.h"
#include "AKDMInput.h"

namespace akdm {
namespace {

// Render resolution the game reports. Falls back to the view's backing size
// until Screen.width/height has been observed.
std::atomic<float> gRenderWidth{0.0f};
std::atomic<float> gRenderHeight{0.0f};

// Guarded selector send: AppKit on Catalyst raises for selectors the headers
// mark unavailable, and an uncaught exception terminates the app.
bool SafeResponds(id object, const char *name) {
    if (object == nil) return false;
    return [object respondsToSelector:sel_registerName(name)];
}

// Depth-first search for Unity's render view.
UIView *FindUnityViewIn(UIView *view, int depth) {
    if (view == nil || depth > 8) return nil;
    if ([NSStringFromClass(view.class) containsString:@"UnityView"]) return view;
    for (UIView *child in view.subviews) {
        UIView *found = FindUnityViewIn(child, depth + 1);
        if (found != nil) return found;
    }
    return nil;
}

UIView *UnityView() {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            UIView *found = FindUnityViewIn(window.rootViewController.view, 0);
            if (found != nil) return found;
        }
    }
    return nil;
}

UIWindow *KeyWindow() {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        UIWindowScene *windowScene = (UIWindowScene *)scene;
        for (UIWindow *window in windowScene.windows) {
            if (window.isKeyWindow) return window;
        }
        if (windowScene.windows.count > 0) return windowScene.windows.firstObject;
    }
    return nil;
}

// PlayTools' AKInterface already exposes exactly the geometry needed here:
// mousePoint (AppKit screen space), windowFrame and pointPixelScale. Using it
// avoids re-deriving the point/pixel relationship, which is where the previous
// hand-rolled conversion drifted.
id AKPluginShared() {
    Class pluginClass = objc_getClass("AKInterface.AKPlugin");
    if (pluginClass == Nil) {
        // Report once: if AKInterface is not in this process the local
        // computation below is the only path, which is worth knowing.
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            AKDM_LOG("cursor: AKInterface.AKPlugin not loaded; using local mapping");
        return nil;
    }
    if (!SafeResponds((id)pluginClass, "shared")) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            AKDM_LOG("cursor: AKPlugin has no +shared; using local mapping");
        return nil;
    }
    using IdFn = id (*)(id, SEL);
    id shared = ((IdFn)objc_msgSend)((id)pluginClass, sel_registerName("shared"));
    return shared != nil && SafeResponds(shared, "mousePoint") ? shared : nil;
}

// Screen point -> Unity pixels using AKInterface's own scale factor.
bool ReadCursorViaAKInterface(double &x, double &y) {
    id plugin = AKPluginShared();
    if (plugin == nil) return false;
    using PointFn = CGPoint (*)(id, SEL);
    using RectFn = CGRect (*)(id, SEL);
    using DoubleFn = double (*)(id, SEL);

    const CGPoint mouse =
        ((PointFn)objc_msgSend)(plugin, sel_registerName("mousePoint"));
    CGRect window = {{0, 0}, {0, 0}};
    if (SafeResponds(plugin, "windowFrame"))
        window = ((RectFn)objc_msgSend)(plugin, sel_registerName("windowFrame"));
    if (window.size.width <= 0 || window.size.height <= 0) return false;

    double scale = 0.0;
    if (SafeResponds(plugin, "pointPixelScale"))
        scale = ((DoubleFn)objc_msgSend)(plugin, sel_registerName("pointPixelScale"));
    if (!(scale > 0.0)) return false;

    // mousePoint and windowFrame share AppKit screen space with the origin at
    // the bottom-left, so a plain translation puts the cursor inside the window.
    x = (mouse.x - window.origin.x) * scale;
    y = (mouse.y - window.origin.y) * scale;
    return true;
}

// Reads the absolute cursor position in AppKit screen coordinates.
// Most recent cursor position observed in a *real* mouse event delivered to this
// application.
//
// Polling +[NSEvent mouseLocation] was unreliable here: it reported positions
// that did not match where the user had actually put the pointer, and the value
// it returned was consumed as if it were authoritative. The reference
// implementation never polls — it tracks the events the app receives
// (HostPointer.mm uses addLocalMonitorForEventsMatchingMask:). Same approach
// here.
std::atomic<double> gEventX{0.0};
std::atomic<double> gEventY{0.0};
std::atomic<bool> gEventSeen{false};
std::atomic<bool> gMonitorInstalled{false};

void InstallMouseMonitor() {
    if (gMonitorInstalled.exchange(true)) return;
    Class eventClass = objc_getClass("NSEvent");
    if (eventClass == Nil) return;
    SEL addSelector =
        sel_registerName("addLocalMonitorForEventsMatchingMask:handler:");
    if (!SafeResponds((id)eventClass,
                      "addLocalMonitorForEventsMatchingMask:handler:")) {
        AKDM_LOG("cursor: local event monitor unavailable");
        return;
    }
    // Mouse-moved plus the drag variants, so dragging keeps the pointer live.
    constexpr uint64_t kMouseMoved = 1ull << 5;
    constexpr uint64_t kLeftDrag = 1ull << 6;
    constexpr uint64_t kRightDrag = 1ull << 7;
    constexpr uint64_t kOtherDrag = 1ull << 27;
    const uint64_t mask = kMouseMoved | kLeftDrag | kRightDrag | kOtherDrag;

    using AddFn = id (*)(id, SEL, uint64_t, id (^)(id));
    using PointFn = NSPoint (*)(id, SEL);
    id monitor = ((AddFn)objc_msgSend)(
        (id)eventClass, addSelector, mask, ^id(id event) {
            if (event != nil && SafeResponds(event, "locationInWindow")) {
                const NSPoint point = ((PointFn)objc_msgSend)(
                    event, sel_registerName("locationInWindow"));
                if (std::isfinite(point.x) && std::isfinite(point.y)) {
                    gEventX.store(point.x, std::memory_order_relaxed);
                    gEventY.store(point.y, std::memory_order_relaxed);
                    gEventSeen.store(true, std::memory_order_release);
                }
            }
            return event;
        });
    if (monitor == nil) {
        AKDM_LOG("cursor: local event monitor not installed");
        return;
    }
    // The monitor must outlive this call; it is never removed, which matches the
    // process lifetime.
    CFRetain((__bridge CFTypeRef)monitor);
    AKDM_LOG("cursor: local mouse monitor installed");
}

// Cursor in window coordinates, from the last delivered mouse event.
bool ReadWindowCursor(double &x, double &y) {
    InstallMouseMonitor();
    if (!gEventSeen.load(std::memory_order_acquire)) return false;
    x = gEventX.load(std::memory_order_relaxed);
    y = gEventY.load(std::memory_order_relaxed);
    return true;
}

} // namespace

namespace {

// Cached geometry, refreshed only when it actually changes. Resolving the window
// and reading its frame on every query would put multiple objc_msgSend calls on
// the Unity player loop, which runs on the main thread.
struct CursorMapping {
    double originX = 0.0;
    double originY = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    bool valid = false;
};
CursorMapping gMapping;
float gViewWidth = 0.0f;
float gViewHeight = 0.0f;

double ScreenHeight();

// Height of the main screen in AppKit points. Needed to convert UIKit's
// top-left window origin into AppKit's bottom-left space.
double ScreenHeight() {
    Class screenClass = objc_getClass("NSScreen");
    if (screenClass == Nil || !SafeResponds((id)screenClass, "mainScreen"))
        return 0.0;
    using IdFn = id (*)(id, SEL);
    id screen = ((IdFn)objc_msgSend)((id)screenClass, sel_registerName("mainScreen"));
    if (screen == nil || !SafeResponds(screen, "frame")) return 0.0;
    using RectFn = NSRect (*)(id, SEL);
    const NSRect rect = ((RectFn)objc_msgSend)(screen, sel_registerName("frame"));
    return rect.size.height;
}

// Reads the cursor in window coordinates the way MaaTools does:
//   NSApplication.shared.windows.first.mouseLocationOutsideOfEventStream
// That API reports the position inside the window without going through UIKit's
// view scaling, which is what made the previous mappings under-report.
// The window's content rect (frame minus title bar), in AppKit screen space.
bool ReadWindowContentRect(CGRect &content) {
    Class appClass = objc_getClass("NSApplication");
    if (appClass == Nil || !SafeResponds((id)appClass, "sharedApplication"))
        return false;
    using IdFn = id (*)(id, SEL);
    using RectFn = CGRect (*)(id, SEL);
    id app = ((IdFn)objc_msgSend)((id)appClass, sel_registerName("sharedApplication"));
    if (app == nil || !SafeResponds(app, "windows")) return false;
    id windows = ((IdFn)objc_msgSend)(app, sel_registerName("windows"));
    if (windows == nil || !SafeResponds(windows, "firstObject")) return false;
    id window = ((IdFn)objc_msgSend)(windows, sel_registerName("firstObject"));
    if (window == nil || !SafeResponds(window, "frame")) return false;
    const CGRect frame = ((RectFn)objc_msgSend)(window, sel_registerName("frame"));
    if (SafeResponds(window, "contentRectForFrameRect:")) {
        using RectArgFn = CGRect (*)(id, SEL, CGRect);
        const CGRect rect = ((RectArgFn)objc_msgSend)(
            window, sel_registerName("contentRectForFrameRect:"), frame);
        if (rect.size.width > 0 && rect.size.height > 0) {
            content = rect;
            return true;
        }
    }
    content = frame;
    return frame.size.width > 0 && frame.size.height > 0;
}

bool ReadWindowCursorAppKit(double &x, double &y) {
    Class appClass = objc_getClass("NSApplication");
    if (appClass == Nil || !SafeResponds((id)appClass, "sharedApplication"))
        return false;
    using IdFn = id (*)(id, SEL);
    using PointFn = NSPoint (*)(id, SEL);
    id app = ((IdFn)objc_msgSend)((id)appClass, sel_registerName("sharedApplication"));
    if (app == nil || !SafeResponds(app, "windows")) return false;
    id windows = ((IdFn)objc_msgSend)(app, sel_registerName("windows"));
    if (windows == nil || !SafeResponds(windows, "firstObject")) return false;
    id window = ((IdFn)objc_msgSend)(windows, sel_registerName("firstObject"));
    if (window == nil) return false;
    if (!SafeResponds(window, "mouseLocationOutsideOfEventStream")) return false;
    const NSPoint point =
        ((PointFn)objc_msgSend)(window, sel_registerName("mouseLocationOutsideOfEventStream"));
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    x = point.x;
    y = point.y;
    return true;
}

bool ReadCursorNow(float &x, float &y) {
    double windowX = 0.0, windowY = 0.0;
    // Prefer MaaTools' source; fall back to the monitored event if AppKit is
    // unavailable for some reason.
    if (!ReadWindowCursorAppKit(windowX, windowY) &&
        !ReadWindowCursor(windowX, windowY))
        return false;

    UIView *view = UnityView();
    if (view == nil) return false;

    const CGSize bounds = view.bounds.size;
    const CGSize frameSize = view.frame.size;
    if (bounds.width <= 0 || bounds.height <= 0) return false;
    if (frameSize.width <= 0 || frameSize.height <= 0) return false;

    // The window is laid out in points while the view is laid out in its own
    // units; PlayCover's scale factor sits between them. Let UIKit convert, so
    // that factor is applied once by the framework that owns it.
    const CGPoint inView =
        [view convertPoint:CGPointMake(windowX, windowY) fromView:nil];

    const float renderWidth =
        gRenderWidth.load() > 0.0f ? gRenderWidth.load() : (float)bounds.width;
    const float renderHeight =
        gRenderHeight.load() > 0.0f ? gRenderHeight.load() : (float)bounds.height;

    // Measured: the cursor at the window's right edge reports x = 1475, and the
    // window's content rect is 1478x831. So mouseLocationOutsideOfEventStream is
    // measured against the *content area*, not the 1920x1080 window. Dividing by
    // the window size is what capped the mapping at ~76.8%. MaaTools exposes
    // contentRect for exactly this reason.
    CGRect content = {{0, 0}, {0, 0}};
    const bool haveContent = ReadWindowContentRect(content);
    const double basisWidth =
        haveContent && content.size.width > 0 ? content.size.width : frameSize.width;
    const double basisHeight =
        haveContent && content.size.height > 0 ? content.size.height : frameSize.height;

    CursorGeometry geometry;
    geometry.windowCursorX = inView.x;
    geometry.windowCursorY = inView.y;
    geometry.viewWidth = basisWidth;
    geometry.viewHeight = basisHeight;
    geometry.renderWidth = renderWidth;
    geometry.renderHeight = renderHeight;

    const UnityPoint mapped = MapCursorToUnity(geometry);
    if (!mapped.valid) return false;
    x = mapped.x;
    y = mapped.y;

    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) % 30 == 0) {
        // Everything that feeds the coordinate decision, printed together so a
        // single log line can be solved instead of requiring another round.
        UIWindow *uiWindow = view.window;
        CGRect uiFrame = {{0, 0}, {0, 0}};
        CGRect uiBounds = {{0, 0}, {0, 0}};
        if (uiWindow != nil) {
            uiFrame = uiWindow.frame;
            uiBounds = uiWindow.bounds;
        }
        CGRect content = {{0, 0}, {0, 0}};
        const bool haveContent = ReadWindowContentRect(content);

        // The basis the mapping actually divided by, and the resulting
        // percentages: without these two the log cannot show whether the
        // division used the content rect or the window frame.
        const double basisW = geometry.viewWidth;
        const double basisH = geometry.viewHeight;

        AKDM_LOG("CURSOR raw=(%.1f,%.1f) | uiWin.frame=%.0fx%.0f@(%.0f,%.0f) "
                 "uiWin.bounds=%.0fx%.0f view.frame=%.0fx%.0f@(%.0f,%.0f) "
                 "view.bounds=%.0fx%.0f scale=%.2f content=%s%.0fx%.0f@(%.0f,%.0f) "
                 "| inView=(%.1f,%.1f) BASIS=%.0fx%.0f "
                 "PCT=%.1f%%/%.1f%% render=%.0fx%.0f | OUT unity=(%.1f,%.1f)",
                 windowX, windowY, (double)uiFrame.size.width,
                 (double)uiFrame.size.height, (double)uiFrame.origin.x,
                 (double)uiFrame.origin.y, (double)uiBounds.size.width,
                 (double)uiBounds.size.height, (double)view.frame.size.width,
                 (double)view.frame.size.height, (double)view.frame.origin.x,
                 (double)view.frame.origin.y, (double)bounds.width,
                 (double)bounds.height, (double)view.contentScaleFactor,
                 haveContent ? "" : "?", (double)content.size.width,
                 (double)content.size.height, (double)content.origin.x,
                 (double)content.origin.y, (double)inView.x, (double)inView.y,
                 basisW, basisH,
                 basisW > 0 ? (double)inView.x / basisW * 100.0 : 0.0,
                 basisH > 0 ? (double)inView.y / basisH * 100.0 : 0.0,
                 (double)renderWidth, (double)renderHeight, (double)x, (double)y);
    }
    return true;
}

} // namespace

// The two axes arrive from separate engine queries, so each is stored on its
// own. Requiring both at once silently dropped the first update.
void SetRenderWidth(float width) {
    if (width <= 0.0f) return;
    if (gRenderWidth.exchange(width) == width) return;
    AKDM_LOG("render width %.0f (height %.0f)", (double)width,
             (double)gRenderHeight.load());
    gMapping.valid = false;
}

void SetRenderHeight(float height) {
    if (height <= 0.0f) return;
    if (gRenderHeight.exchange(height) == height) return;
    AKDM_LOG("render height %.0f (width %.0f)", (double)height,
             (double)gRenderWidth.load());
    gMapping.valid = false;
}


void DescribeCursorMapping(float &viewWidth, float &viewHeight,
                           float &originX, float &originY, float &scaleX,
                           float &scaleY) {
    viewWidth = gViewWidth;
    viewHeight = gViewHeight;
    originX = (float)gMapping.originX;
    originY = (float)gMapping.originY;
    scaleX = (float)gMapping.scaleX;
    scaleY = (float)gMapping.scaleY;
}

// Exposes the raw cursor in window coordinates, for diagnostics.
void GetCursorScreenPosition(float &x, float &y) {
    double windowX = 0.0, windowY = 0.0;
    if (ReadWindowCursor(windowX, windowY)) {
        x = (float)windowX;
        y = (float)windowY;
    } else {
        x = y = -1.0f;
    }
}

void RefreshCursorMapping() {
    // Nothing is cached any more: every query recomputes from the live cursor,
    // which is what keeps the position accurate. Kept as a no-op so the
    // maintenance timer does not need to care.
}

// Reads the cursor position synchronously on the calling thread. The Unity
// queries call this so the value they return is current, not a polled sample.
bool ReadCursorPosition(float &x, float &y) {
    @autoreleasepool {
        return ReadCursorNow(x, y);
    }
}

} // namespace akdm
