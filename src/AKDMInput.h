#pragma once
// Desktop keyboard state, fed by the GameController handler and consumed by
// the Unity icall replacements (see AKDMHook).
//
// The game reads input through Unity's legacy `Input` API. On iOS no hardware
// keyboard is ever reported, so `GetKey`/`GetKeyDown`/`GetKeyUp` always fail
// even though the game ships full PC key-binding logic. This module collects
// real keyboard events from GameController and publishes them as a small
// lock-free snapshot that the hooks can answer from. Mouse and touch input are
// deliberately out of scope: PlayTools converts them into Unity touches.

#include <cstdint>
#include <atomic>

namespace akdm {

// Unity KeyCode values used for the game's default PC bindings. Unity's
// KeyCode is a small enum; these are the values from UnityEngine.KeyCode.
enum UnityKey : int32_t {
    kKeyNone = 0,
    kKeyBackspace = 8,
    kKeyTab = 9,
    kKeyReturn = 13,
    kKeyEscape = 27,
    kKeySpace = 32,
    kKeyAlpha0 = 48,
    kKeyAlpha1 = 49,
    kKeyAlpha2 = 50,
    kKeyAlpha3 = 51,
    kKeyAlpha4 = 52,
    kKeyAlpha5 = 53,
    kKeyAlpha6 = 54,
    kKeyAlpha7 = 55,
    kKeyAlpha8 = 56,
    kKeyAlpha9 = 57,
    kKeyA = 97,
    kKeyB = 98,
    kKeyC = 99,
    kKeyD = 100,
    kKeyE = 101,
    kKeyF = 102,
    kKeyG = 103,
    kKeyH = 104,
    kKeyI = 105,
    kKeyJ = 106,
    kKeyK = 107,
    kKeyL = 108,
    kKeyM = 109,
    kKeyN = 110,
    kKeyO = 111,
    kKeyP = 112,
    kKeyQ = 113,
    kKeyR = 114,
    kKeyS = 115,
    kKeyT = 116,
    kKeyU = 117,
    kKeyV = 118,
    kKeyW = 119,
    kKeyX = 120,
    kKeyY = 121,
    kKeyZ = 122,
    kKeyDelete = 127,
    kKeyUpArrow = 273,
    kKeyDownArrow = 274,
    kKeyRightArrow = 275,
    kKeyLeftArrow = 276,
    kKeyF1 = 282,
    kKeyLeftShift = 304,
    kKeyRightShift = 303,
    kKeyLeftControl = 306,
    kKeyLeftAlt = 308,
    kKeyRightAlt = 307,
    kKeyMouse0 = 323,
    kKeyMouse1 = 324,
    kKeyMouse2 = 325,
};

// Unity KeyCode range we track: 0..kKeyMax.
constexpr int32_t kKeyMax = 512;
constexpr int32_t kKeyWordCount = (kKeyMax + 63) / 64;

// ---------------------------------------------------------------------------
// Hardware event ingress (called from GameController handlers)
// ---------------------------------------------------------------------------

void SetKeyState(int32_t unityKey, bool down);

// ---------------------------------------------------------------------------
// Query surface (called from the Unity icall replacements)
// ---------------------------------------------------------------------------

bool KeyDown(int32_t unityKey);
bool KeyPressed(int32_t unityKey);   // held
bool KeyUp(int32_t unityKey);

// Marks the start of a new Unity frame so the previous frame's key edges are
// released. Rate limited to one call per 8ms, because Unity may query input
// more than once per frame.
void AdvanceFrame();

// Unconditional frame boundary, for tests.
void AdvanceFrameNow();

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

struct InputCounters {
    uint64_t keys = 0, keyDowns = 0, keyUps = 0;
};
InputCounters SnapshotCounters();

} // namespace akdm
