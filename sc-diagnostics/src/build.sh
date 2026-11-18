#!/bin/sh
# Builds ../sc-diagnostics for the SC LIVE 4 (32-bit ARM, hard float). Ubuntu 24.04:
#   sudo dpkg --add-architecture armhf   (with ports.ubuntu.com for armhf, see the emulator's
#                                         tools/build_mesa_virgl.sh)
#   sudo apt install crossbuild-essential-armhf libasound2-dev:armhf libdrm-dev:armhf python3-pil fonts-dejavu-core
# The deck has glibc 2.39, libasound.so.2 and libdrm.so.2, the same as Ubuntu 24.04's armhf.
set -e
cd "$(dirname "$0")"
python3 gen_assets.py panel.json /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf > assets.h
arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -march=armv7-a -mfpu=neon -mfloat-abi=hard -I/usr/include/libdrm \
    -o ../sc-diagnostics diag.c gfx.c io.c -lasound -ldrm -lm -lpthread
arm-linux-gnueabihf-strip ../sc-diagnostics
ls -la ../sc-diagnostics
