#!/bin/zsh
# Runs every offline check. Nothing here launches or modifies the game.
set -eu
cd "${0:A:h}/.."

APP="${1:-$HOME/Library/Containers/io.playcover.PlayCover/Applications/com.hypergryph.arknights.app}"
SDK="$(xcrun --show-sdk-path)"
CXX_INC="$SDK/usr/include/c++/v1"

if [[ ! -d "$APP" ]]; then
  print -u2 "game bundle not found: $APP"
  print -u2 "pass its path as the first argument"
  exit 2
fi

mkdir -p build

print "== building validators"
xcrun clang++ -std=c++17 -O1 -isysroot "$SDK" -I"$CXX_INC" -Isrc \
  tools/validate_target.cpp src/AKDMTarget.cpp src/AKDMCompat.mm \
  -framework Foundation -o build/validate_target
xcrun clang++ -std=c++17 -O1 -isysroot "$SDK" -I"$CXX_INC" -Isrc \
  tools/validate_hooks.cpp src/AKDMTarget.cpp src/AKDMCompat.mm \
  -framework Foundation -o build/validate_hooks

xcrun clang++ -std=c++17 -O1 -isysroot "$SDK" -I"$CXX_INC" -Isrc \
  tools/test_input.cpp src/AKDMInput.cpp -o build/test_input

print "\n== input state machine"
./build/test_input

print "\n== symbol resolution"
./build/validate_target "$APP"

print "\n== input hooks and Touch layout"
./build/validate_hooks "$APP"

print "\n== plugin build"
zsh build.sh

print "\n== plugin artifact"
file libAKDMDesktopMode.dylib
codesign --verify --verbose=1 libAKDMDesktopMode.dylib
otool -D libAKDMDesktopMode.dylib | tail -1
