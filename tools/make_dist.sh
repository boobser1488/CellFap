#!/usr/bin/env bash
# Builds build/dist: the Windows game folder and its zip, and the Android APK beside them.
#   tools/make_dist.sh            (after the desktop build in build/release and android/build_apk.sh)
# The folder holds the game, the dypt library for program cells and mods (with the Visual C++ runtime it needs, so it
# works on a PC without the redistributable), the example mods, modpack and the mod SDK with its docs. No settings:
# every player starts with the defaults, and «Новое» opens on the first start.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REL="$ROOT/build/release"
OUT="$ROOT/build/dist"
W="$OUT/CellFap-windows"

for f in CellFap.exe organisms.dll dypt_embed.dll modpack.exe; do
    [ -f "$REL/$f" ] || { echo "missing $REL/$f: build the game first (cmake --build build/release)"; exit 1; }
done

rm -rf "${W:?}"
mkdir -p "$W/genomes" "$W/import" "$W/mods" "$W/docs"
cp "$REL/CellFap.exe" "$REL/organisms.dll" "$REL/dypt_embed.dll" "$REL/modpack.exe" "$W/"
# dypt_embed.dll is built with MSVC: its runtime goes next to it (app-local deployment of vcruntime140.dll is allowed).
for rt in /c/Windows/System32/vcruntime140.dll /c/Windows/System32/vcruntime140_1.dll; do
    [ -f "$rt" ] && cp "$rt" "$W/"
done

# Example mods: the dypt one from the sources, the native and shader ones as the build made them.
cp -r "$ROOT/mods/example" "$W/mods/"
for m in native_example shader_tint; do
    [ -d "$REL/mods/$m" ] && cp -r "$REL/mods/$m" "$W/mods/"
done

# The mod SDK and how to write mods (without build outputs of the Rust example).
mkdir -p "$W/sdk"
cp "$ROOT/sdk/cellfap_mod.h" "$W/sdk/"
cp -r "$ROOT/sdk/examples" "$W/sdk/"
rm -rf "${W:?}/sdk/examples/rust/target" "${W:?}/sdk/examples/rust/Cargo.lock"
cp "$ROOT/docs/mods.md" "$W/docs/"
cp "$ROOT/README.md" "$W/"

# The zip, made by Windows itself (no extra tools).
rm -f "${OUT:?}/CellFap-windows.zip"
powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$W")' -DestinationPath '$(cygpath -w "$OUT/CellFap-windows.zip")'"

# The phone build, when there is one.
if [ -f "$ROOT/build/android/CellFap.apk" ]; then
    cp "$ROOT/build/android/CellFap.apk" "$OUT/CellFap.apk"
else
    echo "note: no build/android/CellFap.apk (android/build_apk.sh): the dist has no APK"
fi

ls -la "$OUT"
