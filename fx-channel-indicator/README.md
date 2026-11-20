# FX Channel Indicator 2.3.2

Three additions to the SC LIVE 4 FX bar:

1. **FX ASSIGN readout** next to the effect name, showing the channel the FX is
   on: `CH 1` to `CH 4`, or `MASTER`. Same font as the effect name (Roboto
   Medium, 21px, uppercase), left-aligned under its heading. Green while FX is on, white while it is off.
2. **Low / Mid / High frequency mode** (optional). Turn it on in
   **Settings > Fx > FX Frequency**. The frequency slider in the FX bar is then
   replaced by three tap buttons. Set it back to `Slider` for the stock control.
3. **Blinking CUE LED**: the headphone CUE button of the channel the FX is on
   blinks.

## CUE LED

While FX is ON and the rotary is on a channel, that channel's headphone CUE
(PFL) LED:

- blinks evenly (0.5 s on, 0.5 s off) when headphone cue is off;
- stays lit with a short dip once a second when headphone cue is on.

With FX off, or the rotary on MASTER (no CUE button), all LEDs behave normally. The blink is done in Engine's mixer controller module, on top of
EngineX's version of it (its stems-mode change is kept).

## Channel readout

The SC LIVE 4 picks the FX channel with a rotary selector. Its stock assignment
file maps that rotary to Engine's `DJFxAssignSwitch` module on MIDI CC 40
(positions Channel 3, 1, 2, 4, Main). Each move sets exactly one of
`/Engine/Mixer/Channel1..4/DJFx/Assigned` / `/Engine/Mixer/Main/DJFx/Assigned`
to true; the readout binds to those, so it follows the rotary immediately.
Stock Engine also switches the FX off whenever the rotary moves.

## Frequency buttons

Turn it on in **Settings > Fx > FX Frequency** (`Presets`; `Slider` is stock).
Engine's FX frequency control is one bipolar filter (`FilterCutoff`, 0 to 1), so
each button is a position on it:

| Button | Position | The effect is applied to |
|---|---|---|
| LOW  | 15/124 ≈ 0.12 | only below about 256 Hz (bass only) |
| MID  | 85/124 ≈ 0.69 | only above about 484 Hz (keeps the bass dry) |
| HIGH | 103/124 ≈ 0.83 | only above about 2.6 kHz (top end only) |
| ALL  | 0.5 | all bands, unfiltered |

Frequencies come from the table Engine uses for its own frequency readout. A
single filter can't isolate a middle band, so MID removes the bass rather than
the highs. The selected button is white; the header has Engine's green dot, lit
while the FX parameter knob is on Frequency.

**Knob:** while the mode is on, turning the FX parameter knob on Frequency moves
one button left or right (LOW, MID, HIGH, ALL) and stops dead at the ends.

- On a stepped FX knob, the knob handler (`SteppedTimeKnobOutput.qml`) jumps
  presets before the filter moves. It recognises Frequency by the label the FX
  bar records, and behaves like stock if anything there fails.
- On a continuous (jog-style) FX knob, which moves the filter directly, the FX
  bar catches it: any move off a preset snaps back onto one, with at most one
  preset step per 120 ms (counted from the last preset chosen), so the filter
  never runs past the presets and a button is always selected.

The control is a parameter type inside Engine's own FX parameter row
(`FxParameters.qml`), created and destroyed with each effect like the stock
slider. The mode is saved to `/data/enginex/ui/mod-fx-channel-indicator.ini`.

## Jog wheel reverse (2.1.0)

**Settings > Fx > Reverse Left Jog Wheel / Reverse Right Jog Wheel**, on
EngineX 5.3.0.4. Left covers decks 1 and 3, Right covers decks 2 and 4. The
choice is saved in `/data/enginex/ui/mod-fx-channel-indicator.ini` (the same
keys as 1.10.0, so an earlier setting carries over) and applied at startup.

It uses EngineX 5.3.0.5's own jog code: `JogWheel.qml` and
`GlobalAssignmentConfig.qml` are 5.3.0.5's files unchanged (5.3.0.5's
`GlobalAssignmentConfig.qml` is 5.3.0.4's plus the jog lines only), and the
switches feed them through the same `/GUI/Scripted/EngineXJogInvertLeft` /
`Right` / `Ready` properties EngineX's own switches use. Unlike 1.10.0, it no
longer replaces EngineX's main screen or device settings files.

On EngineX 5.3.0.5 the two switches hide themselves and do nothing: EngineX's
built-in ones under Settings > Device are used instead.

## Settings between the screens and the controller code

Engine's controller code runs in its own QML engine. The Low / Mid / High / All
mode is published as a Planck property, `/GUI/Scripted/EngineXModFxPresets`
(created with `Planck.createProperty`, as EngineX 5.3.0.5 does for its jog
switches), which the FX knob handler reads. The mod's settings file is the
fallback.

## Files

| Mod file | Replaces |
|---|---|
| `DJFx.qml` | `Skins/Embedded/Views/MainView/Fx/DJFx.qml` (EngineX copy, identical in 5.3.0.4 and 5.3.0.5, PX selector kept) |
| `MixerChannelCore.qml` | `ControlSurfaceModules/ControlSurfaceModules/MixerChannelCore.qml` (EngineX copy, identical in 5.3.0.4 and 5.3.0.5) |
| `FxParameters.qml` | `Skins/Embedded/Views/MainView/Fx/FxParameters.qml` (stock 5.3.0) |
| `SteppedTimeKnobOutput.qml` | `ControlSurfaceModules/ControlSurfaceModules/private/SteppedTimeKnobOutput.qml` (stock 5.3.0) |
| `JogWheel.qml` | `ControlSurfaceModules/ControlSurfaceModules/JogWheel.qml` (EngineX 5.3.0.5, unchanged) |
| `GlobalAssignmentConfig.qml` | `ControlSurfaceModules/ControlSurfaceModules/GlobalAssignmentConfig.qml` (EngineX 5.3.0.5, unchanged) |
| `FxTabContent.qml` | `Skins/Embedded/Views/ControlCenter/Tabs/Preferences/FxTabContent.qml` (stock 5.3.0) |

Both replace whole files. Disable this Mod before updating EngineX OS or Engine
and re-check it against the new release. Restart Engine after enabling or
disabling. On send-style consoles (PRIME 4) both additions stay hidden.

Not hardware-validated yet.

## 2.0.1

Touching LOW / MID / HIGH / ALL on the screen now works. 2.0.0 used a passive tap
handler, so whatever sits under the FX bar took the touch first. Each preset is
now a MouseArea that takes the touch on press, like Engine's own slider, and it
reaches 30 px above and below the button so it is easy to hit.

## 2.2.0: combining bands

The LOW / MID / HIGH buttons now combine. Tap a band to add it or take it away;
ALL selects all three. The FX knob steps through the combinations in slider
order: LOW, LOW + MID, ALL, MID + HIGH, HIGH.

| Selection | Engine frequency | Effect covers |
|---|---|---|
| LOW | < 256 Hz | the bass |
| LOW + MID | < 2.6 kHz | everything but the top end |
| ALL | centre | everything |
| MID + HIGH | > 256 Hz | everything but the bass |
| HIGH | > 2.6 kHz | the top end |

Engine's frequency control is one slider (lows below the centre, highs above),
so LOW + HIGH without MID and MID on its own can't be made: tapping HIGH while
LOW is on also turns MID on, and a tap that would leave a gap or nothing
changes nothing. MID on its own used to mean "> 484 Hz"; that is now MID + HIGH
at 256 Hz.

## 2.3.0: any combination of bands

LOW / MID / HIGH now combine freely: LOW + HIGH with MID left dry, MID on its own,
anything. Tap a band to switch it on or off (the last one can't go off); ALL turns on
all three. While the FX is on, the selected bands are green; while it's off, white.

The FX knob (on Frequency) steps through every combination and stops at the ends
(2.3.1 adds none at the start, see below):

    LOW, LOW + MID, MID, MID + HIGH, HIGH, LOW + HIGH, ALL

**How.** Engine's frequency control is one filter, so it can't leave the middle dry.
The mod now ships a small library, `lib/libfxbands.so` (source in `src/fxbands.c`),
which Engine loads next to EngineX's hook. It hooks the FX unit's own process function
(dry block in, finished block out), keeps a copy of the dry input,
splits both the dry and the finished sound into three bands with Linkwitz-Riley crossovers
at 256 Hz and 2.6 kHz (the points Engine's own frequency readout uses), and puts them back
together with the effect only in the selected bands. Switching a band fades over 15 ms.
With all three bands on, the effected sound passes through untouched.

In band mode the frequency control stays at its centre. The selection goes to the library
through `/tmp/enginex-fxbands` and is kept in the mod's settings file. The knob handler
nudges the frequency one step to say which way the knob turned; the FX bar reads that,
picks the next combination and puts the frequency back at the centre.

**Loading.** `fx-bands.sh` adds the library to the UI hook line Engine preloads from
(`<hook>:<mod>/lib/libfxbands.so`): `ENGINEX_UI_HOOK` in `/data/enginex/ui-scale.env` on
EngineX 5.3.0.5 and later, `SC5000_UI_HOOK` in `/data/sc5000-custom/ui-scale.env` before
(2.3.2 reads which from Engine's launcher and unit), puts it back if
EngineX rewrites the file, and restarts Engine once if it is running without it (first
enable). On stop it takes the library off the line. USB Audio Out 0.6.3 or later keeps
the library on that line too; older versions overwrite it.

If the library isn't running (another Engine build, or it couldn't hook in), the bar
falls back to 2.2.0's combinations. `/tmp/enginex-fxbands.status` says whether it hooked
in (`installed=1`). Tested in the emulator (the band split measured on Engine's own FX
chain); not hardware-validated yet.

## 2.3.1: the bands actually apply; none selected

2.3.0's library sat on the FX chain function EngineX wraps, which turned out to see the
sound after the effect had already been mixed in: it put "effected" bands back over
"effected" bands, so every band always sounded on. 2.3.1 hooks the FX unit's own process
function instead (Engine 5.3.0 for SC LIVE 4 has three versions of it, at 0xe78008,
0xe78130 and 0xe9f73c), whose input is the clean channel signal; measured on an echo tail
in the emulator, unselected bands now drop by 22 to 37 dB. It no longer depends on
EngineX's FX hook. FX units that are idle (output equal to input) are left alone.

Every band can be switched off now: tap the last lit band, or tap ALL when all three are
on. With none selected the effect isn't heard (it keeps running, so its tail comes back
with the first band). The knob order starts at none:

    none, LOW, LOW + MID, MID, MID + HIGH, HIGH, LOW + HIGH, ALL

## 2.3.2: EngineX 5.3.0.5

EngineX 5.3.0.5 renamed the variable Engine preloads from (`ENGINEX_UI_HOOK`), moved the
file it's read from (`/data/enginex/ui-scale.env`) and moved its hook
(`/etc/enginex/libengine-ui-hook.so`). 2.3.1 still wrote the old line to the old file, so
on 5.3.0.5 the band library never loaded and the bar fell back to 2.2.0's combinations: no
LOW + HIGH, no MID alone, no "none". 2.3.2 works out which scheme the installed EngineX uses
(from Engine's launcher and its unit) and writes that; 5.3.0.4 is handled as before.
`sh fx-bands.sh status` shows the file and variable it uses and whether Engine has the
library.
