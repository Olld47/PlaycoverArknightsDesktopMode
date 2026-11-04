#pragma once
// Runtime configuration read once at load time from
//   ~/Library/Containers/<bundle>/Data/Documents/AKDM/config.plist
// Missing keys fall back to the defaults below; a missing file is not an error.
//
// The plugin only supplies keyboard input (PlayTools owns the mouse-to-touch
// path), so there is nothing here for mice or touches.

namespace akdm {

struct Config {
    // Off by default: calling the managed setter is the only correct way to
    // flip PC mode, but it runs inside IL2CPP and is the least proven part of
    // the plugin. Enable it explicitly once the keyboard path is verified.
    bool enablePCMode = false;
    bool enableInputHooks = true;
};

// Reads the config file if present. Always returns a usable Config.
Config LoadConfig();

} // namespace akdm
