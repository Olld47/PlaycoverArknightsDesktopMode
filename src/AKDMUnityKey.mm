// Discovery of Unity's internal UnitySetKeyState.
//
// The function is not exported, so it cannot be found by symbol lookup or
// dlsym. It is reached the same way the reference implementation does it:
// -[UnityView handleCommand:] contains a BL to it, and the callee has a
// recognisable epilogue. Scanning that one method body is bounded and does not
// depend on any fixed offset, so it survives game updates.
//
// This lives in its own translation unit because the discovery needs the
// Objective-C runtime, and the rest of the hook layer is plain C++.

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>

#include "AKDMCompat.h"
#include "AKDMTarget.h"

#include <atomic>

namespace akdm {
namespace {

using SetKeyStateFn = void (*)(int32_t key, bool down);

// The resolved setter lives here so both the discovery and the injection path
// can reach it without crossing translation-unit boundaries.
std::atomic<SetKeyStateFn> gSetKeyState{nullptr};

// The BL target is often a trampoline: a run of `B` instructions that chains to
// the real entry point. Follow it (bounded) so the pattern check runs against
// actual code rather than a jump table.
uintptr_t FollowBranchChain(Target &target, uintptr_t address) {
    for (int depth = 0; depth < 8; ++depth) {
        uint32_t word = 0;
        if (!target.ReadBytes(address, &word, 4)) return address;
        if ((word >> 26) != 0x05u) return address;  // not an unconditional B
        int64_t imm = (int64_t)(word & 0x3ffffff);
        if (imm & (1 << 25)) imm -= (1 << 26);
        address += (uintptr_t)(imm << 2);
    }
    return address;
}

// True when `address` is a function whose prologue saves x19/x20 — the shape
// UnitySetKeyState has, since it keeps the key in x19 and the flag in x20.
static bool HasKeyStatePrologue(Target &target, uintptr_t address) {
    uint32_t words[3] = {};
    if (!target.ReadBytes(address, words, sizeof(words))) return false;
    return words[0] == 0xa9be4ff4u &&  // stp x20, x19, [sp, #-0x20]!
           words[1] == 0xa9017bfdu &&  // stp x29, x30, [sp, #0x10]
           words[2] == 0x910043fdu;    // add x29, sp, #0x10
}

} // namespace

bool InjectKeyStateReady() {
    return gSetKeyState.load(std::memory_order_acquire) != nullptr;
}

uintptr_t FindUnitySetKeyStateImpl(Target &target) {
    Class viewClass = objc_getClass("UnityView");
    if (viewClass == Nil) {
        AKDM_LOG("setkey: UnityView class not found");
        return 0;
    }
    Method method =
        class_getInstanceMethod(viewClass, sel_registerName("handleCommand:"));
    if (method == nullptr) {
        AKDM_LOG("setkey: -[UnityView handleCommand:] not found");
        return 0;
    }
    const IMP imp = method_getImplementation(method);
    Dl_info info = {};
    if (dladdr((void *)imp, &info) == 0 || info.dli_fbase == nullptr) {
        AKDM_LOG("setkey: dladdr failed for handleCommand:");
        return 0;
    }
    const uintptr_t impAddress = (uintptr_t)imp;
    AKDM_LOG("setkey: handleCommand: at 0x%lx", (unsigned long)impAddress);

    // Bounded scan: a real method is far smaller than this, and the bound keeps
    // a malformed header from turning into an unbounded read.
    for (int i = 0; i < 512; ++i) {
        const uintptr_t pc = impAddress + (uintptr_t)i * 4;
        uint32_t word = 0;
        if (!target.ReadBytes(pc, &word, 4)) break;
        if ((word & 0xfc000000u) != 0x94000000u) continue;  // BL
        // Reference algorithm: the call is `SetKeyState(key, true)`, so the
        // preceding instruction is `mov w1, #1`. Requiring it removes the
        // dozens of unrelated objc_msgSend calls in this method.
        uint32_t previous = 0;
        if (!target.ReadBytes(pc - 4, &previous, 4)) continue;
        if (previous != 0x52800021u) continue;
        int64_t imm = (int64_t)(word & 0x3ffffff);
        if (imm & (1 << 25)) imm -= (1 << 26);
        const uintptr_t callee = pc + (uintptr_t)(imm << 2);
        const uintptr_t real = FollowBranchChain(target, callee);
        if (!HasKeyStatePrologue(target, real)) continue;
        gSetKeyState.store((SetKeyStateFn)real, std::memory_order_release);
        AKDM_LOG("setkey: UnitySetKeyState at 0x%lx (BL +0x%x -> trampoline "
                 "0x%lx)",
                 (unsigned long)real, i * 4, (unsigned long)callee);
        return real;
    }
    AKDM_LOG("setkey: no candidate found in handleCommand:");
    return 0;
}

// Unity's input state is only safe to touch from the thread that owns the player
// loop. GameController callbacks arrive on a private queue, so non-main callers
// are marshalled onto the main thread rather than calling straight through.
bool InjectKeyStateImpl(int32_t unityKey, bool down) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            InjectKeyStateImpl(unityKey, down);
        });
        return true;
    }
    SetKeyStateFn fn = gSetKeyState.load(std::memory_order_acquire);
    if (fn == nullptr) return false;
    fn(unityKey, down);
    return true;
}

} // namespace akdm
