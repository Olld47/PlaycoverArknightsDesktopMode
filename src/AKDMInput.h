#pragma once
// Desktop input state, fed by GameController and consumed by the Unity icall
// hooks (see AKDMGameHook).
//
// The game reads input through Unity's legacy `Input` API. On iOS no hardware
// keyboard or mouse is ever reported, so every `GetKey`/`GetAxis`/mouse query
// fails even though the game ships full PC key-binding logic. This module
// collects real hardware events from GameController and publishes them as a
// small lock-free snapshot that the hooks can answer from.

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

// Mouse axes reported through Input.GetAxis.
enum class MouseAxis { kNone, kX, kY, kScrollX, kScrollY };

// ---------------------------------------------------------------------------
// Hardware event ingress (called from GameController handlers)
// ---------------------------------------------------------------------------

void SetKeyState(int32_t unityKey, bool down);
void SetMouseButtonState(int32_t button, bool down);
// Relative motion in device units, accumulated until the game consumes it.
// `sensitivity` is applied here so every consumer sees the same scaling.
void AddMouseDelta(float dx, float dy);
void SetMouseSensitivity(float sensitivity);

// When disabled, mouse clicks are not also reported as a synthetic finger.
void SetTouchEmulation(bool enabled);
bool TouchEmulationEnabled();
void AddScrollDelta(float dx, float dy);
// Absolute cursor position in window points, updated by the UIKit layer.
void SetPointerPosition(float x, float y, bool valid);
void SetPointerLocked(bool locked);
// Visibility as requested by the game (UnityEngine.Cursor.set_visible).
void SetPointerVisible(bool visible);
bool PointerVisible();

// ---------------------------------------------------------------------------
// Query surface (called from the Unity icall replacements)
// ---------------------------------------------------------------------------

bool KeyDown(int32_t unityKey);
bool KeyPressed(int32_t unityKey);   // held
bool KeyUp(int32_t unityKey);
bool MouseButton(int32_t button);    // held
bool MouseButtonDown(int32_t button);
bool MouseButtonUp(int32_t button);
bool MousePresent();
// Writes the current absolute mouse position in Unity screen coordinates.
void MousePosition(float &x, float &y);
// True once a position sample has been published by the pointer observer.
bool PointerPositionValid();
// Returns and clears the accumulated scroll delta (Unity reports per-frame).
void ConsumeScroll(float &x, float &y);
// Returns this frame's relative motion, in Unity pixels. The value is stable
// for every query within a frame, matching Unity's own behaviour.
void ConsumeMouseDelta(float &dx, float &dy);

// Publishes pending mouse motion if the snapshot is stale. Called from the axis
// queries so motion never waits for a frame boundary the game may not reach
// while the pointer is captured.
void RefreshMotionSnapshot();

// Marks the start of a new Unity frame so the previous frame's motion and edge
// events are released. Rate limited to one call per 8ms, because Unity may
// query touchCount more than once per frame.
void AdvanceFrame();

// Unconditional frame boundary, for tests and for callers that already know
// they are at a real frame start.
void AdvanceFrameNow();
// Monotonic frame counter, advanced by AdvanceFrameNow. The pointer phase
// machine uses it to advance once per frame instead of once per *query*:
// Unity queries touchCount several times per frame, and a per-query machine
// would skip phases (Began consumed as Moved by the second query).
uint64_t FrameCount();
// Axis lookup by Unity axis name ("Mouse X", "Mouse Y", "Mouse ScrollWheel").
MouseAxis AxisFromName(const char *name, size_t length);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

struct InputCounters {
    uint64_t keys = 0, keyDowns = 0, keyUps = 0;
    uint64_t buttons = 0, moves = 0, scrolls = 0;
};
InputCounters SnapshotCounters();

} // namespace akdm
