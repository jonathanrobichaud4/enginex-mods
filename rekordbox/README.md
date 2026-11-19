# rekordbox

Runs the Pioneer XDJ-RX3 rekordbox player on the SC LIVE 4, using [rblive4](https://github.com/Malphite31/rblive4) by Malphite31. The mod adds a **rekordbox** entry to Settings > ABOUT / UPDATE, right under Hardware Diagnostics, and a page at `http://<deck-ip>:8093/`.

Engine stops while rekordbox runs, and starts again when you quit rekordbox:

- hold **SHIFT** and **BACK** together for two seconds, or
- press **Back to Engine** on the web page.

If rekordbox crashes, or doesn't finish starting within 30 seconds, Engine comes back by itself.

No Pioneer files come with this mod. You make the rekordbox part from your own XDJ-RX3 files once, on a computer.

## Setting it up

Getting the firmware: the XDJ-RX3 v1.20 update (`XDJ-RX3.UPD`) is encrypted. Decrypting it is up to you and isn't done by this mod or its tools; PrimeBox's [docs/01](https://github.com/chaparro2001/PrimeBox/blob/main/docs/01-firmware-extraction.md) explains how its `rx3dec` makes the ISO from it with a key you supply, and that this may be restricted where you live. The pack maker starts from that ISO.

1. Install the mod from the Mod Hub in the EngineX web console.
2. On your computer, get the `pc` folder (it's in the mod's repo, and as rekordbox-pack-maker.zip) and open `make-rekordbox-pack.py`: double-click it, or run `python3 make-rekordbox-pack.py`. A small window opens:
   1. Choose your firmware: the **decrypted XDJ-RX3 v1.20 ISO** (press ISO...), or a folder you already extracted (press Folder...). With the ISO, everything is read straight out of it, rootfs.cramfs included, so there's nothing else to unpack and no Docker or fusecram. A folder can be laid out the way rblive4's [docs/04](https://github.com/Malphite31/rblive4/blob/main/docs/04-firmware-assets.md) or PrimeBox's [docs/01](https://github.com/chaparro2001/PrimeBox/blob/main/docs/01-firmware-extraction.md) describe. It tells you right away if something's missing.
   2. Press **Make pack**. It applies rblive4's player patches itself, so the stock player works too.
   3. Type the deck's address (Settings > ABOUT / UPDATE, after Web Console) and press **Send to deck**.

   It only needs Python 3 (with Tkinter, which the python.org installers for Windows and macOS include). Without a window: `python3 make-rekordbox-pack.py /path/to/extracted --upload DECK-IP`.
3. Instead of sending it, you can also put `rekordbox-pack.tgz` on the top level of a USB drive, plug it into the deck and press **Set up** in Settings > ABOUT / UPDATE, or upload it on `http://<deck-ip>:8093/`.
4. Press **Start**.

The pack is unpacked to `/data/rbx3-run`, the same place rblive4 uses.

## What the mod does differently from rblive4's scripts

- Everything that isn't Pioneer's comes prebuilt with the mod: rblive4's shims (fbshim, knobshim, audioshim), DirectFB 1.4.16 with rblive4's patch, `usb-watch.sh` and `timeout`. You don't need a cross compiler, scp or SSH. `src/build.sh` builds them, linked against Debian wheezy's glibc 2.13, which has the same symbol versions as the XDJ-RX3's userland.
- The shims and DirectFB are copied into the chroot each time rekordbox starts, so updating the mod updates them.
- The device stubs rbp needs are bound over real devices inside the chroot only. If the deck has no device by that name, the stub is made in `/dev` and removed again afterwards. `/dev/mem` gets its permissions back when rekordbox quits.
- Engine and edisksd are started again whenever rekordbox ends, however it ends.

## Troubleshooting

- Logs: `/tmp/rekordbox.log` (and the journal, tag `rekordbox`); rbp's own output is in `/tmp/rekordbox/rbp.log`; USB drive events are in `/tmp/rekordbox/usbwatch.log`.
- `/data/rekordbox.env` (optional, shell syntax) can change a few things without rebuilding:
  - `RB_READY_WAIT=60` gives rbp longer to start.
  - `RB_PRELOAD=...` sets the shims rbp gets (default `/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so`).
  - `RB_ENV="SPEAKER_GAIN=0.5 JOG_VERBOSE=1"` passes more variables to rbp.
- Uninstalling the mod leaves `/data/rbx3-run` in place; delete it to get the space back.
- rblive4 was made on Engine OS 4.3.x, and EngineX is built on 5.3. If the screen, sound or controls misbehave, the cause is most likely there. rblive4's [troubleshooting page](https://github.com/Malphite31/rblive4/blob/main/docs/12-troubleshooting.md) is the place to start.

## How the button works

Engine only finds QML files it already has, so a new settings entry can't simply be added as a file. Instead, this mod and Hardware Diagnostics install the same About tab. It shows Hardware Diagnostics' entry, plus whatever the rekordbox launcher serves at `http://127.0.0.1:8093/entry.qml` (`StartRekordbox.qml`). When this mod isn't installed, that entry isn't there and the tab looks as before. Load order between the two mods doesn't matter.

## Credits and licences

- rblive4 (MIT): `usb-watch.sh`, the shim sources and the player patch tables in `pc/rblive4/` are from it, pinned at commit `6003702`. See `pc/rblive4/LICENSE`.
- DirectFB (LGPL-2.1): `lib/dfb/` is built from DirectFB 1.4.16 (github.com/deniskropp/DirectFB, commit `3637392`) with rblive4's `tools/build-directfb/directfb-full.diff`; `src/build.sh` rebuilds it.
- This mod is not affiliated with Pioneer DJ, AlphaTheta, Denon DJ or inMusic.
