#!/bin/sh
# Builds the rekordbox mod's binaries into ../ (none of them contain anything from Pioneer):
#
#   ../rekordbox-launcher   the launcher (static, 32-bit ARM hard float, runs on the deck itself)
#   ../timeout              rblive4's timeout helper for usb-watch.sh (static, hard float)
#   ../lib/*.so             rblive4's shims: fbshim (fbshim-tsc.c), knobshim (knobshim2.c), audioshim
#   ../lib/dfb/             DirectFB 1.4.16 with rblive4's directfb-full.diff (LGPL-2.1)
#
# The shims and DirectFB are loaded into the XDJ-RX3 player's soft-float glibc 2.13 userland, so
# they're built soft float and linked against Debian wheezy's armel glibc 2.13, which has the
# same symbol versions. rblive4 links them against the player's own libraries instead; this way
# nobody needs those to build.
#
# Ubuntu 24.04:
#   sudo apt install gcc-arm-linux-gnueabihf gcc-arm-linux-gnueabi autoconf automake libtool \
#       pkg-config patchelf git curl python3
#   ./build.sh            (RBLIVE4=path to an rblive4 checkout, else it's cloned at the pinned commit)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
MOD=$(cd "$HERE/.." && pwd)
WORK=${WORK:-$HERE/.work}
RBLIVE4_COMMIT=6003702930108cabcc0fbbf930a4b517bcd2fc94
DIRECTFB_COMMIT=363739298335a0f76dea2de1c9bc523293242baa   # 1.4.16, the tree directfb-full.diff is made against
FLUX_COMMIT=${FLUX_COMMIT:-}
VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$MOD/mod.json")
mkdir -p "$WORK"

# ---- sources
RB=${RBLIVE4:-$WORK/rblive4}
if [ ! -d "$RB/.git" ] && [ -z "${RBLIVE4:-}" ]; then
    git clone -q https://github.com/Malphite31/rblive4 "$RB"
    git -C "$RB" checkout -q "$RBLIVE4_COMMIT"
fi

# ---- launcher and timeout (hard float, static)
python3 - "$HERE/page.html" "$WORK/page.inc" <<'EOF'
import sys
html = open(sys.argv[1]).read()
out = []
for line in html.splitlines():
    out.append('"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"')
open(sys.argv[2], "w").write("\n".join(out) + "\n")
EOF
arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -static -pthread -DVERSION="\"$VERSION\"" -I"$WORK" \
    -o "$MOD/rekordbox-launcher" "$HERE/launcher.c"
arm-linux-gnueabihf-gcc -O2 -static -o "$MOD/timeout" "$RB/scripts/device/timeout.c"
arm-linux-gnueabihf-strip "$MOD/rekordbox-launcher" "$MOD/timeout"

# ---- glibc 2.13 (Debian wheezy armel) to link the soft-float parts against
SYS=$WORK/wheezy
if [ ! -f "$SYS/lib/arm-linux-gnueabi/libc.so.6" ]; then
    mkdir -p "$WORK/debs" "$SYS"
    for p in e/eglibc/libc6_2.13-38+deb7u10_armel.deb e/eglibc/libc6-dev_2.13-38+deb7u10_armel.deb \
             l/linux/linux-libc-dev_3.2.78-1_armel.deb; do
        f=$WORK/debs/${p##*/}
        [ -f "$f" ] || curl -sfL -o "$f" "http://archive.debian.org/debian/pool/main/$p"
        dpkg-deb -x "$f" "$SYS"
    done
    # absolute symlinks and linker scripts must point inside the sysroot
    find "$SYS" -type l -lname '/*' | while read -r l; do ln -sfn "$SYS$(readlink "$l")" "$l"; done
fi
L=$SYS/lib/arm-linux-gnueabi
SOFT="-O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector -fPIC"

# ---- shims: today's headers (they need the newer ALSA ones), wheezy's libraries
mkdir -p "$MOD/lib"
SH="$SOFT -shared -U_FILE_OFFSET_BITS -U_TIME_BITS -D_FILE_OFFSET_BITS=32 -D_TIME_BITS=32 -nodefaultlibs -w"
cc_shim() { out=$1; shift; arm-linux-gnueabi-gcc $SH -o "$MOD/lib/$out" "$@" "$HERE/isoc23.c" -lgcc; }
cc_shim fbshim.so "$RB/scripts/shims/fbshim-tsc.c" $L/librt.so.1 $L/libpthread.so.0 $L/libm.so.6 $L/libc.so.6
cc_shim knobshim.so "$RB/scripts/shims/knobshim2.c" $L/librt.so.1 $L/libpthread.so.0 $L/libm.so.6 $L/libc.so.6
cc_shim audioshim.so "$RB/scripts/shims/audioshim.c" $L/librt.so.1 $L/libdl.so.2 $L/libc.so.6
arm-linux-gnueabi-strip --strip-unneeded "$MOD"/lib/*.so

# ---- DirectFB 1.4.16 + rblive4's patch, all wheezy
CC_W="$WORK/cc-wheezy"
cat >"$CC_W" <<EOF
#!/bin/sh
exec arm-linux-gnueabi-gcc --sysroot=$SYS -isystem $SYS/usr/include -isystem $SYS/usr/include/arm-linux-gnueabi \
    -B$SYS/usr/lib/arm-linux-gnueabi -L$SYS/usr/lib/arm-linux-gnueabi -L$L -march=armv5t -mfloat-abi=soft "\$@"
EOF
chmod +x "$CC_W"
if ! command -v fluxcomp >/dev/null; then
    [ -x "$WORK/flux/src/fluxcomp" ] || {
        rm -rf "$WORK/flux"; git clone -q https://github.com/deniskropp/flux.git "$WORK/flux"
        [ -n "$FLUX_COMMIT" ] && git -C "$WORK/flux" checkout -q "$FLUX_COMMIT"
        (cd "$WORK/flux" && autoreconf -fi && ./configure -q && make -s -j"$(nproc)") >"$WORK/flux.log" 2>&1
    }
    PATH=$WORK/flux/src:$PATH
fi
D=$WORK/directfb
if [ ! -d "$D/.git" ]; then git clone -q https://github.com/deniskropp/DirectFB.git "$D"; fi
git -C "$D" checkout -q -f "$DIRECTFB_COMMIT"
git -C "$D" clean -qfdx
(cd "$D" && patch -s -p1 <"$RB/tools/build-directfb/directfb-full.diff")
echo '/* empty: nothing to forward with a glibc 2.13 sysroot */' >"$D/systems/fbdev/compat_shim.c"
(cd "$D" && autoreconf -fi && CC="$CC_W" CFLAGS=-O2 PKG_CONFIG_LIBDIR=/nonexistent ./configure \
    --host=arm-linux-gnueabi --prefix=/usr --libdir=/lib --disable-x11 --disable-sdl --disable-vnc \
    --with-gfxdrivers=none --with-inputdrivers=linuxinput,keyboard --disable-osx --disable-devmem \
    --disable-png --disable-jpeg --disable-gif --disable-freetype --disable-zlib --disable-sysfs &&
    make -j"$(nproc)" && rm -rf "$WORK/dfbout" && make install DESTDIR="$WORK/dfbout") >"$WORK/directfb.log" 2>&1 ||
    { tail -20 "$WORK/directfb.log"; exit 1; }
O=$MOD/lib/dfb
I=$WORK/dfbout/lib
rm -rf "$O"
mkdir -p "$O/directfb-1.4-6/systems" "$O/directfb-1.4-6/inputdrivers" "$O/directfb-1.4-6/wm"
for l in directfb direct fusion; do cp "$(ls "$I"/lib$l-1.4.so.*.*.* | head -1)" "$O/lib$l-1.4.so.0.0.0"; done
cp "$I/directfb-1.4-6/systems/libdirectfb_fbdev.so" "$O/directfb-1.4-6/systems/"
cp "$I/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so" "$I/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so" \
   "$O/directfb-1.4-6/inputdrivers/"
cp "$I/directfb-1.4-6/wm/libdirectfbwm_default.so" "$O/directfb-1.4-6/wm/"
# the player looks for libdirectfb-1.4.so.0 (the RX3's naming), not .so.6
for f in $(find "$O" -name '*.so*'); do
    for l in directfb direct fusion; do patchelf --replace-needed "lib$l-1.4.so.6" "lib$l-1.4.so.0" "$f"; done
done
for l in directfb direct fusion; do patchelf --set-soname "lib$l-1.4.so.0" "$O/lib$l-1.4.so.0.0.0"; done
arm-linux-gnueabi-strip --strip-unneeded $(find "$O" -name '*.so*')

# ---- check: soft float, nothing newer than glibc 2.7
bad=0
for f in "$MOD"/lib/*.so $(find "$O" -name '*.so*'); do
    v=$(arm-linux-gnueabi-objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -u | tr '\n' ' ')
    case "$v" in *GLIBC_2.[89]*|*GLIBC_2.[1-9][0-9]*) echo "!! $f needs $v"; bad=1 ;; esac
    arm-linux-gnueabi-readelf -A "$f" | grep -q Tag_ABI_VFP_args && { echo "!! $f is hard float"; bad=1; }
done
[ $bad = 0 ] || exit 1
ls -la "$MOD/rekordbox-launcher" "$MOD/timeout" "$MOD"/lib/*.so
du -sh "$O"
