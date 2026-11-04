// Unity input interception. See AKDMHook.h for the mechanism.

#include "AKDMHook.h"
#include "AKDMCompat.h"
#include "AKDMHardware.h"
#include "AKDMInput.h"
#include "AKDMTarget.h"

#include <cstring>
#include <mutex>

namespace akdm {
namespace {

// Il2CppString:// Il2CppString: { void *klass; void *monitor; int32_t length; char16_t chars[]; }
constexpr size_t kStringLengthOffset = 16;
constexpr size_t kStringCharsOffset = 20;

// ---------------------------------------------------------------------------
// Lazy resolution of the game's own icalls
// ---------------------------------------------------------------------------

// The wrapper's resolver: `void *il2cpp_resolve_icall(const char *name)`.
using ResolveIcallFn = void *(*)(const char *);

// Target reports runtime addresses (its segments are pre-shifted by the load
// bias), so nothing here adds an offset. Kept for logging only.
uintptr_t gSlide = 0;

struct IcallTarget {
    ResolveIcallFn resolve = nullptr;   // shared by every input wrapper
    uintptr_t name = 0;                 // address of the icall name string
    std::atomic<void *> cached{nullptr};
    uintptr_t slot = 0;                 // runtime address we overwrote (0 = none)
    uintptr_t previous = 0;             // value that was there before
};


// Recovered from the validator's FindIcallSlot mirror: decodes the wrapper's
// adrp/ldr (slot), cbnz (guard) and adrp/add (icall name string) instruction
// sequence and returns the runtime addresses of the slot and the name.
struct SlotInfo {
    uintptr_t slot = 0;
    uintptr_t name = 0;
    ResolveIcallFn resolve = nullptr;
};

bool FindSlot(Target &target, uintptr_t code, SlotInfo &info) {
    for (int i = 0; i < 24; ++i) {
        const uintptr_t pc = code + (uintptr_t)i * 4;
        uint32_t word = 0;
        if (!target.ReadBytes(pc, &word, 4)) return false;
        if ((word & 0x9f000000u) != 0x90000000u) continue;
        const uint32_t rd = word & 0x1f;
        const uint32_t immlo = (word >> 29) & 0x3;
        const uint32_t immhi = (word >> 5) & 0x7ffff;
        int64_t imm = (int64_t)((immhi << 2) | immlo);
        if (imm & (1 << 20)) imm -= (1 << 21);
        const uintptr_t page = (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
        uint32_t load = 0;
        if (!target.ReadBytes(pc + 4, &load, 4)) return false;
        if ((load & 0xffc00000u) != 0xf9400000u) continue;
        if (((load >> 5) & 0x1f) != rd) continue;
        const uintptr_t candidate = page + (uintptr_t)((load >> 10) & 0xfff) * 8;
        uint32_t guard = 0;
        if (!target.ReadBytes(pc + 8, &guard, 4)) return false;
        if ((guard & 0xff000000u) != 0xb5000000u) continue;

        uintptr_t nameAddress = 0;
        for (int j = i + 2; j < i + 10; ++j) {
            const uintptr_t cursor = code + (uintptr_t)j * 4;
            uint32_t a = 0, b = 0;
            if (!target.ReadBytes(cursor, &a, 4)) break;
            if (!target.ReadBytes(cursor + 4, &b, 4)) break;
            if ((a & 0x9f000000u) != 0x90000000u) continue;
            const uint32_t nrd = a & 0x1f;
            if ((b & 0xffc00000u) != 0x91000000u) continue;
            if (((b >> 5) & 0x1f) != nrd) continue;
            const uint32_t nlo = (a >> 29) & 0x3;
            const uint32_t nhi = (a >> 5) & 0x7ffff;
            int64_t nimm = (int64_t)((nhi << 2) | nlo);
            if (nimm & (1 << 20)) nimm -= (1 << 21);
            nameAddress = (cursor & ~(uintptr_t)0xfff) +
                          (uintptr_t)(nimm << 12) + (uintptr_t)((b >> 10) & 0xfff);
            break;
        }
        if (nameAddress == 0) continue;
        info.slot = candidate;
        info.name = nameAddress;
        return true;
    }
    return false;
}

IcallTarget gGetKey, gGetKeyDown, gGetKeyUp;

// Restores every slot this module replaced. Used when installation fails, so
// the game is never left with a partially wired input path.
void UndoSlots() {    IcallTarget *all[] = {&gGetKey, &gGetKeyDown, &gGetKeyUp};
    for (IcallTarget *target : all) {
        if (target->slot == 0) continue;
        WritePointer(target->slot, target->previous);
        AKDM_LOG("hook rollback: slot 0x%lx restored to %p",
                 (unsigned long)target->slot, (void *)target->previous);
        target->slot = 0;
    }
}


// Returns the game's own icall implementation captured from the slot before we
// replaced it.
//
// The plugin deliberately does NOT call il2cpp_resolve_icall here: that enters
// the IL2CPP runtime from whichever thread happens to be asking, which is not
// safe from an input hook and previously produced a pointer-authentication
// crash. Because the Unity wrappers resolve lazily, a slot that the game had
// already used holds the real implementation and we captured it at install
// time. When nothing was captured the replacement simply reports "no input"
// instead of risking a fault.
void *ResolveOriginal(IcallTarget &target) {
    return target.cached.load();
}

template <typename Fn>
inline Fn Original(IcallTarget &target) {
    return (Fn)ResolveOriginal(target);
}

std::atomic<bool> gActive{false};
std::atomic<void (*)()> gFirstCallAction{nullptr};
std::atomic<bool> gFirstCallDone{false};

inline void RunFirstCallAction() {
    if (gFirstCallDone.load()) return;
    auto action = gFirstCallAction.load();
    if (action == nullptr) return;
    if (gFirstCallDone.exchange(true)) return;
    action();
}

// ---------------------------------------------------------------------------
// Virtual pointer
// ---------------------------------------------------------------------------

// The pointer phase machine and the mouse/touch replacement functions lived
// here and were removed: PlayTools converts mouse input into Unity touches
// natively, and hooking those queries only competed with that pipeline.


using GetKeyFn = bool (*)(int32_t);

bool ReplacementGetKey(int32_t key) {
    RunFirstCallAction();
    if (gActive.load() && KeyPressed(key)) return true;
    GetKeyFn original = Original<GetKeyFn>(gGetKey);
    return original ? original(key) : false;
}

bool ReplacementGetKeyDown(int32_t key) {
    RunFirstCallAction();
    if (gActive.load() && KeyDown(key)) return true;
    GetKeyFn original = Original<GetKeyFn>(gGetKeyDown);
    return original ? original(key) : false;
}

bool ReplacementGetKeyUp(int32_t key) {
    RunFirstCallAction();
    if (gActive.load() && KeyUp(key)) return true;
    GetKeyFn original = Original<GetKeyFn>(gGetKeyUp);
    return original ? original(key) : false;
}

bool InstallSlot(Target &target, const char *method, void *replacement,
                 IcallTarget &icall) {
    MethodRef ref = target.ResolveMethod("UnityEngine.InputLegacyModule.dll",
                                         "UnityEngine", "Input", method);
    if (!ref.valid()) {
        AKDM_LOG("hook %s: %s", method, target.LastError().c_str());
        return false;
    }
    SlotInfo info;
    if (!FindSlot(target, ref.code, info)) {
        AKDM_LOG("hook %s: icall slot not found", method);
        return false;
    }
    if (!target.InSegment("__DATA", info.slot, sizeof(uintptr_t))) {
        AKDM_LOG("hook %s: slot 0x%lx not in __DATA", method,
                 (unsigned long)info.slot);
        return false;
    }

    icall.resolve = info.resolve;
    icall.name = info.name;

    const uintptr_t slotAddress = info.slot;

    // The slot is zero until the game first uses that wrapper. Overwriting a
    // zero slot is still safe for *our* answers, but it removes the game's own
    // path: the wrapper would never resolve its icall, so anything we do not
    // answer would fall through to nothing. Capture what is there; the game
    // resolves lazily on first use, so most slots are already populated by the
    // time an input hook matters.
    uintptr_t existing = 0;
    if (icall.slot != 0) {
        // Already ours from an earlier pass: not a new install.
        return false;
    }
    if (!ReadValue(slotAddress, existing) || existing == 0) {
        // The game has not used this wrapper yet, so there is nothing to fall
        // back to. Leaving the slot alone keeps the game's own lazy resolution
        // intact; the periodic maintenance pass installs the hook once the
        // game has populated it.
        AKDM_LOG("hook %s: slot 0x%lx not resolved yet; deferring", method,
                 (unsigned long)slotAddress);
        return false;
    }
    icall.cached.store((void *)existing);

    // The slot lives in the zero-filled tail of __DATA. Writing it needs the
    // page to be writable, which it normally already is; use the guarded
    // helper so a read-only mapping cannot take the process down.
    const uintptr_t value = (uintptr_t)replacement;
    if (!WritePointer(slotAddress, value)) {
        AKDM_LOG("hook %s: cannot write slot 0x%lx", method,
                 (unsigned long)slotAddress);
        return false;
    }
    // Remember what we replaced so the hook can be undone if installation as a
    // whole fails part way through.
    icall.slot = slotAddress;
    icall.previous = existing;
    AKDM_LOG("hook %s: slot 0x%lx <- %p (existing %p)", method,
             (unsigned long)slotAddress, replacement, (void *)existing);
    return true;
}

} // namespace

InputHookReport InstallInputHooks(Target &target, uintptr_t slide) {
    InputHookReport report;
    gSlide = slide;

    struct Spec {
        const char *method;
        void *replacement;
        IcallTarget *icall;
        bool *flag;
    };
    // Touch and mouse hooks are deliberately NOT installed: PlayTools already
    // converts mouse input into Unity touches and feeds the game's native touch
    // pipeline, and covering get_touchCount/GetTouch/get_mousePosition only
    // competed with it. The game's PC key bindings read Input.GetKey, which has
    // no data source on iOS, so that is all this plugin needs to supply.
    const Spec specs[] = {
        {"GetKey", (void *)ReplacementGetKey, &gGetKey, &report.getKey},
        {"GetKeyDown", (void *)ReplacementGetKeyDown, &gGetKeyDown, &report.getKeyDown},
        {"GetKeyUp", (void *)ReplacementGetKeyUp, &gGetKeyUp, &report.getKeyUp},
    };

    for (const Spec &spec : specs) {
        report.attempted += 1;
        if (!InstallSlot(target, spec.method, spec.replacement, *spec.icall)) continue;
        if (spec.flag) *spec.flag = true;
        report.installed += 1;
    }

    // Never undo on a retry: earlier passes may already own working slots, and
    // rolling them back would silently disable input that was fine.
    if (report.installed == 0 && !gActive.load()) {
        UndoSlots();
        return report;
    }
    if (report.installed > 0) gActive.store(true);
    return report;
}

bool InstallScreenHooks(Target &target, uintptr_t slide) {
    // Removed with the pointer module: nothing consumes Screen size any more.
    (void)target;
    (void)slide;
    return false;
}

bool ProbeCursorHook(Target &target, uintptr_t slide) {
    MethodRef ref = target.ResolveMethod("UnityEngine.CoreModule.dll", "UnityEngine",
                                         "Cursor", "set_visible", 1);
    if (!ref.valid()) {
        AKDM_LOG("cursor probe: %s", target.LastError().c_str());
        return false;
    }
    AKDM_LOG("cursor probe: set_visible at 0x%lx (runtime 0x%lx)",
             (unsigned long)ref.code, (unsigned long)(ref.code + slide));
    return true;
}

void SetFirstCallAction(void (*action)()) { gFirstCallAction.store(action); }

void RefreshHardwareObservers() { AttachHardwareObservers(); }

} // namespace akdm
