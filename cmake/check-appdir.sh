#!/bin/sh
# check-appdir.sh <AppDir> <qt lib dir>: fails when a bundled library's build-id no longer matches
# the copy it came from. linuxdeploy's patchelf overwrites the start of a library that has no room
# for the RUNPATH it adds, and the loader then crashes on it.
set -eu
appdir=$1
qt_libs=$2
build_id() { readelf -n "$1" 2>/dev/null | sed -n 's/.*Build ID: //p'; }
bad=""
for lib in "$appdir"/usr/lib/*.so*; do
  name=$(basename "$lib")
  for dir in "$qt_libs" /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/local/lib; do
    [ -e "$dir/$name" ] || continue
    want=$(build_id "$dir/$name")
    [ -n "$want" ] && [ "$want" != "$(build_id "$lib")" ] && bad="$bad $name"
    break
  done
done
if [ -n "$bad" ]; then
  echo "patchelf damaged these bundled libraries:$bad" >&2
  echo "Build in the container instead: cmake --build <preset> --target appimage" >&2
  rm -f Mira-x86_64.AppImage
  exit 1
fi
