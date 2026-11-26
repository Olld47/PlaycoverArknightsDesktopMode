// Host cursor -> Unity screen pixel mapping. See AKDMMapping.h.

#include "AKDMMapping.h"

namespace akdm {

UnityPoint MapCursorToUnity(const CursorGeometry &geometry) {
    UnityPoint result;
    if (geometry.viewWidth <= 0.0 || geometry.viewHeight <= 0.0 ||
        geometry.renderWidth <= 0.0 || geometry.renderHeight <= 0.0)
        return result;

    // The host cursor arrives in window coordinates (UIKit: origin top-left,
    // +Y downwards), which is the same space UIView uses. Collapse the window
    // onto the view, then scale to the engine's render resolution.
    const double cursorInViewFromLeft =
        geometry.windowCursorX - geometry.viewXInWindow;
    const double cursorInViewFromTop =
        geometry.windowCursorY - geometry.viewYInWindow;

    // Ratio normalisation, matching the reference implementation
    // (UpdateNativePosition in EndPlayDesktop's NativeInput.mm).
    result.x = (float)(cursorInViewFromLeft / geometry.viewWidth *
                       geometry.renderWidth);
    // No flip: measured on this build (host cursor bottom-left -> pointer drawn
    // at the top of the screen), the game consumes mousePosition with y counted
    // DOWN from the top, unlike desktop Unity.
    result.y = (float)(cursorInViewFromTop / geometry.viewHeight *
                       geometry.renderHeight);

    // The content rect is a couple of points smaller than the area the engine
    // samples, so a cursor pinned at an edge can land marginally outside the
    // render bounds (observed: 101.8%). Clamp so the pointer never leaves the
    // surface.
    const float maxX = (float)geometry.renderWidth - 1.0f;
    const float maxY = (float)geometry.renderHeight - 1.0f;
    if (result.x < 0.0f) result.x = 0.0f;
    if (result.y < 0.0f) result.y = 0.0f;
    if (result.x > maxX) result.x = maxX;
    if (result.y > maxY) result.y = maxY;
    result.valid = true;
    return result;
}

} // namespace akdm
