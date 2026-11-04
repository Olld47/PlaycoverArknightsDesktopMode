#!/bin/zsh
# Builds libAKDMDesktopMode.dylib for PlayCover injection.
#
#   zsh build.sh                 # release build (no logging)
#   AKDM_ENABLE_LOGGING=1 zsh build.sh
#
# Requires the Xcode Command Line Tools with the Mac Catalyst (iOSSupport)
# frameworks. The output is ad-hoc signed and has an @executable_path install
# name, which is what PlayCover expects for an injected plugin.

set -eu
cd "${0:A:h}"

SDK="$(xcrun --show-sdk-path)"
IOS_SUPPORT="$SDK/System/iOSSupport/System/Library/Frameworks"
OUTPUT="libAKDMDesktopMode.dylib"
LOGGING="${AKDM_ENABLE_LOGGING:-0}"

case "$LOGGING" in
  0|1) ;;
  *) print -u2 'AKDM_ENABLE_LOGGING must be 0 or 1'; exit 2 ;;
esac

if [[ ! -d "$IOS_SUPPORT" ]]; then
  print -u2 "Mac Catalyst frameworks not found under: $IOS_SUPPORT"
  print -u2 "Install the Xcode Command Line Tools (or full Xcode)."
  exit 2
fi

SOURCES=(
  src/AKDMCompat.mm
  src/AKDMConfig.mm
  src/AKDMTarget.cpp
  src/AKDMInput.cpp
  src/AKDMHardware.mm
  src/AKDMHook.cpp
  src/AKDMPlugin.mm
)

mkdir -p build

xcrun clang++ \
  -std=c++17 -O2 -fobjc-arc -fblocks -dynamiclib \
  -target arm64-apple-ios15.0-macabi \
  -isysroot "$SDK" \
  -I"$SDK/usr/include/c++/v1" \
  -Isrc \
  -DAKDM_ENABLE_LOGGING="$LOGGING" \
  -iframework "$IOS_SUPPORT" \
  -F "$IOS_SUPPORT" \
  -framework Foundation -framework UIKit -framework GameController \
  -install_name '@executable_path/Frameworks/libAKDMDesktopMode.dylib' \
  -Wl,-no_warn_duplicate_libraries \
  "${SOURCES[@]}" \
  -o "build/$OUTPUT"

codesign --force --sign - "build/$OUTPUT"
cp "build/$OUTPUT" "$OUTPUT"
file "$OUTPUT"
shasum -a 256 "$OUTPUT"
print "built: ${0:A:h}/$OUTPUT (logging=$LOGGING)"
