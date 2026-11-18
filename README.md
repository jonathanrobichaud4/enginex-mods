# EngineX mods for the Denon SC LIVE 4

Mods for [EngineX OS](https://github.com/Cd1s/EngineX-OS) on the Denon DJ SC LIVE 4, built and tested on EngineX 5.3.0.4 (USB Audio Out also supports 5.3.0.5). Install them through the Mod Hub in the EngineX web console.

| Mod | Version | What it does |
|---|---|---|
| [USB Audio Out](usb-audio-out/) | 0.6.3 | Sends the master (before the master volume, with EngineX's master effects) to a computer on the USB-B port as a 48 kHz stereo sound card input. |
| [FX Channel Indicator](fx-channel-indicator/) | 2.3.1 | Shows which channel the FX unit is on, blinks that channel's CUE LED, adds a band mode for the FX frequency control (LOW / MID / HIGH in any combination, LOW + HIGH included, green while the FX is on), and reverse jog wheel switches on 5.3.0.4. |
| [System Stats](system-stats/) | 1.1.0 | CPU use, RAM use and CPU temperature next to the clock. |
| [Streaming Icons](streaming-icons/) | 1.1.0 | Shows each track's real service icon (Spotify, Deezer, NetEase, SoundCloud, Beatport, Apple Music, USB/SD) in the browser and on the decks, per track, instead of TIDAL on everything EngineX plays through TIDAL. |
| [Hardware Diagnostics](sc-diagnostics/) | 1.1.0 | A test app for the deck's hardware: controls light up as you use them, LED and pad colour tests, jog displays, tones to Main/Booth/Phones with input meters, screen patterns and a touch grid. Start it from Settings > ABOUT / UPDATE or http://<deck-ip>:8092/. Engine stops while it runs and comes back on EXIT (or hold BROWSE for 3 s). |

`tools/streaming/` has the diagnostic scripts used to debug the streaming services (they mask tokens and passwords in their reports).

## Building

FX Channel Indicator, System Stats and Streaming Icons change some of Engine's own QML. Engine's QML belongs to inMusic, so this repo only stores the changes, as patches against the stock files, and you build the packages from your own firmware. The easiest way to get the stock files is the [SC LIVE 4 emulator](https://github.com/jonathanrobichaud4/sclive4-emulator):

```sh
sudo ~/sclive4-emulator/emu.sh dump-qml ~/engine-qml     # stock QML from your firmware
./build.sh ~/engine-qml ~/sclive4-emulator/rootfs/data/enginex/ui/packs/en   # -> dist/<mod>-<version>.tar.gz
```

The second folder is EngineX's UI pack (MIT licensed). Streaming Icons also changes two of EngineX's own files, so its patches for those are against the pack; on a deck it's `/data/sc5000-custom/ui/packs/en`. The other mods only need the first argument.

The patches are made against Engine 5.3.0 for SC LIVE 4. Upload the packages in `dist/` through the Mod Hub.

USB Audio Out has no Engine code in it and works as is from its folder. Its `lib/534` and `lib/535` hooks are EngineX's `libengine-ui-hook.so` built with `premaster-tap.patch` / `premaster-tap-535.patch`; EngineX is MIT licensed (see `usb-audio-out/LICENSE-EngineX`).

## Testing without a deck

The [emulator](https://github.com/jonathanrobichaud4/sclive4-emulator) runs Engine and EngineX on a PC, with the whole control surface in a browser, so mods can be tried before they go on the deck.
