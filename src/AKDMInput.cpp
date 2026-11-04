// Desktop keyboard state shared between the GameController handler and the
// Unity icall replacements. Lock-free: every field is atomic and the hooks
// only ever read consistent snapshots.
//
// Mouse and touch input are deliberately not modelled here: PlayTools converts
// them into Unity touches natively, and the plugin only supplies the keyboard
// edge that iOS never reports.

#include "AKDMInput.h"

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

std::atomic<uint64_t> gKeyEvents{0}, gKeyDownEvents{0}, gKeyUpEvents{0};

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

} // namespace

void AdvanceFrame() {
    // Unity may query input more than once per frame, and clearing the edge
    // buffers twice would drop presses. One boundary per 8ms is still faster
    // than any display refresh, so a real frame is never missed.
    static std::atomic<int64_t> lastAdvance{0};
    const int64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    int64_t previous = lastAdvance.load();
    if (now - previous < 8 * 1000 * 1000) return;
    if (!lastAdvance.compare_exchange_weak(previous, now)) return;
    AdvanceFrameNow();
}

void AdvanceFrameNow() {
    for (int i = 0; i < kKeyWordCount; ++i) {
        gKeyDownEdge[i].store(gKeyDownPending[i].exchange(0));
        gKeyUpEdge[i].store(gKeyUpPending[i].exchange(0));
    }
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

InputCounters SnapshotCounters() {
    InputCounters counters;
    counters.keys = gKeyEvents.load();
    counters.keyDowns = gKeyDownEvents.load();
    counters.keyUps = gKeyUpEvents.load();
    return counters;
}

} // namespace akdm
