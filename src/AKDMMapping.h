#pragma once
// Pure coordinate arithmetic for the host cursor.
//
// Kept free of Objective-C so it can be unit tested: every regression in the
// cursor mapping so far was an arithmetic or axis error here, and each one cost
// a full manual test round to find.

namespace akdm {

struct CursorGeometry {
    double windowCursorX = 0.0;  // cursor in window coords, origin top-left
    double windowCursorY = 0.0;  //   +Y downwards (UIKit convention)
    double viewXInWindow = 0.0;  // view origin inside the window (UIKit space)
    double viewYInWindow = 0.0;
    double viewWidth = 0.0;
    double viewHeight = 0.0;
    double renderWidth = 0.0;   // Screen.width
    double renderHeight = 0.0;  // Screen.height
};
struct UnityPoint {
    float x = 0.0f;
    float y = 0.0f;
    bool valid = false;
};

// Maps the host cursor into the coordinate space the engine consumes for
// Input.mousePosition. Returns valid=false when the geometry is unusable.
UnityPoint MapCursorToUnity(const CursorGeometry &geometry);

} // namespace akdm
