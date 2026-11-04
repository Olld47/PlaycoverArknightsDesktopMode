#!/bin/zsh
# Builds and runs the offline target validator against the installed game.
# Nothing here launches, injects into or modifies the game.
set -eu
cd "${0:A:h}/.."
APP="${1:-$HOME/Library/Containers/io.playcover.PlayCover/Applications/com.hypergryph.arknights.app}"
if [[ ! -d "$APP" ]]; then
  print -u2 "game bundle not found: $APP"
  exit 2
fi
mkdir -p build
xcrun clang++ -std=c++17 -O1 -g \
  -Isrc \
  tools/validate_target.cpp src/AKDMTarget.cpp src/AKDMCompat.mm \
  -framework Foundation \
  -o build/validate_target
./build/validate_target "$APP"
