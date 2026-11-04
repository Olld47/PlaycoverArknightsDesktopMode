#pragma once
// Unity input interception.
//
// The game reads input through Unity's legacy `Input` API. Each managed wrapper
// (`Input.GetKey`, `Input.get_mousePosition`, ...) is a tiny IL2CPP method that
// resolves and calls an internal icall through a pointer slot:
//
//   ldr  x1, [x20, #slot]      ; lazily resolved icall pointer
//   cbnz x1, .ready
//   ...  resolve, then str x0, [x20, #slot]
// .ready:
//   br   x1
//
// Those slots live in the writable, zero-initialised tail of __DATA, so
// installing our own pointer is a pure data write: no instruction is patched,
// nothing needs a trampoline, and the change disappears with the process.
//
// This module finds each slot by decoding the wrapper's own `adrp`/`ldr` pair,
// verifies the surrounding instructions match the expected shape, and only then
// publishes our implementation. If the game has already resolved the icall we
// keep the original pointer and chain to it for any query we do not answer.

#include <cstdint>

namespace akdm {

struct InputHookReport {
    bool getKey = false;
    bool getKeyDown = false;
    bool getKeyUp = false;
    bool getAxis = false;
    bool getAxisRaw = false;
    bool mousePosition = false;
    bool mousePresent = false;
    bool mouseButton = false;
    bool mouseButtonDown = false;
    bool mouseButtonUp = false;
    bool scrollDelta = false;
    bool touchCount = false;
    bool getTouch = false;
    int installed = 0;
    int attempted = 0;
};

// Resolves every Input method it needs through `target` and installs the
// hooks it can. Individual failures are reported, never fatal.
InputHookReport InstallInputHooks(class Target &target, uintptr_t slide);

// Hooks UnityEngine.Screen.get_width/get_height and forwards their values to
// the pointer module, so the mouse mapping uses the game's render resolution
// (the game normalises mouse input by Screen.width/height).
bool InstallScreenHooks(class Target &target, uintptr_t slide);

// Reports whether the cursor hook target resolved (diagnostics only).
bool ProbeCursorHook(class Target &target, uintptr_t slide);

// Re-installs hardware observers if the game replaced our handlers.
void RefreshHardwareObservers();

// Registers a one-shot callback that runs the first time a hooked Unity input
// function is invoked. That moment is guaranteed to be on the IL2CPP thread,
// which is the only safe place to call into managed code.
void SetFirstCallAction(void (*action)());

// Installs a fully transparent probe on Input.get_touchCount: the game's own
// implementation is always called through, so touch behaviour is untouched.
// Its only purpose is to give the one-shot callback a reliable trigger — the
// game queries touchCount every frame, but a game that is not yet in PC mode
// never queries GetKey, so the keyboard hooks themselves cannot be the trigger
// (chicken-and-egg: the keyboard hooks cannot install until the slots resolve,
// and the slots only resolve once the game asks for keys).
bool InstallTouchProbe(class Target &target, uintptr_t slide);

// Sets the callback the touch probe runs on *every* call (not once). Used for
// work that must be retried until the IL2CPP class it touches is initialised.
void SetProbeAction(void (*action)());

// Forces DeviceInfoUtil.IsPCMode() to return true by populating the
// __Hotfix0_IsPCMode static delegate slot with a minimal wrapper object whose
// method pointer just returns true.
//
// Why not DeviceInfoUtil.SetPCMode(true): that method routes through a platform
// strategy object that is never assembled in the iOS build, so the call is a
// silent no-op (observed: invoked without effect, IsPCMode still false). The
// hot-update framework's own hook slot is what the reference implementation
// (libUnityDesktopMode) populates via Marshal::GetDelegateForFunctionPointer;
// the dispatch site in IsPCMode only dereferences +0x18/+0x28/+0x40 of the
// object, so a hand-built object with a `return true` stub behaves identically.
//
// Must run once, on the IL2CPP thread, after the class is initialised.
bool InstallPCModeOverride(class Target &target, uintptr_t slide);

// Reports whether the game's PC input module is alive. Samples the
// TorappuPCInputHelper static singletons that OnProcess dereferences before it
// ever reaches the IsPCMode branch:
//
//   OnProcess: +44 ldr x0,[statics+0x50] ; +48 cbz x0 -> other path
//
// A permanently null slot means the module was never assembled on iOS, which
// is what a real fix would have to address. Returns true once the slot is
// populated (the game did it itself, no action needed).
bool ProbeInputModule(class Target &target);

// Locates Unity's internal UnitySetKeyState, which is not exported and cannot
// be found by symbol lookup. It is reached through -[UnityView handleCommand:]:
// that method contains a BL to it, and the callee has a recognisable epilogue.
// Returns the runtime address, or 0 when the pattern is absent.
uintptr_t FindUnitySetKeyState(class Target &target);

// Feeds a key press into Unity's own input state so any consumer sees it,
// regardless of whether the game polls Input.GetKey itself. Safe to call from
// any thread that already holds the IL2CPP runtime.
bool InjectKeyState(int32_t unityKey, bool down);

} // namespace akdm
