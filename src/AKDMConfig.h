#pragma once
// Runtime configuration read once at load time from
//   ~/Library/Containers/<bundle>/Data/Documents/AKDM/config.plist
// Missing keys fall back to the defaults below; a missing file is not an error.
//
// The plugin only supplies keyboard input (PlayTools owns the mouse-to-touch
// path), so there is nothing here for mice or touches.

namespace akdm {

struct Config {
    // On by default: PC mode is what makes the game query Input.GetKey at all.
    // Without it the keyboard hooks can never install (the slots only resolve
    // once the game asks for keys), so a default of false disables the whole
    // plugin. The managed SetPCMode(true) call is safe now: it fires through
    // the touch probe, on the IL2CPP thread, from the game's own per-frame
    // touchCount query.
    bool enablePCMode = true;
    bool enableInputHooks = true;
};

// Reads the config file if present. Always returns a usable Config.
Config LoadConfig();

} // namespace akdm
