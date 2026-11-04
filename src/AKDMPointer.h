#pragma once
// Pointer position tracking.
//
// Unity's `Input.mousePosition` is absolute, but GameController only reports
// relative motion, and UIKit's hover gesture does not fire while the pointer is
// captured. The position is therefore polled from the window system: on a
// Catalyst build the app still has an NSWindow, and `NSEvent.mouseLocation`
// gives the absolute position regardless of capture.
//
// Polling is driven by the maintenance timer; nothing here synthesises input.

namespace akdm {

// Reads the current pointer position and publishes it in the game's render
// space. Safe to call from any thread (hops to the main queue when needed).
void PollPointerPosition();

// Reads the pointer position synchronously on the calling thread. Used by the
// input hooks so a query returns the position *now*, not the last polled one.
// Returns false when the window geometry is not known yet.
bool ReadPointerPositionNow(float &x, float &y);

// Installs the UIKit hover observer as a fallback, used only when no NSWindow
// is reachable (for example before the window has been created).
void AttachPointerObserver();

// Supplies the render resolution the game actually uses (`Screen.width/height`).
// The pointer mapping must be expressed in that space because the game
// normalises mouse input by those values.
void SetRenderSize(float width, float height);

// Recomputes the render size used to scale window points into game pixels.
void UpdateRenderSize();

} // namespace akdm
