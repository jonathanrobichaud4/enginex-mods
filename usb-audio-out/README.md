# USB Audio Out 0.1.0 (experimental)

In standalone mode, makes the deck's USB-B port a USB sound card. A computer
connected to it sees an input called **EngineX Master Out** carrying the deck's
master: 48 kHz, 16-bit stereo. Class-compliant, so no driver is needed on
macOS, Windows 10/11 or Linux.

## Use

1. Install and enable this mod (Mods page of the web console on port 8080).
2. In the deck's control centre set **Live Stream** to **ON**. The master comes
   from EngineX's Live Stream tap, so nothing reaches the USB port while it is off
   (the computer gets silence).
3. Connect USB-B to the computer and pick **EngineX Master Out** as the input in
   your recording or streaming app.

## How it works

- `usb-audio.sh start` (run by EngineX at boot and when the mod is enabled)
  takes the USB-B port from any other gadget (Engine's own), creates a USB Audio
  Class 2 gadget (deck to computer only) and starts `bin/usbaudio-pump`.
- `usbaudio-pump` reads `http://127.0.0.1:8080/live.wav?format=pcm` (EngineX's
  Live Stream master) and writes it to the gadget's sound card. It keeps about
  60 ms of audio buffered and corrects the drift between the deck's and the
  USB clock with the gadget's "Playback Pitch" control (or by dropping or
  repeating a single sample when that control is missing). It plays silence
  while the stream is unavailable and recovers when the cable is replugged.
- `usb-audio.sh stop` (disable or uninstall) stops the pump, removes the gadget
  and gives the port back to the gadget that had it.

## While it is enabled

- Engine's own use of USB-B (sharing the deck's drives with the computer, the
  "computer connected" prompt) is unavailable. Disable the mod to get it back.
- Your main outputs are not touched: the pump only reads a copy of the master.

## Logs

Everything is logged with the tag `usb-audio-out` in the system journal (in the
web console's log download, `/api/logs`) and in `/tmp/usb-audio-out.log`, with
drift and buffer statistics every 10 seconds.

## Engine probe

The separate `usb-audio-engine-probe` package creates the gadget in the format
Engine's own built-in USB audio code looks for (32-bit, both directions) and
copies Engine's reaction from its log ("Using USB audio gadget", or the channel
count and sample rate it requires). It is an experiment to find out whether
Engine can feed USB-B itself; don't enable both packages at once.

## Source

`usbaudio-pump.c` is the helper's source. Built with
`arm-linux-gnueabihf-gcc -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard`
against the deck's `libasound.so.2` (alsa-lib 1.2.11, glibc 2.39).

## Tap point (0.3.0)

0.2.0 tapped the main output node before its own processing, and the master knob
still changed the level. 0.3.0 taps Engine's set recorder instead: the recorder is
its own node in Engine's mixer, and the hook copies its input right where the
recorder adds up its sources, before the recording gain and the file writer.

The file `premaster` picks the point:
- `recorder` (default): the set recorder's input, then EngineX's own master
  effects (Echo Out, Pioneer FX, Auto-Gain, limiter) with the same settings as
  the speakers get. They run on a separate copy with their own state, so the
  master knob still has no effect on the computer's level.
- `recorder-dry`: the set recorder's input with none of EngineX's effects.
- `master`: 0.2.0's point, kept for comparison.
- `0`: the deck's final output, after the master knob.

If the recorder node only runs while a recording is going, the hook falls back
to the final output after 32 silent blocks, so you never get dead air.

### Checking it

Every second the hook writes `/tmp/enginex-premaster.status`:

    hook_ready=1 source=recorder
    final blocks=123456 peak=0.4210
    master-sum blocks=123456 peak=0.6120
    recorder blocks=123456 peak=0.6120

`blocks` counts audio blocks seen at each point (it should keep going up), and
`peak` is the loudest left-channel sample in the last second. Play a track, turn
the master knob down, and run `sh usb-audio.sh status` (or `cat` the file) again.
Whichever point keeps the same peak is before the master knob. If the file is
missing, the patched hook did not load: the log says why.

Installing it: EngineX loads its hook from `/data/enginex/bundle/lib/libengine-ui-hook.so` (0.3.1 reads the path from Engine itself). On
enable, this mod checks that file is exactly the EngineX v5.3.0.4-r5 one, mounts
the patched build over it (nothing is written to the system partition) and
restarts Engine once. Disabling the mod unmounts it and restarts Engine again.
While the mod is enabled Engine restarts once at every boot. The patch is
`premaster-tap.patch`.

The Mono switch is still left out: Engine applies it after the master knob.

If Engine fails to start, EngineX's guard drops into safe mode with no hook, as
it does for any crash; disable this mod and reboot.

## 0.4.1

Updating from 0.3.1 to 0.4.0 left the 0.3.1 hook in place: Engine keeps the hook
file open while it runs, so the old one could not be unmounted. 0.4.1 detaches it
lazily (Engine lets go of it when it restarts), so updates and disabling take
effect without a reboot. The status file's first line now names the hook build,
for example `hook=usb-audio-out-0.4.1 ... source=recorder (with EngineX effects)`.

## 0.5.0: no extra Engine restart at boot

Up to 0.4.1 the mod mounted the patched hook after Engine had already started,
so every boot ran Engine twice. 0.5.0 instead adds one line,
`SC5000_UI_HOOK=<this mod>/lib/534 or 535/libengine-ui-hook.so`, to
`/data/sc5000-custom/ui-scale.env`, a file Engine's service reads every time it
starts (EngineX keeps its UI scale setting there too; that line is left alone).
From the second boot on, Engine starts with the patched hook straight away.
Engine still restarts once right after you first enable the mod.

Changing the UI scale in EngineX's settings rewrites that file without the line.
Nothing breaks: on the next boot the mod adds it back and restarts Engine once,
like 0.4.1 did.

"Capture Inactive" on a Mac is the stream's name from the kernel's USB audio
driver while no app is recording from it; it changes to "Capture Active" when an
app opens the input. This kernel has no setting to rename it.

## 0.6.0: EngineX 5.3.0.5

Adds a patched hook built from EngineX v5.3.0.5 (`lib/libengine-ui-hook-premaster-535.so`,
`premaster-tap-535.patch`); the 5.3.0.4 one stays. The mod picks the one that
matches EngineX's own hook and does nothing for any other release. In 5.3.0.5
EngineX turns master Echo Out off on every console (the decks have per-channel
Transition FX), and the USB copy of the effects follows that too.

## 0.6.1

EngineX rewrites `ui-scale.env` every time it saves its UI settings (and deletes
it at 100% scale), which dropped the hook line, so Engine went back to EngineX's
own hook and the USB audio followed the master knob again. A small background
loop now puts the line back within 2 seconds, and the one-time restart after
enabling waits until the line is there. Disabling the mod stops the loop and
removes the line.

## 0.6.2

The patched hooks are now `lib/534/libengine-ui-hook.so` and
`lib/535/libengine-ui-hook.so`. EngineX's hook only patches Engine (audio hooks,
streaming gateway, Stems and the rest) when the preloaded file is named
`libengine-ui-hook.so`; under 0.5.0 to 0.6.1's `libengine-ui-hook-premaster.so`
name it loaded the UI packs only, so the USB tap never ran and EngineX's own audio
features were off too. The status file now says `hook=usb-audio-out-0.6.2`.

## 0.6.3

Shares the `SC5000_UI_HOOK` line in `/data/sc5000-custom/ui-scale.env` with other mods.
The line is now `SC5000_UI_HOOK=<hook>[:<extra library>...]`: this mod only sets or
checks the first entry and keeps anything after a colon, so FX Channel Indicator's band
library (2.3.0) stays loaded when the pre-master tap turns on or off. With only extra
libraries left, the line goes back to EngineX's own hook plus those libraries instead of
being removed.
