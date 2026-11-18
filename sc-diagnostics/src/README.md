# sc-diagnostics source

A small C program (no Qt) that draws straight to the screen through KMS (or /dev/fb0), reads
the touchscreen through evdev, the control surface through its rawmidi port, and plays and
records through ALSA on the JP21 codec.

    ./build.sh            # needs arm-linux-gnueabihf-gcc and armhf libasound/libdrm headers

`gen_assets.py` turns `panel.json` (the control map from the sclive4-emulator) and DejaVu Sans
Bold into `assets.h`.

Run modes:

    sc-diagnostics            the app itself
    sc-diagnostics --serve    the launcher on port 8092 (GET / page, POST /start, POST /stop, GET /status)

Overrides, if the hardware doesn't match what it expects: DIAG_ROTATE (0/90/180/270),
DIAG_TOUCH (event device), DIAG_TOUCH_ROTATE, DIAG_MIDI (rawmidi device), DIAG_PCM (ALSA device),
DIAG_PORT, DIAG_STOP_ENGINE / DIAG_START_ENGINE (shell commands).
