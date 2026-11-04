#pragma once
// Hardware event bridge: GameController -> akdm input state.
//
// On Catalyst, keyboard and mouse hardware events arrive through GameController
// (GCKeyboard/ GCMouse) because the game is an iOS app. The game installs its
// own handlers for these objects; we chain behind them so the game keeps its
// original behaviour while we additionally observe every event.
//
// Nothing here synthesises touches or swallows events: the original handler is
// always invoked first and unchanged.

#include <cstdint>

namespace akdm {

// Installs (or re-installs) observers on the coalesced keyboard and every
// attached mouse. Safe to call repeatedly and from any thread; the actual
// work happens on the main queue.
void AttachHardwareObservers();

// Translates a GameController key code to a Unity KeyCode (0 when unmapped).
int32_t UnityKeyFromGameControllerKey(long gcKeyCode);

} // namespace akdm
