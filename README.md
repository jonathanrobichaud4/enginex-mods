# EngineX mods for the Denon SC LIVE 4

Mods for [EngineX OS](https://github.com/Cd1s/EngineX-OS) on the Denon DJ SC LIVE 4, built and tested on EngineX 5.3.0.4 (USB Audio Out also supports 5.3.0.5). Install them through the Mod Hub in the EngineX web console.

| Mod | Version | What it does |
|---|---|---|
| [USB Audio Out](usb-audio-out/) | 0.6.2 | Sends the master (before the master volume, with EngineX's master effects) to a computer on the USB-B port as a 48 kHz stereo sound card input. |
| [FX Channel Indicator](fx-channel-indicator/) | 2.2.0 | Shows which channel the FX unit is on, blinks that channel's CUE LED, adds a band mode for the FX frequency control (LOW / MID / HIGH, combinable), and reverse jog wheel switches on 5.3.0.4. |
| [System Stats](system-stats/) | 1.1.0 | CPU use, RAM use and CPU temperature next to the clock. |

`tools/streaming/` has the diagnostic scripts used to debug the streaming services (they mask tokens and passwords in their reports).

## Building

FX Channel Indicator and System Stats change some of Engine's own QML. Engine's QML belongs to inMusic, so this repo only stores the changes, as patches against the stock files, and you build the packages from your own firmware. The easiest way to get the stock files is the [SC LIVE 4 emulator](https://github.com/jonathanrobichaud4/sclive4-emulator):

```sh
sudo ~/sclive4-emulator/emu.sh dump-qml ~/engine-qml     # stock QML from your firmware
./build.sh ~/engine-qml                                  # -> dist/<mod>-<version>.tar.gz
```

The patches are made against Engine 5.3.0 for SC LIVE 4. Upload the packages in `dist/` through the Mod Hub.

USB Audio Out has no Engine code in it and works as is from its folder. Its `lib/534` and `lib/535` hooks are EngineX's `libengine-ui-hook.so` built with `premaster-tap.patch` / `premaster-tap-535.patch`; EngineX is MIT licensed (see `usb-audio-out/LICENSE-EngineX`).

## Testing without a deck

The [emulator](https://github.com/jonathanrobichaud4/sclive4-emulator) runs Engine and EngineX on a PC, with the whole control surface in a browser, so mods can be tried before they go on the deck.
