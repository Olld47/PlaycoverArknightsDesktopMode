// GameController -> akdm input bridge.
//
// The game's own handlers are always invoked first and unchanged; we only
// observe. Handlers are chained with associated objects so repeated attach
// calls cannot stack wrappers.

#include "AKDMHardware.h"
#include "AKDMCompat.h"
#include "AKDMInput.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

namespace akdm {
namespace {

const void *kKeyboardWrapperKey = &kKeyboardWrapperKey;
const void *kMouseMoveWrapperKey = &kMouseMoveWrapperKey;
const void *kMouseButtonWrapperKey = &kMouseButtonWrapperKey;

// GCKeyCode -> Unity KeyCode. Built once at attach time.
struct KeyMapEntry {
    GCKeyCode code;
    int32_t unity;
};

int32_t UnityKeyForLetter(GCKeyCode code) {
    static const struct {
        GCKeyCode code;
        int32_t unity;
    } letters[] = {
        {GCKeyCodeKeyA, kKeyA}, {GCKeyCodeKeyB, kKeyB}, {GCKeyCodeKeyC, kKeyC},
        {GCKeyCodeKeyD, kKeyD}, {GCKeyCodeKeyE, kKeyE}, {GCKeyCodeKeyF, kKeyF},
        {GCKeyCodeKeyG, kKeyG}, {GCKeyCodeKeyH, kKeyH}, {GCKeyCodeKeyI, kKeyI},
        {GCKeyCodeKeyJ, kKeyJ}, {GCKeyCodeKeyK, kKeyK}, {GCKeyCodeKeyL, kKeyL},
        {GCKeyCodeKeyM, kKeyM}, {GCKeyCodeKeyN, kKeyN}, {GCKeyCodeKeyO, kKeyO},
        {GCKeyCodeKeyP, kKeyP}, {GCKeyCodeKeyQ, kKeyQ}, {GCKeyCodeKeyR, kKeyR},
        {GCKeyCodeKeyS, kKeyS}, {GCKeyCodeKeyT, kKeyT}, {GCKeyCodeKeyU, kKeyU},
        {GCKeyCodeKeyV, kKeyV}, {GCKeyCodeKeyW, kKeyW}, {GCKeyCodeKeyX, kKeyX},
        {GCKeyCodeKeyY, kKeyY}, {GCKeyCodeKeyZ, kKeyZ},
    };
    for (const auto &entry : letters) {
        if (entry.code == code) return entry.unity;
    }
    return 0;
}

} // namespace

int32_t UnityKeyFromGameControllerKey(long gcKeyCode) {
    const GCKeyCode code = (GCKeyCode)gcKeyCode;

    const int32_t letter = UnityKeyForLetter(code);
    if (letter != 0) return letter;

    if (code == GCKeyCodeOne) return kKeyAlpha1;
    if (code == GCKeyCodeTwo) return kKeyAlpha2;
    if (code == GCKeyCodeThree) return kKeyAlpha3;
    if (code == GCKeyCodeFour) return kKeyAlpha4;
    if (code == GCKeyCodeFive) return kKeyAlpha5;
    if (code == GCKeyCodeSix) return kKeyAlpha6;
    if (code == GCKeyCodeSeven) return kKeyAlpha7;
    if (code == GCKeyCodeEight) return kKeyAlpha8;
    if (code == GCKeyCodeNine) return kKeyAlpha9;
    if (code == GCKeyCodeZero) return kKeyAlpha0;

    if (code == GCKeyCodeReturnOrEnter) return kKeyReturn;
    if (code == GCKeyCodeEscape) return kKeyEscape;
    if (code == GCKeyCodeTab) return kKeyTab;
    if (code == GCKeyCodeSpacebar) return kKeySpace;
    if (code == GCKeyCodeDeleteOrBackspace) return kKeyBackspace;

    if (code == GCKeyCodeUpArrow) return kKeyUpArrow;
    if (code == GCKeyCodeDownArrow) return kKeyDownArrow;
    if (code == GCKeyCodeLeftArrow) return kKeyLeftArrow;
    if (code == GCKeyCodeRightArrow) return kKeyRightArrow;

    if (code == GCKeyCodeLeftShift) return kKeyLeftShift;
    if (code == GCKeyCodeRightShift) return kKeyRightShift;
    if (code == GCKeyCodeLeftControl)
        return kKeyLeftControl;
    if (code == GCKeyCodeRightControl)
        return kKeyLeftControl;  // Unity has no distinct right control here.
    if (code == GCKeyCodeLeftAlt) return kKeyLeftAlt;
    if (code == GCKeyCodeRightAlt) return kKeyRightAlt;

    if (code == GCKeyCodeF1) return kKeyF1;
    return 0;
}

void AttachHardwareObservers() {
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            GCKeyboardInput *keyboard = GCKeyboard.coalescedKeyboard.keyboardInput;
            if (keyboard) {
                GCKeyboardValueChangedHandler current = keyboard.keyChangedHandler;
                id existing =
                    objc_getAssociatedObject(keyboard, kKeyboardWrapperKey);
                if (!(existing && current == existing)) {
                    GCKeyboardValueChangedHandler wrapper =
                        ^(GCKeyboardInput *input, GCControllerButtonInput *button,
                          GCKeyCode code, BOOL pressed) {
                            // Forward first: the game's own handling must be
                            // unaffected by anything we do afterwards.
                            if (current) current(input, button, code, pressed);
                            const int32_t unity =
                                UnityKeyFromGameControllerKey((long)code);
                            if (unity != 0) SetKeyState(unity, pressed != NO);
                        };
                    keyboard.keyChangedHandler = wrapper;
                    objc_setAssociatedObject(keyboard, kKeyboardWrapperKey,
                                             wrapper,
                                             OBJC_ASSOCIATION_COPY_NONATOMIC);
                    AKDM_LOG("keyboard observer attached (chained=%d)",
                             current != nil ? 1 : 0);
                }
            }


            // Mouse, scroll and button handling is intentionally absent:
            // PlayTools already converts mouse input into Unity touches (its
            // hook_GCMouse_mice / enableScrollWheel paths), and intercepting
            // those events here only competed with that pipeline. The plugin
            // contributes keyboard state and PC-mode activation only.
        }
    });
}

} // namespace akdm
