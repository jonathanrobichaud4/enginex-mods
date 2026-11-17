#!/bin/sh
# Builds the installable mod packages (dist/<id>-<version>.tar.gz) for EngineX's Mod Hub.
#
# Mods that change Engine's own QML keep only their changes here, as patches against the stock
# files: Engine's QML is inMusic's code and isn't redistributed. To build, get the stock files
# from your own firmware with the emulator (https://github.com/jonathanrobichaud4/sclive4-emulator):
#
#   sudo ~/sclive4-emulator/emu.sh dump-qml ~/engine-qml
#   ./build.sh ~/engine-qml
#
# Needs: sh, python3, patch, tar. Patches are made against Engine 5.3.0 for SC LIVE 4.
set -eu

STOCK=${1:-}
[ -d "$STOCK" ] || { echo "usage: ./build.sh STOCK_QML_DIR   (see the top of this file)" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/dist
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

for dir in "$HERE"/*/; do
    [ -f "$dir/mod.json" ] || continue
    id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$dir/mod.json")
    ver=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$dir/mod.json")
    stage=$WORK/$id
    mkdir -p "$stage"
    # everything but the patches goes in as is
    (cd "$dir" && find . -path ./patches -prune -o -type f -print) | while read -r f; do
        mkdir -p "$stage/$(dirname "$f")"
        cp -p "$dir/$f" "$stage/$f"
    done
    # QML: stock file + patch -> the file named in mod.json's qmlFiles
    python3 -c 'import json,sys
for f, res in json.load(open(sys.argv[1])).get("qmlFiles", {}).items(): print(f + "\t" + res)' "$dir/mod.json" > "$WORK/qml.list"
    while IFS='	' read -r file res; do
        [ -f "$STOCK/$res" ] || { echo "missing stock file: $STOCK/$res" >&2; exit 1; }
        cp "$STOCK/$res" "$stage/$file"
        patch -s --no-backup-if-mismatch "$stage/$file" "$dir/patches/$file.patch" ||
            { echo "$id: $file.patch doesn't apply to your stock $res (different Engine version?)" >&2; exit 1; }
    done < "$WORK/qml.list"
    tar -czf "$OUT/$id-$ver.tar.gz" -C "$WORK" "$id"
    echo "built dist/$id-$ver.tar.gz"
done
