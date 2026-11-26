// Unit tests for the cursor mapping (src/AKDMMapping.cpp).
//
// Cases come from the observations that actually caught bugs, so a regression
// in the same direction fails here instead of costing another manual round:
//   * the host cursor in the window's bottom-left corner must not land at the
//     top of the screen (y was flipped twice);
//   * the mapping must ignore where the window sits and how the view is inset.

#include "../src/AKDMMapping.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void Check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++failures;
}

void CheckNear(float actual, float expected, float tolerance, const char *what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    printf("%s %s (got %.1f, want %.1f ±%.1f)\n", ok ? "  ok  " : "  FAIL",
           what, (double)actual, (double)expected, (double)tolerance);
    if (!ok) ++failures;
}

// The measured geometry: a 1920x1080 point view filling the window, rendering at
// 3840x2160. Window and view origins coincide.
akdm::CursorGeometry FullView() {
    akdm::CursorGeometry g;
    g.viewXInWindow = 0.0;
    g.viewYInWindow = 0.0;
    g.viewWidth = 1920.0;
    g.viewHeight = 1080.0;
    g.renderWidth = 3840.0;
    g.renderHeight = 2160.0;
    return g;
}

} // namespace

int main() {
    using namespace akdm;

    printf("--- window centre ---\n");
    {
        auto g = FullView();
        g.windowCursorX = 960.0;
        g.windowCursorY = 540.0;
        const auto p = MapCursorToUnity(g);
        Check(p.valid, "centre maps successfully");
        CheckNear(p.x, 1920.0f, 1.0f, "centre x is half the render width");
        CheckNear(p.y, 1080.0f, 1.0f, "centre y is half the render height");
    }

    printf("\n--- bottom-left corner (the case that broke twice) ---\n");
    {
        // Measured: host cursor at the window's bottom-left, locationInWindow
        // y = 846 of 1080 (846 points down from the top edge).
        auto g = FullView();
        g.windowCursorX = 15.8;
        g.windowCursorY = 846.0;
        const auto p = MapCursorToUnity(g);
        CheckNear(p.x, 31.6f, 1.0f, "bottom-left maps to a small x");
        // This build consumes y down from the top, so a cursor near the bottom
        // must map to a large y.
        Check(p.y > 1500.0f, "bottom-left maps to a large y (near the bottom)");
    }

    printf("\n--- top-left corner ---\n");
    {
        auto g = FullView();
        g.windowCursorX = 0.0;
        g.windowCursorY = 0.0;
        const auto p = MapCursorToUnity(g);
        CheckNear(p.x, 0.0f, 1.0f, "top-left maps to x=0");
        CheckNear(p.y, 0.0f, 1.0f, "top-left maps to y=0");
    }

    printf("\n--- bottom-right corner ---\n");
    {
        auto g = FullView();
        g.windowCursorX = 1920.0;
        g.windowCursorY = 1080.0;
        const auto p = MapCursorToUnity(g);
        CheckNear(p.x, 3840.0f, 1.0f, "bottom-right maps to full width");
        CheckNear(p.y, 2160.0f, 1.0f, "bottom-right maps to full height");
    }

    printf("\n--- y is not flipped relative to the observation ---\n");
    {
        // Guards the exact failure: a cursor low in the window must not map to
        // the upper half of the render surface.
        auto g = FullView();
        g.windowCursorX = 960.0;
        g.windowCursorY = 900.0;  // low in the window
        const auto p = MapCursorToUnity(g);
        Check(p.y > 1080.0f, "cursor low in the window maps to the lower half");
    }

    printf("\n--- view inset inside the window ---\n");
    {
        // A view that starts 40 points down (title bar above it), height 1000.
        auto g = FullView();
        g.viewYInWindow = 40.0;
        g.viewHeight = 1000.0;
        g.windowCursorX = 960.0;
        g.windowCursorY = 540.0;  // 500 down from the view's top edge
        const auto p = MapCursorToUnity(g);
        CheckNear(p.x, 1920.0f, 1.0f, "inset does not shift x");
        CheckNear(p.y, 1080.0f, 2.0f, "inset view still maps the centre to the middle");
    }

    printf("\n--- results stay inside the render surface ---\n");
    {
        // Observed on device: a cursor pinned beyond the content rect produced
        // 101.8% of the render height. The output must never exceed the surface.
        auto g = FullView();
        g.windowCursorX = 1999.0;   // past the view width
        g.windowCursorY = 1100.0;   // past the view height
        const auto p = MapCursorToUnity(g);
        Check(p.x <= 3840.0f && p.y <= 2160.0f, "clamped to the render bounds");
        Check(p.x >= 0.0f && p.y >= 0.0f, "never negative");

        g.windowCursorX = -50.0;
        g.windowCursorY = -50.0;
        const auto q = MapCursorToUnity(g);
        Check(q.x == 0.0f && q.y == 0.0f, "cursor outside the view clamps to the origin");
    }

    printf("\n--- degenerate geometry is rejected ---\n");
    {
        auto g = FullView();
        g.viewWidth = 0.0;
        Check(!MapCursorToUnity(g).valid, "zero view width is rejected");

        g = FullView();
        g.renderHeight = 0.0;
        Check(!MapCursorToUnity(g).valid, "zero render height is rejected");

        g = FullView();
        g.viewHeight = 0.0;
        Check(!MapCursorToUnity(g).valid, "zero view height is rejected");
    }

    printf("\n%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
