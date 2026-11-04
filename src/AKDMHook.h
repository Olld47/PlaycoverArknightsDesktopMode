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

} // namespace akdm
