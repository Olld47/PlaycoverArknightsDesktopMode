// Desktop input state shared between the GameController handlers and the
// Unity icall replacements. Lock-free: every field is atomic and the hooks
// only ever read consistent snapshots.

#include "AKDMInput.h"

#include <cmath>
#include <cstring>
#include <ctime>

namespace akdm {
namespace {

// Held state per Unity key code.
std::atomic<uint64_t> gKeyHeld[kKeyWordCount];
// Edge events for the frame currently being observed (GetKeyDown/GetKeyUp).
std::atomic<uint64_t> gKeyDownEdge[kKeyWordCount];
std::atomic<uint64_t> gKeyUpEdge[kKeyWordCount];
// Edge events collected since the last frame boundary, published by
// AdvanceFrame.
std::atomic<uint64_t> gKeyDownPending[kKeyWordCount];
std::atomic<uint64_t> gKeyUpPending[kKeyWordCount];

std::atomic<uint64_t> gMouseHeld{0};
// Edge events are double buffered: hardware handlers set the "pending" words,
// and AdvanceFrame publishes them as "current" for exactly one frame. Nothing
// is lost if several events arrive between two queries.
std::atomic<uint64_t> gMouseDownPending{0};
std::atomic<uint64_t> gMouseUpPending{0};
std::atomic<uint64_t> gMouseDownEdge{0};
std::atomic<uint64_t> gMouseUpEdge{0};

std::atomic<float> gDeltaX{0.0f};
std::atomic<float> gDeltaY{0.0f};
std::atomic<float> gScrollX{0.0f};
std::atomic<float> gScrollY{0.0f};

// Per-frame snapshots. Unity reports the same axis value for every query made
// during one frame, so accumulated motion is latched into a frame snapshot and
// the latch rotates once per Unity frame (see AdvanceFrame).
std::atomic<float> gFrameDeltaX{0.0f};
std::atomic<float> gFrameDeltaY{0.0f};
std::atomic<float> gFrameScrollX{0.0f};
std::atomic<float> gFrameScrollY{0.0f};
std::atomic<uint64_t> gFrameCounter{0};
// True once the current frame's motion snapshot has been published.
std::atomic<bool> gMotionPublished{false};

std::atomic<float> gPointerX{0.0f};
std::atomic<float> gPointerY{0.0f};
std::atomic<bool> gPointerValid{false};
std::atomic<bool> gPointerLocked{false};
std::atomic<bool> gMouseSeen{false};
std::atomic<bool> gPointerVisible{true};
std::atomic<bool> gTouchEmulation{true};
std::atomic<float> gSensitivity{1.0f};

std::atomic<uint64_t> gKeyEvents{0}, gKeyDownEvents{0}, gKeyUpEvents{0};
std::atomic<uint64_t> gButtonEvents{0}, gMoveEvents{0}, gScrollEvents{0};

inline bool ValidKey(int32_t key) { return key >= 0 && key < kKeyMax; }

inline void SetBit(std::atomic<uint64_t> *words, int32_t key, bool value) {
    if (!ValidKey(key)) return;
    const int32_t word = key / 64;
    const uint64_t mask = 1ull << (key % 64);
    if (value)
        words[word].fetch_or(mask);
    else
        words[word].fetch_and(~mask);
}

inline bool TestBit(const std::atomic<uint64_t> *words, int32_t key) {
    if (!ValidKey(key)) return false;
    const int32_t word = key / 64;
    const uint64_t mask = 1ull << (key % 64);
    return (words[word].load() & mask) != 0;
}

// Publishes motion collected since the previous publish into the snapshot that
// queries read. Called from AdvanceFrame and, lazily, from the axis queries.
void PublishMotionFrame() {
    gFrameDeltaX.store(gDeltaX.exchange(0.0f));
    gFrameDeltaY.store(gDeltaY.exchange(0.0f));
    gFrameScrollX.store(gScrollX.exchange(0.0f));
    gFrameScrollY.store(gScrollY.exchange(0.0f));
}

} // namespace

// Publishes pending motion into the snapshot the queries read.
//
// This is idempotent within a frame: once published, the snapshot keeps its
// value until AdvanceFrame starts the next frame, so GetAxis("Mouse X") and
// GetAxis("Mouse Y") always agree. It exists because frame boundaries come from
// Unity's touchCount query, which the game may not issue while the pointer is
// captured; without this the very first motion after capture could be stranded.
void RefreshMotionSnapshot() {
    if (!gMotionPublished.exchange(true)) PublishMotionFrame();
}

void AdvanceFrame() {
    // Unity may query touchCount more than once per frame, and clearing the
    // edge buffers twice would drop presses. One boundary per 8ms is still
    // faster than any display refresh, so a real frame is never missed.
    static std::atomic<int64_t> lastAdvance{0};
    const int64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    int64_t previous = lastAdvance.load();
    if (now - previous < 8 * 1000 * 1000) return;
    if (!lastAdvance.compare_exchange_weak(previous, now)) return;
    AdvanceFrameNow();
}

void AdvanceFrameNow() {
    // Publish the motion collected during the frame that just ended, and let
    // the next query publish again. Every query made during one frame sees the
    // same value, which is how Unity reports axes.
    // Publishing here also marks the new frame as published, so the first query
    // of the frame returns this value rather than republishing an empty
    // accumulator over it.
    PublishMotionFrame();
    gMotionPublished.store(true);
    gFrameCounter.fetch_add(1);
    for (int i = 0; i < kKeyWordCount; ++i) {
        gKeyDownEdge[i].store(gKeyDownPending[i].exchange(0));
        gKeyUpEdge[i].store(gKeyUpPending[i].exchange(0));
    }
    gMouseDownEdge.store(gMouseDownPending.exchange(0));
    gMouseUpEdge.store(gMouseUpPending.exchange(0));
}

void SetKeyState(int32_t unityKey, bool down) {
    if (!ValidKey(unityKey)) return;
    gKeyEvents.fetch_add(1);
    if (down) {
        const bool wasHeld = TestBit(gKeyHeld, unityKey);
        SetBit(gKeyHeld, unityKey, true);
        if (!wasHeld) {
            SetBit(gKeyDownPending, unityKey, true);
            gKeyDownEvents.fetch_add(1);
        }
    } else {
        SetBit(gKeyHeld, unityKey, false);
        SetBit(gKeyUpPending, unityKey, true);
        gKeyUpEvents.fetch_add(1);
    }
}

void SetMouseButtonState(int32_t button, bool down) {
    if (button < 0 || button > 7) return;
    gButtonEvents.fetch_add(1);
    const uint64_t mask = 1ull << button;
    if (down) {
        if ((gMouseHeld.load() & mask) == 0) gMouseDownPending.fetch_or(mask);
        gMouseHeld.fetch_or(mask);
    } else {
        gMouseHeld.fetch_and(~mask);
        gMouseUpPending.fetch_or(mask);
    }
    gMouseSeen.store(true);
}

void AddMouseDelta(float dx, float dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    gMoveEvents.fetch_add(1);
    // Motion proves a mouse exists. Without this, Input.mousePresent would stay
    // false until the first click and every position query would fall through
    // to the game's (empty) implementation.
    gMouseSeen.store(true);
    // New motion invalidates the current frame's snapshot, so the next axis
    // query publishes it even if the game never reaches a frame boundary
    // (touchCount is not queried while the pointer is captured).
    gMotionPublished.store(false);
    // Accumulate with a compare-exchange loop; float atomics have no
    // fetch_add in the standard library.
    auto accumulate = [](std::atomic<float> &target, float value) {
        float current = target.load();
        while (!target.compare_exchange_weak(current, current + value)) {
        }
    };
    const float sensitivity = gSensitivity.load();
    accumulate(gDeltaX, dx * sensitivity);
    accumulate(gDeltaY, dy * sensitivity);
}

void AddScrollDelta(float dx, float dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    gScrollEvents.fetch_add(1);
    auto accumulate = [](std::atomic<float> &target, float value) {
        float current = target.load();
        while (!target.compare_exchange_weak(current, current + value)) {
        }
    };
    accumulate(gScrollX, dx);
    accumulate(gScrollY, dy);
}

void SetPointerPosition(float x, float y, bool valid) {
    if (valid && std::isfinite(x) && std::isfinite(y)) {
        gPointerX.store(x);
        gPointerY.store(y);
        gPointerValid.store(true);
    } else {
        gPointerValid.store(false);
    }
}

void SetPointerLocked(bool locked) { gPointerLocked.store(locked); }

void SetPointerVisible(bool visible) { gPointerVisible.store(visible); }

void SetTouchEmulation(bool enabled) { gTouchEmulation.store(enabled); }

bool TouchEmulationEnabled() { return gTouchEmulation.load(); }

void SetMouseSensitivity(float sensitivity) {
    if (sensitivity > 0.05f && sensitivity < 20.0f) gSensitivity.store(sensitivity);
}

bool PointerVisible() { return gPointerVisible.load(); }

bool KeyDown(int32_t unityKey) {
    if (!ValidKey(unityKey)) return false;
    const int32_t word = unityKey / 64;
    const uint64_t mask = 1ull << (unityKey % 64);
    return (gKeyDownEdge[word].load() & mask) != 0;
}

bool KeyPressed(int32_t unityKey) { return TestBit(gKeyHeld, unityKey); }

bool KeyUp(int32_t unityKey) {
    if (!ValidKey(unityKey)) return false;
    const int32_t word = unityKey / 64;
    const uint64_t mask = 1ull << (unityKey % 64);
    return (gKeyUpEdge[word].load() & mask) != 0;
}

bool MouseButton(int32_t button) {
    if (button < 0 || button > 7) return false;
    return (gMouseHeld.load() & (1ull << button)) != 0;
}

bool MousePresent() { return gMouseSeen.load(); }

bool MouseButtonDown(int32_t button) {
    if (button < 0 || button > 7) return false;
    return (gMouseDownEdge.load() & (1ull << button)) != 0;
}

bool MouseButtonUp(int32_t button) {
    if (button < 0 || button > 7) return false;
    return (gMouseUpEdge.load() & (1ull << button)) != 0;
}

bool PointerPositionValid() { return gPointerValid.load(); }

void MousePosition(float &x, float &y) {
    x = gPointerX.load();
    y = gPointerY.load();
}

void ConsumeScroll(float &x, float &y) {
    RefreshMotionSnapshot();
    x = gFrameScrollX.load();
    y = gFrameScrollY.load();
}

void ConsumeMouseDelta(float &dx, float &dy) {
    // Input.GetAxis("Mouse X") and ("Mouse Y") are separate calls but must see
    // the same value, so this reads the frame snapshot rather than draining a
    // queue.
    //
    // The snapshot is refreshed here as well as in AdvanceFrame: frame
    // boundaries come from Unity's touchCount query, which the game may not
    // call while the pointer is captured, and motion must not be stranded
    // waiting for a frame that never arrives.
    RefreshMotionSnapshot();
    dx = gFrameDeltaX.load();
    dy = gFrameDeltaY.load();
}

MouseAxis AxisFromName(const char *name, size_t length) {
    if (name == nullptr) return MouseAxis::kNone;
    auto equals = [&](const char *literal) {
        const size_t n = std::strlen(literal);
        return n == length && std::memcmp(name, literal, n) == 0;
    };
    if (equals("Mouse X")) return MouseAxis::kX;
    if (equals("Mouse Y")) return MouseAxis::kY;
    if (equals("Mouse ScrollWheel")) return MouseAxis::kScrollY;
    if (equals("Mouse ScrollHorizontal")) return MouseAxis::kScrollX;
    return MouseAxis::kNone;
}

InputCounters SnapshotCounters() {
    InputCounters counters;
    counters.keys = gKeyEvents.load();
    counters.keyDowns = gKeyDownEvents.load();
    counters.keyUps = gKeyUpEvents.load();
    counters.buttons = gButtonEvents.load();
    counters.moves = gMoveEvents.load();
    counters.scrolls = gScrollEvents.load();
    return counters;
}

uint64_t FrameCount() { return gFrameCounter.load(); }

} // namespace akdm
