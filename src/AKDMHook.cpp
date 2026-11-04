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

        // Decode the resolver call that follows the name load: `BL
        // il2cpp_resolve_icall`. Its target is what the wrapper itself would
        // use, so a replacement installed on a zero slot can chain the same way.
        for (int k = i + 2; k < i + 12; ++k) {
            const uintptr_t cursor = code + (uintptr_t)k * 4;
            uint32_t word = 0;
            if (!target.ReadBytes(cursor, &word, 4)) break;
            if ((word & 0xfc000000u) != 0x94000000u) continue;
            int64_t imm = (int64_t)(word & 0x3ffffff);
            if (imm & (1 << 25)) imm -= (1 << 26);
            info.resolve = (ResolveIcallFn)(cursor + (uintptr_t)(imm << 2));
            break;
        }
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
    void *cached = target.cached.load(std::memory_order_acquire);
    if (cached != nullptr) return cached;

    // The slot was zero at install time, so the game had never resolved this
    // icall. Resolve it the same way the Unity wrapper would have: read the
    // icall name and ask the runtime. This runs on the IL2CPP thread because
    // every caller is one of our replacements, invoked from managed code.
    if (target.resolve == nullptr || target.name == 0) return nullptr;
    char name[160] = {};
    if (!ReadMemory(target.name, name, sizeof(name) - 1)) return nullptr;
    void *resolved = target.resolve(name);
    if (resolved != nullptr) target.cached.store(resolved, std::memory_order_release);
    AKDM_LOG("icall '%s' resolved lazily -> %p", name, resolved);
    return resolved;
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
    // A zero slot is installable. Refusing it created a deadlock: the game only
    // populates the slot by calling the wrapper, and it never calls the wrapper
    // until PC mode routes input there, which in turn never happens. The
    // replacement resolves the original lazily on first use instead (see
    // ResolveOriginal), so chaining still works from a zero start.
    if (!ReadValue(slotAddress, existing)) {
        AKDM_LOG("hook %s: slot 0x%lx unreadable", method,
                 (unsigned long)slotAddress);
        return false;
    }
    if (existing == 0)
        AKDM_LOG("hook %s: slot 0x%lx still zero; installing anyway", method,
                 (unsigned long)slotAddress);
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

// ---------------------------------------------------------------------------
// Touch probe
// ---------------------------------------------------------------------------

namespace {

using GetTouchCountFn = int32_t (*)(void);
IcallTarget gTouchProbe;
std::atomic<void (*)()> gProbeAction{nullptr};

// The one-shot action is not actually one-shot for this probe: the override
// needs DeviceInfoUtil's static fields, which IL2CPP only allocates when the
// class is first used — possibly long after the probe is installed. Retrying
// from the game's own per-frame touchCount query is the one place guaranteed to
// run on the IL2CPP thread with the runtime live.
int32_t ReplacementTouchProbe() {
    auto probe = gProbeAction.load();
    if (probe != nullptr) probe();
    GetTouchCountFn original = Original<GetTouchCountFn>(gTouchProbe);
    return original ? original() : 0;
}

bool InstallSlotFor(Target &target, const char *image, const char *ns,
                    const char *klass, const char *method, void *replacement,
                    IcallTarget &icall) {
    MethodRef ref = target.ResolveMethod(image, ns, klass, method);
    if (!ref.valid()) {
        AKDM_LOG("probe %s: %s", method, target.LastError().c_str());
        return false;
    }
    SlotInfo info;
    if (!FindSlot(target, ref.code, info)) {
        AKDM_LOG("probe %s: icall slot not found", method);
        return false;
    }
    if (!target.InSegment("__DATA", info.slot, sizeof(uintptr_t))) return false;
    if (icall.slot != 0) return false;

    uintptr_t existing = 0;
    if (!ReadValue(info.slot, existing) || existing == 0) {
        AKDM_LOG("probe %s: slot not resolved yet; deferring", method);
        return false;
    }
    icall.cached.store((void *)existing);
    if (!WritePointer(info.slot, (uintptr_t)replacement)) return false;
    icall.slot = info.slot;
    icall.previous = existing;
    AKDM_LOG("probe %s: slot 0x%lx <- %p (existing %p)", method,
             (unsigned long)info.slot, replacement, (void *)existing);
    return true;
}

} // namespace

bool InstallTouchProbe(Target &target, uintptr_t slide) {
    (void)slide;
    if (gTouchProbe.slot != 0) return true;
    const bool ok = InstallSlotFor(target, "UnityEngine.InputLegacyModule.dll",
                                   "UnityEngine", "Input", "get_touchCount",
                                   (void *)ReplacementTouchProbe, gTouchProbe);
    if (ok) AKDM_LOG("touch probe installed (one-shot trigger armed)");
    return ok;
}

// ---------------------------------------------------------------------------
// Unity internal key state
// ---------------------------------------------------------------------------

// Both functions live in AKDMUnityKey.mm: discovery needs the Objective-C
// runtime and injection needs to marshal onto the player-loop thread.
// The resolved setter is stored there too, so this file stays plain C++.
uintptr_t FindUnitySetKeyStateImpl(Target &target);
bool InjectKeyStateReady();
bool InjectKeyStateImpl(int32_t unityKey, bool down);

uintptr_t FindUnitySetKeyState(Target &target) {
    return FindUnitySetKeyStateImpl(target);
}

bool InjectKeyState(int32_t unityKey, bool down) {
    if (!InjectKeyStateReady()) return false;
    return InjectKeyStateImpl(unityKey, down);
}

bool ProbeInputModule(Target &target) {
    // Retained as a cheap health check. The original diagnostic dumped the
    // TorappuPCInputHelper statics; that investigation concluded that the PC
    // input module is never assembled on iOS, which is why keyboard input is
    // delivered through UnitySetKeyState instead.
    (void)target;
    return false;
}

bool InstallScreenHooks(Target &target, uintptr_t slide) {
    // Removed with the pointer module: nothing consumes Screen size any more.
    (void)target;
    (void)slide;
    return false;
}

// ---------------------------------------------------------------------------
// PC mode override
// ---------------------------------------------------------------------------

namespace {

// The replacement "method" the crafted delegate points at: `mov w0, #1; ret`.
// IsPCMode's dispatch site calls it with two arguments it never inspects, so a
// constant-true function reproduces the reference plugin's Marshal-built stub.
extern "C" int32_t akdm_pc_mode_stub(void *, void *) { return 1; }

std::atomic<bool> gPCModeOverridden{false};

// Minimal stand-in for the Marshal delegate the hot-update framework would
// normally install. Only three fields are ever read:
//   +0x18 method pointer  (called)
//   +0x28 argument        (passed in x1, ignored by the stub)
//   +0x40 argument        (passed in x0, ignored by the stub)
// The klass pointer is copied from an arbitrary known good class so that any
// incidental GC or type inspection sees a plausible object header.
struct alignas(8) FakeDelegate {
    void *pad[3];        // +0x00 klass/monitor, +0x10 target
    void *methodPtr;     // +0x18
    void *arg1;          // +0x20
    void *arg2;          // +0x28
    void *pad2[3];       // +0x30 .. +0x38
    void *arg3;          // +0x40
};

// Reads a pointer from `address`, following the runtime's own view (no extra
// slide: Target already reports runtime addresses and dyld has rebased the
// chained pointers by the time this runs).
bool ReadPointer(uintptr_t address, uintptr_t &out) {
    return ReadValue(address, out);
}

} // namespace

// Logs a retry-able failure only when the reason changes, so a probe firing
// every frame cannot flood the log with the same line.
void LogOverrideState(const char *message, uintptr_t detail) {
    static std::atomic<int> last{0};
    const int tag = (int)(detail & 0x7fff) ^ (message[0] << 8);
    if (last.exchange(tag) == tag) return;
    if (detail != 0)
        AKDM_LOG("pc override: %s (0x%lx)", message, (unsigned long)detail);
    else
        AKDM_LOG("pc override: %s", message);
}

bool InstallPCModeOverride(Target &target, uintptr_t slide) {
    (void)slide;
    // Note: the flag is only set on success, so a probe that fires every frame
    // keeps retrying until DeviceInfoUtil's static fields exist.
    if (gPCModeOverridden.load()) return true;

    // DeviceInfoUtil's Il2CppClass* global. Recovered from the IsPCMode
    // disassembly (adrp x20 / ldr x8,[x20] at +0x14/+0x34); the same global
    // backs SetPCMode, so resolving it through the method is data-driven.
    MethodRef isPC = target.ResolveMethod("Torappu.Common.dll", "Torappu",
                                          "DeviceInfoUtil", "IsPCMode", 0);
    if (!isPC.valid()) {
        AKDM_LOG("pc override: %s", target.LastError().c_str());
        return false;
    }

    // Decode IsPCMode's class global by tracking register provenance.
    //
    // The load is not adjacent to its adrp: the prologue is
    //   +14 adrp x20, page
    //   +18 add  x20, x20, #off    (class metadata address)
    //   +34 ldr  x8, [x20]         (the Il2CppClass* global)
    // so a scan that only looks at pc+4 can never match it. Track adrp results
    // per register, apply adds, and take the first load that uses one.
    uintptr_t classGlobal = 0;
    uintptr_t regBase[32] = {};
    bool regKnown[32] = {};
    for (int i = 0; i < 24; ++i) {
        const uintptr_t pc = isPC.code + (uintptr_t)i * 4;
        uint32_t word = 0;
        if (!target.ReadBytes(pc, &word, 4)) break;

        if ((word & 0x9f000000u) == 0x90000000u) {  // adrp xd, page
            const uint32_t rd = word & 0x1f;
            const uint32_t immlo = (word >> 29) & 0x3;
            const uint32_t immhi = (word >> 5) & 0x7ffff;
            int64_t imm = (int64_t)((immhi << 2) | immlo);
            if (imm & (1 << 20)) imm -= (1 << 21);
            regBase[rd] = (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
            regKnown[rd] = true;
            continue;
        }

        if ((word & 0xff800000u) == 0x91000000u) {  // add xd, xn, #imm12
            const uint32_t rn = (word >> 5) & 0x1f;
            const uint32_t rd = word & 0x1f;
            if (regKnown[rn]) {
                regBase[rd] = regBase[rn] + (uintptr_t)((word >> 10) & 0xfff);
                regKnown[rd] = true;
            }
            continue;
        }

        if ((word & 0xffc00000u) == 0xf9400000u) {  // ldr xd, [xn, #imm]
            const uint32_t rn = (word >> 5) & 0x1f;
            const uint32_t imm = ((word >> 10) & 0xfff) * 8;
            if (!regKnown[rn]) continue;
            classGlobal = regBase[rn] + imm;
            break;
        }
    }
    if (classGlobal == 0) {
        LogOverrideState("class global not found in IsPCMode", 0);
        return false;
    }

    uintptr_t classPtr = 0;
    if (!ReadPointer(classGlobal, classPtr) || classPtr == 0) {
        LogOverrideState("DeviceInfoUtil class not initialised yet", classGlobal);
        return false;
    }

    uintptr_t statics = 0;
    if (!ReadPointer(classPtr + 0xB8, statics) || statics == 0) {
        LogOverrideState("static fields block not allocated yet", classPtr);
        return false;
    }

    // The delegate slot is at +0x28 in the statics block (observed in IsPCMode:
    // ldr x8,[x8,#0x28] after loading static_fields).
    const uintptr_t slot = statics + 0x28;
    uintptr_t existing = 0;
    ReadPointer(slot, existing);
    if (existing != 0) {
        // Already populated — either the game's own hot-update or a previous
        // pass. Leave whatever is there.
        AKDM_LOG("pc override: hotfix slot already populated (%p); leaving it",
                 (void *)existing);
        gPCModeOverridden.store(true);
        return true;
    }

    static FakeDelegate delegate;
    delegate.methodPtr = (void *)&akdm_pc_mode_stub;
    delegate.arg1 = nullptr;
    delegate.arg2 = nullptr;
    delegate.arg3 = nullptr;
    delegate.pad[0] = (void *)classPtr;  // plausible header for the object
    delegate.pad[1] = nullptr;
    delegate.pad[2] = nullptr;
    delegate.pad2[0] = nullptr;
    delegate.pad2[1] = nullptr;
    delegate.pad2[2] = nullptr;

    if (!WritePointer(slot, (uintptr_t)&delegate)) {
        LogOverrideState("cannot write hotfix slot", slot);
        return false;
    }
    gPCModeOverridden.store(true);
    AKDM_LOG("pc override: __Hotfix0_IsPCMode at 0x%lx -> stub %p",
             (unsigned long)slot, (void *)&akdm_pc_mode_stub);

    return true;
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

void SetProbeAction(void (*action)()) { gProbeAction.store(action); }

void RefreshHardwareObservers() { AttachHardwareObservers(); }

} // namespace akdm
