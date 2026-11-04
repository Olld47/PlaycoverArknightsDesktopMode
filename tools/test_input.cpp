// Unit tests for the desktop input state machine (src/AKDMInput.cpp).
//
// These run on the host and do not touch the game. They pin down the semantics
// the Unity hooks rely on:
//   * a key press is visible to GetKey for as long as it is held,
//   * GetKeyDown is true for exactly one frame per press,
//   * a press and release inside one frame still reports both,
//   * auto-repeat does not re-trigger GetKeyDown.

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

    printf("\n--- many keys across words ---\n");
    Reset();
    SetKeyState(kKeyLeftShift, true);
    SetKeyState(kKeyZ, true);
    AdvanceFrameNow();
    Check(KeyPressed(kKeyLeftShift) && KeyDown(kKeyZ),
          "keys in different 64-bit words are tracked independently");
    SetKeyState(kKeyLeftShift, false);
    SetKeyState(kKeyZ, false);
    AdvanceFrameNow();
    Check(!KeyPressed(kKeyLeftShift) && !KeyPressed(kKeyZ), "both released");

    printf("\n--- invalid keys ignored ---\n");
    Reset();
    SetKeyState(-1, true);
    SetKeyState(kKeyMax, true);
    AdvanceFrameNow();
    Check(!KeyDown(-1) && !KeyDown(kKeyMax), "out-of-range keys are rejected");

    return failures == 0 ? 0 : 1;
}
