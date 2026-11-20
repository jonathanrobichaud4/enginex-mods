# EngineX mods for the Denon SC LIVE 4

Mods for [EngineX OS](https://github.com/Cd1s/EngineX-OS) on the Denon DJ SC LIVE 4, built for EngineX 5.3.0.5 (v5.3.0.5-r1). The screen files are rebased on 5.3.0.5's, so on 5.3.0.4 use the earlier releases. FX Channel Indicator and USB Audio Out work out which way the installed EngineX loads hook libraries, so their scripts handle both. Install them through the Mod Hub in the EngineX web console.

| Mod | Version | What it does |
|---|---|---|
| [USB Audio Out](usb-audio-out/) | 0.7.0 | Sends the master (before the master volume, with EngineX's master effects) to a computer on the USB-B port as a 48 kHz stereo sound card input. |
| [FX Channel Indicator](fx-channel-indicator/) | 2.4.0 | Shows which channel the FX unit is on, blinks that channel's CUE LED, adds a band mode for the FX frequency control (LOW / MID / HIGH in any combination, LOW + HIGH included, green while the FX is on), and built for EngineX 5.3.0.5 (reverse jog is EngineX's own there). |
| [System Stats](system-stats/) | 1.1.1 | CPU use, RAM use and CPU temperature next to the clock. |
| [Streaming Icons](streaming-icons/) | 1.2.0 | Shows each track's real service icon (Spotify, Deezer, NetEase, SoundCloud, Beatport, Apple Music, USB/SD) in the browser and on the decks, per track, instead of TIDAL on everything EngineX plays through TIDAL. |
| [Hardware Diagnostics](sc-diagnostics/) | 1.2.0 | A test app for the deck's hardware: controls light up as you use them, LED and pad colour tests, jog displays, tones to Main/Booth/Phones with input meters, screen patterns and a touch grid. Start it from Settings > ABOUT / UPDATE or http://<deck-ip>:8092/. Engine stops while it runs and comes back on EXIT (or hold BROWSE for 3 s). |
| [rekordbox](rekordbox/) | 1.1.0 | Runs the XDJ-RX3 rekordbox player ([rblive4](https://github.com/Malphite31/rblive4)) on the deck. Start it from Settings > ABOUT / UPDATE, under Hardware Diagnostics, or http://<deck-ip>:8093/. Engine stops while it runs and comes back when you hold SHIFT + BACK for 2 s. You make its pack once from your own XDJ-RX3 files with a small Python app that also sends it to the deck. |

`tools/streaming/` has the diagnostic scripts used to debug the streaming services (they mask tokens and passwords in their reports).

## Building

FX Channel Indicator, System Stats and Streaming Icons change some of Engine's own QML. Engine's QML belongs to inMusic, so this repo only stores the changes, as patches against the stock files, and you build the packages from your own firmware. The easiest way to get the stock files is the [SC LIVE 4 emulator](https://github.com/jonathanrobichaud4/sclive4-emulator):

```sh
sudo ~/sclive4-emulator/emu.sh dump-qml ~/engine-qml     # stock QML from your firmware
./build.sh ~/engine-qml ~/enginex-5.3.0.5-pack/en   # -> dist/<mod>-<version>.tar.gz
```

The second folder is EngineX 5.3.0.5's UI pack (MIT licensed): on a deck it's `/data/enginex/ui/packs/en`, or build it from EngineX's source at tag v5.3.0.5-r1 with `tools/ui-mod/build_packs.py --qrc ~/engine-qml --out ~/enginex-5.3.0.5-pack --release`. Files EngineX changes are patched against the pack; each mod's `patches/enginex-base` lists them. The rest are patched against stock Engine QML (the first argument).

The patches are made against Engine 5.3.0 for SC LIVE 4. Upload the packages in `dist/` through the Mod Hub.

USB Audio Out has no Engine code in it and works as is from its folder. Hardware Diagnostics and rekordbox install the same About tab, which shows each one's entry when that mod is installed, so either can be installed alone or both together. rekordbox's prebuilt parts (launcher, rblive4's shims, patched DirectFB) are rebuilt with `rekordbox/src/build.sh`. USB Audio Out's `lib/534`, `lib/535` and `lib/535r1` hooks are EngineX's `libengine-ui-hook.so` (v5.3.0.4-r5, v5.3.0.5-beta1, v5.3.0.5-r1) built with `premaster-tap.patch` / `premaster-tap-535.patch` / `premaster-tap-535-r1.patch`; EngineX is MIT licensed (see `usb-audio-out/LICENSE-EngineX`).

## Testing without a deck

The [emulator](https://github.com/jonathanrobichaud4/sclive4-emulator) runs Engine and EngineX on a PC, with the whole control surface in a browser, so mods can be tried before they go on the deck.
