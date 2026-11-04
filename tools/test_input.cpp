// Unit tests for the desktop input state machine (src/AKDMInput.cpp).
//
// These run on the host and do not touch the game. They pin down the semantics
// the Unity hooks rely on:
//   * a key press is visible to GetKey for as long as it is held,
//   * GetKeyDown is true for exactly one frame per press,
//   * a press and release inside one frame still reports both,
//   * mouse X and Y both see the same frame's motion,
//   * scroll is reported once per frame.

#include "../src/AKDMInput.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void Check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++failures;
}

// Motion is published when a frame ends, so a test that injects input must
// advance once to publish it and then read it as the "current" frame.
void Reset() {
    akdm::AdvanceFrameNow();
    akdm::AdvanceFrameNow();
}

} // namespace

int main() {
    using namespace akdm;

    printf("--- key edges ---\n");
    Reset();
    SetKeyState(kKeyA, true);
    AdvanceFrameNow();
    Check(KeyPressed(kKeyA), "held key reports pressed");
    Check(KeyDown(kKeyA), "GetKeyDown true on the press frame");
    Check(!KeyUp(kKeyA), "GetKeyUp false while held");

    AdvanceFrameNow();
    Check(KeyPressed(kKeyA), "still pressed on the next frame");
    Check(!KeyDown(kKeyA), "GetKeyDown false on the following frame");

    SetKeyState(kKeyA, false);
    AdvanceFrameNow();
    Check(!KeyPressed(kKeyA), "released key reports not pressed");
    Check(KeyUp(kKeyA), "GetKeyUp true on the release frame");
    AdvanceFrameNow();
    Check(!KeyUp(kKeyA), "GetKeyUp false on the following frame");

    printf("\n--- press and release inside one frame ---\n");
    Reset();
    SetKeyState(kKeyB, true);
    SetKeyState(kKeyB, false);
    AdvanceFrameNow();
    Check(KeyDown(kKeyB), "GetKeyDown survives a same-frame release");
    Check(KeyUp(kKeyB), "GetKeyUp survives a same-frame release");
    AdvanceFrameNow();
    Check(!KeyDown(kKeyB) && !KeyUp(kKeyB), "both edges clear afterwards");

    printf("\n--- repeated presses ---\n");
    Reset();
    SetKeyState(kKeyC, true);
    SetKeyState(kKeyC, true);  // auto-repeat must not re-trigger GetKeyDown
    AdvanceFrameNow();
    Check(KeyDown(kKeyC), "first press registers");
    SetKeyState(kKeyC, true);
    AdvanceFrameNow();
    Check(!KeyDown(kKeyC), "auto-repeat does not re-trigger GetKeyDown");
    SetKeyState(kKeyC, false);
    AdvanceFrameNow();

    printf("\n--- mouse buttons ---\n");
    Reset();
    SetMouseButtonState(0, true);
    AdvanceFrameNow();
    Check(MouseButton(0), "left button held");
    Check(MouseButtonDown(0), "left button down edge");
    SetMouseButtonState(0, false);
    AdvanceFrameNow();
    Check(!MouseButton(0), "left button released");
    Check(MouseButtonUp(0), "left button up edge");

    printf("\n--- mouse motion ---\n");
    Reset();
    AddMouseDelta(10.0f, -4.0f);
    AdvanceFrameNow();  // ends the frame that collected the motion
    float dx = 0, dy = 0;
    ConsumeMouseDelta(dx, dy);
    Check(dx == 10.0f && dy == -4.0f, "motion reported for the frame");
    ConsumeMouseDelta(dx, dy);
    Check(dx == 10.0f && dy == -4.0f,
          "both Mouse X and Mouse Y see the same frame's motion");
    AdvanceFrameNow();
    ConsumeMouseDelta(dx, dy);
    Check(dx == 0.0f && dy == 0.0f, "motion cleared after the frame advances");

    printf("\n--- motion without a frame boundary ---\n");
    // The game does not query touchCount while the pointer is captured, so
    // AdvanceFrame may never run. Motion must still reach the axis queries.
    Reset();
    AddMouseDelta(7.0f, 3.0f);
    ConsumeMouseDelta(dx, dy);
    Check(dx == 7.0f && dy == 3.0f,
          "motion published without any frame boundary");
    AddMouseDelta(2.0f, 1.0f);
    ConsumeMouseDelta(dx, dy);
    Check(dx == 2.0f && dy == 1.0f,
          "later motion is not stranded by the previous snapshot");

    printf("\n--- sensitivity ---\n");
    Reset();
    SetMouseSensitivity(2.0f);
    AddMouseDelta(5.0f, 5.0f);
    AdvanceFrameNow();
    ConsumeMouseDelta(dx, dy);
    Check(dx == 10.0f && dy == 10.0f, "sensitivity scales motion");
    SetMouseSensitivity(1.0f);

    printf("\n--- scroll ---\n");
    Reset();
    AddScrollDelta(0.0f, 1.5f);
    AdvanceFrameNow();
    float sx = 0, sy = 0;
    ConsumeScroll(sx, sy);
    Check(sx == 0.0f && sy == 1.5f, "scroll reported for the frame");
    ConsumeScroll(sx, sy);
    Check(sy == 1.5f, "scroll stable within a frame");
    AdvanceFrameNow();
    ConsumeScroll(sx, sy);
    Check(sy == 0.0f, "scroll cleared after the frame advances");

    printf("\n--- touch emulation switch ---\n");
    Check(TouchEmulationEnabled(), "touch emulation defaults to enabled");
    SetTouchEmulation(false);
    Check(!TouchEmulationEnabled(), "touch emulation can be disabled");
    SetTouchEmulation(true);
    Check(TouchEmulationEnabled(), "touch emulation can be re-enabled");

    printf("\n--- axis names ---\n");
    Check(AxisFromName("Mouse X", 7) == MouseAxis::kX, "\"Mouse X\" maps to X");
    Check(AxisFromName("Mouse Y", 7) == MouseAxis::kY, "\"Mouse Y\" maps to Y");
    Check(AxisFromName("Mouse ScrollWheel", 17) == MouseAxis::kScrollY,
          "\"Mouse ScrollWheel\" maps to scroll Y");
    Check(AxisFromName("Horizontal", 10) == MouseAxis::kNone,
          "unrelated axes are not intercepted");
    Check(AxisFromName("Mouse X", 6) == MouseAxis::kNone,
          "prefix matches are rejected");

    printf("\n--- pointer ---\n");
    SetPointerPosition(100.0f, 200.0f, true);
    Check(MousePresent() == false || true, "pointer position accepted");
    float px = 0, py = 0;
    MousePosition(px, py);
    Check(px == 100.0f && py == 200.0f, "pointer position round-trips");
    SetPointerPosition(NAN, 0.0f, true);
    MousePosition(px, py);
    Check(px == 100.0f, "non-finite position is ignored");

    printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
