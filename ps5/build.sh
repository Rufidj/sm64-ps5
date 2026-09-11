#!/bin/bash
# Builds the PS5 title into ps5/out/<TITLE_ID>/, a folder ready to copy to the
# console's homebrew folder.
#
# Run the PC build first (make, from the repository root, with baserom.us.z64
# there): it extracts the assets and generates the sources this compiles.
#
# Environment:
#   PS5_PAYLOAD_SDK  the ps5-payload-sdk install (bin/prospero-clang)
#   PS5LINK          the ps5link-sdk checkout, built (linker/link_real, linker/crt1_ps5.o)
#   SHARPPROSPERO    a SharpProspero checkout, for its "self --sign" tool (needs .NET)
#   LIBC_PRX         a signed libc.prx for sce_module/ (see README); optional here
#   TITLE_ID         the title's id, 4 letters and 5 digits (default PPSA64064)
#   TITLE_NAME       the name shown on the home screen (default "SM64 PS5")
#   HD_PACK          an sm64ex texture pack folder, to match the index to it; optional
#   LLVM_MC, LLVM_OBJCOPY  LLVM tools with the amdgcn target (default: from PATH)
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out=$here/out
TITLE_ID=${TITLE_ID:-PPSA64064}
TITLE_NAME=${TITLE_NAME:-SM64 PS5}

fail() { echo "build.sh: $*" >&2; exit 1; }

[ -n "$PS5_PAYLOAD_SDK" ] || fail "set PS5_PAYLOAD_SDK to the ps5-payload-sdk install"
[ -n "$PS5LINK" ] || fail "set PS5LINK to the ps5link-sdk checkout"
[ -n "$SHARPPROSPERO" ] || fail "set SHARPPROSPERO to a SharpProspero checkout"
CC=$PS5_PAYLOAD_SDK/bin/prospero-clang
LINK=$PS5LINK/linker/link_real
CRT1=$PS5LINK/linker/crt1_ps5.o
[ -x "$CC" ] || fail "no prospero-clang at $CC"
[ -x "$LINK" ] && [ -f "$CRT1" ] || fail "build ps5link first (make in $PS5LINK)"
[ -f "$root/baserom.us.z64" ] || fail "put baserom.us.z64 in $root"
[ -d "$root/build/us_pc/include" ] || fail "run the PC build (make) in $root first"
[ -x "$root/tools/textconv" ] || fail "the PC build's tools are missing: run make in $root first"
echo "$TITLE_ID" | grep -Eq '^[A-Z]{4}[0-9]{5}$' || fail "TITLE_ID must be 4 capital letters and 5 digits"

mkdir -p "$out/obj/engine" "$out/obj/ps5"

echo ">>> shaders"
"$here/build_shaders.sh" > "$out/shaders.log" 2>&1 || { cat "$out/shaders.log"; fail "building the shaders failed"; }

echo ">>> menu font"
mkdir -p "$here/menu_build"
python3 "$here/menu_tools/make_font.py" "$here/menu_build/menu_font.h"

DEFS="-D_LANGUAGE_C -DVERSION_US=1 -DF3DEX_GBI_2E=1 -DNON_MATCHING=1 -DAVOID_UB=1 -DNO_SEGMENTED_MEMORY -DUSE_SYSTEM_MALLOC"
INCS="-I$root/include -I$root/src -I$root/src/pc -I$root -I$root/build/us_pc -I$root/build/us_pc/include"
LANG_INCS="-I$here/lang -I$out/lang"
ENGINE_FLAGS="-g -O2 -fno-strict-aliasing -fwrapv -Wno-everything $DEFS $INCS $LANG_INCS -DWIDESCREEN -DSM64_PS5_REFLECTIONS -DSM64_PS5_HD_TEXTURES -DSM64_PS5_LANGUAGE -DENABLE_RUMBLE=1 $EXTRA_CFLAGS"

echo ">>> Spanish text"
mkdir -p "$out/lang"
python3 "$here/lang/make_glyphs.py" "$out/lang/glyphs_es.h"
python3 "$here/lang/wrap_es.py" "$root/text/us/dialogs.h" "$here/lang/text_es/dialogs_es.txt" \
    "$here/lang/charmap_es.txt" "$root/src/game/ingame_menu.c" "$here/lang/text_es/dialogs.h"
cpp -P -Wno-trigraphs -I "$here/lang/text_es" $INCS $DEFS "$root/text/define_text.inc.c" -o - |
    "$root/tools/textconv" "$here/lang/charmap_es.txt" - "$out/lang/text_es_define.inc.c"
python3 "$here/lang/make_strings.py" "$here/lang/strings_es.txt" "$out/lang/strings_es.c.in"
"$root/tools/textconv" "$here/lang/charmap_es.txt" "$out/lang/strings_es.c.in" "$out/lang/strings_es.c"
SAVE_PATH='-DSM64_SAVE_FILE_PATH="/app0/data/sm64_ps5/saves/sm64_save_file.bin"'

echo ">>> engine ($(grep -vc '^#' "$here/engine_sources.txt") sources)"
export CC ENGINE_FLAGS SAVE_PATH root out
grep -v '^#' "$here/engine_sources.txt" | xargs -P "$(nproc)" -I{} bash -c '
    src={}; obj=$out/obj/engine/$(echo "$src" | tr / _).o
    $CC $ENGINE_FLAGS "$SAVE_PATH" -c "$root/$src" -o "$obj" || { echo "failed: $src" >&2; exit 255; }'

echo ">>> the language module"
for src in "$here/lang/ps5_lang.c" "$here/lang/translation_es.c" "$out/lang/strings_es.c"; do
    $CC $ENGINE_FLAGS "$SAVE_PATH" -c "$src" -o "$out/obj/ps5/$(basename "$src" .c).o"
done

echo ">>> glue"
$CC -O2 -Wno-everything $DEFS $INCS -I"$root/src/pc/controller" -c "$here/glue/controller_entry_point_ps5.c" -o "$out/obj/engine/controller_entry_point_ps5.o"
$CC -O2 -fno-builtin -c "$here/glue/libc_shims.c" -o "$out/obj/ps5/libc_shims.o"
$CC $ENGINE_FLAGS -I"$here" -c "$here/glue/rumble_ps5.c" -o "$out/obj/ps5/rumble_ps5.o"
$CC -O2 -Wno-everything -c "$root/src/pc/gfx/gfx_cc.c" -o "$out/obj/ps5/gfx_cc.o"
$CC -O2 -Wno-everything $DEFS $INCS -c "$root/src/pc/audio/audio_null.c" -o "$out/obj/ps5/audio_null.o"

echo ">>> PS5 layer"
LAYER="main_ps5 menu_ps5 hd_textures asset_loader sha1 ps5gpu gfx_agc gfx_ps5 controller_ps5 audio_ps5"
for f in $LAYER; do
    $CC -O2 -Wall -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers \
        $DEFS -DENABLE_RUMBLE=1 -I"$here" $INCS $EXTRA_CFLAGS -c "$here/$f.c" -o "$out/obj/ps5/$f.o" 2>&1 | grep -v "third_party/stb_image.h" | grep -E "error|warning" || true
    [ "$out/obj/ps5/$f.o" -nt "$here/$f.c" ] || fail "compiling $f.c failed"
done

echo ">>> link"
rm -f "$out/sm64_ps5.elf"
objs=""
for f in $LAYER; do objs="$objs $out/obj/ps5/$f.o"; done
"$LINK" "$out/sm64_ps5.elf" "$CRT1" $objs "$out/obj/ps5/gfx_cc.o" "$out/obj/ps5/audio_null.o" "$out/obj/ps5/libc_shims.o" \
    "$out/obj/ps5/ps5_lang.o" "$out/obj/ps5/translation_es.o" "$out/obj/ps5/strings_es.o" \
    "$out/obj/ps5/rumble_ps5.o" \
    "$out"/obj/engine/*.o > "$out/link.log" 2>&1 || { tail -20 "$out/link.log"; fail "linking failed"; }
[ -s "$out/sm64_ps5.elf" ] || fail "linking produced nothing"

echo ">>> taking the ROM's data out of the program"
python3 "$here/asset_tools/asset_strip.py" "$out/sm64_ps5.elf" "$root/baserom.us.z64" "$root/assets.json" \
    "$out/sm64_ps5_stripped.elf" "$out/sm64_assets.map" | tail -1

echo ">>> HD texture index"
python3 "$here/hd_tools/hd_index.py" "$root/baserom.us.z64" "$root/assets.json" "$root/build/us_pc/bin" \
    "$out/sm64_hd_textures.idx" ${HD_PACK:+"$HD_PACK"}

echo ">>> signing"
rm -f "$out/eboot.bin"
(cd "$SHARPPROSPERO/tools/SharpProspero.Bindings.Generator" &&
    dotnet run -c Release -- self --sign --in "$out/sm64_ps5_stripped.elf" --out "$out/eboot.bin") > "$out/sign.log" 2>&1 || true
[ -s "$out/eboot.bin" ] || { tail -20 "$out/sign.log"; fail "signing failed"; }

echo ">>> package"
pkg=$out/$TITLE_ID
rm -rf "$pkg"
mkdir -p "$pkg/sce_sys" "$pkg/sce_module" "$pkg/data/sm64_ps5/saves"
cp "$out/eboot.bin" "$out/sm64_assets.map" "$out/sm64_hd_textures.idx" "$pkg/"
cp "$here/package/sce_sys/icon0.png" "$pkg/sce_sys/"
sed -e "s/@TITLE_ID@/$TITLE_ID/g" -e "s/@TITLE_NAME@/$TITLE_NAME/g" "$here/package/sce_sys/param.json" > "$pkg/sce_sys/param.json"
if [ -n "$LIBC_PRX" ]; then
    cp "$LIBC_PRX" "$pkg/sce_module/libc.prx"
else
    echo "!!! no LIBC_PRX given: put a signed libc.prx in $pkg/sce_module/ before copying the folder"
fi
echo "done: $pkg"
echo "copy baserom.us.z64 to $pkg/data/sm64_ps5/ (and optionally the texture pack to data/sm64_ps5/texturas_hd/)"
