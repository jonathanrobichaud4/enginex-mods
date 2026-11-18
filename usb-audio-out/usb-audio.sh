#!/bin/sh
# usb-audio-out: the deck's master as a USB sound card input on the computer
# connected to USB-B, in standalone mode.
#
# EngineX runs this with "start" (boot, enable) and "stop" (disable, uninstall).
#   start: frees the USB-B port, creates a USB Audio Class 2 gadget
#          ("EngineX Master Out", 48 kHz stereo, deck -> computer) and starts
#          usbaudio-pump, which feeds it EngineX's Live Stream master.
#   stop:  stops the pump, removes the gadget, and gives the port back to
#          whatever gadget had it before (Engine's own).
#
# MODE (file "mode" next to this script):
#   pump          the above (default)
#   engine-probe  create the gadget in the format Engine's own USB audio code
#                 looks for (32-bit, both directions) instead, start no pump,
#                 and copy Engine's reaction into the log. An experiment.
#
# PRE-MASTER (file "premaster" next to this script):
#   recorder  (default) the computer gets what Engine's set recorder gets,
#             taken where the recorder adds up its inputs, then run through
#             EngineX's own master effects (Echo Out, Pioneer FX, Auto-Gain,
#             limiter) with their current settings.
#   recorder-dry  the same without EngineX's master effects.
#   master    the main output node's input sum (0.2.0's tap point).
#   0         the computer gets what the deck outputs (after the master knob).
#   Both taps need a patched EngineX hook (lib/, one per EngineX release,
#   EngineX v5.3.0.4-r5 plus premaster-tap.patch).
#   Engine is pointed at the patched hook with a SC5000_UI_HOOK line in
#   /data/sc5000-custom/ui-scale.env, which Engine's unit reads at every start.
#   EngineX's own hook file is never changed. It is only used when EngineX's hook
#   is exactly 5.3.0.4's; anything else stays after the master knob, says so in
#   the log, and the line is taken out again. Disabling the mod removes the line.
#   The hook writes the level at each point to /tmp/enginex-premaster.status
#   every second ("sh usb-audio.sh status" shows it).
#
# Everything is logged to the system journal with the tag "usb-audio-out"
# (in EngineX's /api/logs download) and to /tmp/usb-audio-out.log.

ACTION="${1:-start}"
DIR="${ENGINEX_MOD_DIR:-$(cd "$(dirname "$0")" && pwd)}"
MODE="$(cat "$DIR/mode" 2>/dev/null | tr -d ' \r\n')"
[ -n "$MODE" ] || MODE=pump
CONFIGFS="${USBAUDIO_CONFIGFS:-/sys/kernel/config}"
UDC_CLASS="${USBAUDIO_UDC_CLASS:-/sys/class/udc}"
GADGETS=$CONFIGFS/usb_gadget
NAME=enginex_audio_out
G=$GADGETS/$NAME
RUN="${USBAUDIO_RUN:-/tmp}"
LOG=$RUN/usb-audio-out.log
PIDFILE=$RUN/usb-audio-out.pid
UNBOUND=$RUN/usb-audio-out.unbound
PREMASTER="$(cat "$DIR/premaster" 2>/dev/null | tr -d ' \r\n')"
# EngineX's own hook, which the patched one stands in for.
find_hook() {
    [ -n "${USBAUDIO_HOOK:-}" ] && { echo "$USBAUDIO_HOOK"; return; }
    for h in /data/enginex/bundle/lib/libengine-ui-hook.so /etc/sc5000/libengine-ui-hook.so; do
        [ -f "$h" ] && { echo "$h"; return; }
    done
    echo /data/enginex/bundle/lib/libengine-ui-hook.so
}
HOOK="$(find_hook)"
# Engine's unit reads this file at every start (EngineX's UI scale setting lives in
# it too); a SC5000_UI_HOOK line here makes Engine preload the patched hook from the
# first start of each boot, so no restart is needed after the first enable.
ENVFILE="${USBAUDIO_ENVFILE:-/data/sc5000-custom/ui-scale.env}"
HOOK_FLAG="${USBAUDIO_PREMASTER_FLAG:-/data/enginex/ui/live-premaster}"
# EngineX 5.3.0.4's libengine-ui-hook.so (data bundle and v5.3.0.4-r5), and the patched build of it.
# EngineX's own hook per release, and the patched build that stands in for it.
#   5.3.0.4 (r5):        lib/534/libengine-ui-hook.so  (premaster-tap.patch)
#   5.3.0.5 (beta1/rel): lib/535/libengine-ui-hook.so  (premaster-tap-535.patch)
# The file name must stay libengine-ui-hook.so: the hook only patches Engine
# when LD_PRELOAD contains that name, otherwise it loads the UI packs only.
pick_patched() {
    case "$1" in
        b743d9a239c84862dd5d0b904e59f1e9179152cd3fe9e43a23a4302822ad0700)
            HOOK_NEW="$DIR/lib/534/libengine-ui-hook.so"
            HOOK_NEW_SHA=0effb527e6232e4a9d471344a9567a513d4d7522d25f8b4c450bee250ffc94f4
            HOOK_RELEASE=5.3.0.4 HOOK_RELEASE_DIR=534 ;;
        2841ca5a4aaed0cbe468eae95ed5276ade5ea5a1dd910772103f460570a8dfad)
            HOOK_NEW="$DIR/lib/535/libengine-ui-hook.so"
            HOOK_NEW_SHA=64564117dbe0164c1ecfec4246414225459c9b4a5d85cef637a9ca6f2671af5a
            HOOK_RELEASE=5.3.0.5 HOOK_RELEASE_DIR=535 ;;
        *) return 1 ;;
    esac
}
HOOK_NEW=

log() {
    echo "$(date '+%H:%M:%S') $*" >> "$LOG"
    logger -t usb-audio-out -- "$*" 2>/dev/null || :
}

udc_name() {
    ls "$UDC_CLASS" 2>/dev/null | head -n 1
}

stop_pump() {
    if [ -f "$PIDFILE" ]; then
        pid=$(cat "$PIDFILE")
        kill "$pid" 2>/dev/null && log "stopped usbaudio-pump ($pid)"
        i=0
        while kill -0 "$pid" 2>/dev/null && [ $i -lt 2 ]; do sleep 1; i=$((i + 1)); done
        kill -9 "$pid" 2>/dev/null
        rm -f "$PIDFILE"
    fi
}

remove_gadget() {
    [ -d "$G" ] || return 0
    echo "" > "$G/UDC" 2>/dev/null
    rm -f "$G"/configs/c.1/uac2.usb0 2>/dev/null
    rmdir "$G"/configs/c.1/strings/0x409 2>/dev/null
    rmdir "$G"/configs/c.1 2>/dev/null
    rmdir "$G"/functions/uac2.usb0 2>/dev/null
    rmdir "$G"/strings/0x409 2>/dev/null
    rmdir "$G" 2>/dev/null
    [ -d "$G" ] && log "warning: could not fully remove the gadget" || log "removed the USB audio gadget"
}

rebind_previous() {
    udc=$(udc_name)
    [ -f "$UNBOUND" ] && [ -n "$udc" ] || return 0
    while read -r prev; do
        [ -n "$prev" ] && [ -d "$GADGETS/$prev" ] || continue
        if echo "$udc" > "$GADGETS/$prev/UDC" 2>/dev/null; then
            log "gave USB-B back to gadget '$prev'"
        else
            log "warning: could not give USB-B back to gadget '$prev' (it rebinds when the cable is reconnected)"
        fi
    done < "$UNBOUND"
    rm -f "$UNBOUND"
}

# Writes a configfs attribute; logs when the kernel refuses it.
attr() {
    if ! echo "$2" > "$1" 2>/dev/null; then
        log "note: kernel refused ${1##*/}=$2"
        return 1
    fi
}

create_gadget() {
    udc=$1
    mkdir -p "$G" || return 1
    attr "$G/idVendor" 0x1d6b
    attr "$G/idProduct" 0x0104
    attr "$G/bcdUSB" 0x0200
    attr "$G/bcdDevice" 0x0100
    mkdir -p "$G/strings/0x409"
    serial=$( { tr -d '\0' < /sys/firmware/devicetree/base/serial-number; } 2>/dev/null )
    attr "$G/strings/0x409/serialnumber" "${serial:-ENGINEX01}"
    attr "$G/strings/0x409/manufacturer" "EngineX OS"
    attr "$G/strings/0x409/product" "EngineX Master Out"
    mkdir -p "$G/configs/c.1/strings/0x409"
    attr "$G/configs/c.1/strings/0x409/configuration" "Audio"
    attr "$G/configs/c.1/MaxPower" 100
    attr "$G/configs/c.1/bmAttributes" 0xc0

    F="$G/functions/uac2.usb0"
    mkdir -p "$F" || { log "error: the kernel has no USB audio gadget function (uac2)"; return 1; }
    attr "$F/function_name" "EngineX Master Out" 2>/dev/null
    # Friendlier names for the computer's audio settings where the kernel allows it.
    # The stream's own "Capture Inactive" / "Capture Active" labels are fixed in this
    # kernel: macOS shows "Inactive" until an app records from the input.
    for a in if_ctrl_name p_it_name p_ot_name p_it_ch_name; do
        [ -e "$F/$a" ] && attr "$F/$a" "EngineX Master" 2>/dev/null
    done
    if [ "$MODE" = engine-probe ]; then
        # What Engine's az0x::usb::AudioGadget opens: S32_LE playback and capture
        # at one sample rate. Engine logs the channel count and rate it requires.
        attr "$F/p_chmask" 3; attr "$F/p_srate" 48000; attr "$F/p_ssize" 4
        attr "$F/c_chmask" 3; attr "$F/c_srate" 48000; attr "$F/c_ssize" 4
    else
        # Deck -> computer only: 48 kHz, 16-bit (the Live Stream master's format).
        attr "$F/p_chmask" 3; attr "$F/p_srate" 48000; attr "$F/p_ssize" 2
        if ! attr "$F/c_chmask" 0; then
            # Older kernels need a computer -> deck side too; keep it minimal.
            attr "$F/c_chmask" 3; attr "$F/c_srate" 48000; attr "$F/c_ssize" 2
        fi
    fi
    ln -s "$F" "$G/configs/c.1/" 2>/dev/null || { log "error: cannot add the audio function to the gadget"; return 1; }

    i=0
    while [ $i -lt 3 ]; do
        if echo "$udc" > "$G/UDC" 2>/dev/null; then
            log "USB-B is now 'EngineX Master Out' (mode $MODE, controller $udc)"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    log "error: could not attach the gadget to USB-B ($udc is busy)"
    return 1
}

hook_mounted() {
    grep -q " $HOOK " /proc/mounts 2>/dev/null
}

# Engine keeps the hook mapped while it runs, so a plain umount fails with "busy".
# A lazy one detaches the file now; Engine drops its old copy when it restarts.
unmount_hook() {
    i=0
    while hook_mounted && [ $i -lt 4 ]; do
        umount -l "$HOOK" 2>/dev/null || umount "$HOOK" 2>/dev/null || break
        i=$((i + 1))
    done
    ! hook_mounted
}

restart_engine_later() {
    systemctl is-active -q engine 2>/dev/null || return 0
    log "restarting Engine in 2 s so it loads the $1 hook"
    setsid sh -c 'sleep 2; systemctl restart engine' < /dev/null > /dev/null 2>&1 &
}

# 1 when the running Engine has the patched hook loaded.
engine_has_patched() {
    for pid in $(pidof Engine 2>/dev/null); do
        [ -n "$HOOK_NEW" ] && grep -qF "/usb-audio-out/lib/$HOOK_RELEASE_DIR/libengine-ui-hook.so" /proc/$pid/maps 2>/dev/null && return 0
    done
    return 1
}

# 1 when the running Engine has any patched hook of this mod (any release) loaded.
engine_has_any_patched() {
    for pid in $(pidof Engine 2>/dev/null); do
        grep -qE "/usb-audio-out/lib/53[0-9]/libengine-ui-hook.so|libengine-ui-hook-premaster" /proc/$pid/maps 2>/dev/null && return 0
    done
    return 1
}

engine_running() {
    [ -n "$(pidof Engine 2>/dev/null)" ]
}

# The line is SC5000_UI_HOOK=<hook>[:<extra library>...]: the first entry is the hook
# (ours or EngineX's), anything after a colon was added by another mod (fx-channel-indicator's
# band library) and is kept as it is.
hook_extras() {
    sed -n 's/^SC5000_UI_HOOK=[^:]*//p' "$ENVFILE" 2>/dev/null | head -n 1
}
hook_first() {
    sed -n 's/^SC5000_UI_HOOK=\([^:]*\).*/\1/p' "$ENVFILE" 2>/dev/null | head -n 1
}

# Sets the hook in the SC5000_UI_HOOK line (with no argument: back to EngineX's own, or no
# line at all if nothing else is in it), keeping every other line and any extra libraries.
set_env_hook() {
    tmp="$ENVFILE.usb-audio-out.$$"
    extras=$(hook_extras)
    { grep -v '^SC5000_UI_HOOK=' "$ENVFILE" 2>/dev/null
      if [ -n "$1" ]; then echo "SC5000_UI_HOOK=$1$extras"
      elif [ -n "$extras" ]; then echo "SC5000_UI_HOOK=$HOOK$extras"; fi
    } > "$tmp" || { rm -f "$tmp"; return 1; }
    if [ -s "$tmp" ]; then
        mv -f "$tmp" "$ENVFILE"
    else
        rm -f "$tmp" "$ENVFILE"
    fi
}

env_has_hook() {
    [ -n "$HOOK_NEW" ] && [ "$(hook_first)" = "$HOOK_NEW" ]
}

WATCHPID=$RUN/usb-audio-out-env.pid

# EngineX rewrites ui-scale.env (or deletes it at 100% scale) every time it saves
# its UI settings, dropping our line. Engine only reads the file when it starts,
# so a small loop puts the line back within 2 s of each rewrite.
start_env_watch() {
    stop_env_watch
    setsid sh "$0" watch-env "$HOOK_NEW" < /dev/null > /dev/null 2>&1 &
    echo $! > "$WATCHPID"
}

stop_env_watch() {
    [ -f "$WATCHPID" ] || return 0
    kill "$(cat "$WATCHPID")" 2>/dev/null
    rm -f "$WATCHPID"
}

watch_env() {
    want=$1
    [ -n "$want" ] || exit 0
    while :; do
        [ -f "$want" ] || { set_env_hook ""; exit 0; } # mod removed without a stop
        if [ "$(hook_first)" != "$want" ]; then
            HOOK_NEW=$want
            set_env_hook "$want" && log "pre-master: EngineX rewrote $ENVFILE; put the hook line back"
        fi
        sleep 2
    done
}

# Restart Engine only once the line is really there, so it starts on the patched hook.
restart_engine_patched() {
    systemctl is-active -q engine 2>/dev/null || return 0
    log "restarting Engine in 2 s so it loads the patched hook"
    setsid sh -c '
        sleep 2
        i=0
        while ! grep -q "^SC5000_UI_HOOK='"$HOOK_NEW"'\(:\|\$\)" "'"$ENVFILE"'" 2>/dev/null && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
        systemctl restart engine
    ' < /dev/null > /dev/null 2>&1 &
}

# 0.2.0 to 0.4.1 bind-mounted the patched hook over EngineX's; undo that.
remove_old_mount() {
    hook_mounted || return 0
    unmount_hook && log "pre-master: removed the bind mount older versions used" || log "pre-master: could not remove an old bind mount; it goes away at the next reboot"
}

premaster_on() {
    case "$PREMASTER" in master|master-dry|recorder-dry) src=$PREMASTER ;; *) src=recorder ;; esac
    echo "$src" > "$HOOK_FLAG" 2>/dev/null || log "warning: cannot create $HOOK_FLAG"
    log "pre-master: tap point $src"
    remove_old_mount
    have=$(sha256sum "$HOOK" 2>/dev/null | cut -d' ' -f1)
    if ! pick_patched "$have"; then
        log "pre-master: $HOOK is not a hook this mod has a patched build for (${have:-missing}); USB audio stays after the master knob"
        premaster_unset
        return 0
    fi
    new=$(sha256sum "$HOOK_NEW" 2>/dev/null | cut -d' ' -f1)
    if [ "$new" != "$HOOK_NEW_SHA" ]; then
        log "pre-master: lib/$HOOK_RELEASE_DIR/libengine-ui-hook.so is damaged; USB audio stays after the master knob"
        premaster_unset
        return 0
    fi
    log "pre-master: EngineX $HOOK_RELEASE hook found, using lib/$HOOK_RELEASE_DIR/libengine-ui-hook.so"
    start_env_watch
    if ! env_has_hook; then
        set_env_hook "$HOOK_NEW" || { log "pre-master: cannot write $ENVFILE; USB audio stays after the master knob"; return 0; }
        log "pre-master: Engine will load the patched hook at every start"
    fi
    if engine_has_patched; then
        log "pre-master: Engine started with the patched hook (no restart needed)"
    elif engine_running; then
        restart_engine_patched
    fi
}

# Takes the SC5000_UI_HOOK line out and gets Engine back on EngineX's own hook.
premaster_unset() {
    stop_env_watch
    if env_has_hook || grep -q '^SC5000_UI_HOOK=' "$ENVFILE" 2>/dev/null; then
        set_env_hook "" && log "pre-master: Engine goes back to EngineX's own hook"
    fi
    engine_has_any_patched && restart_engine_later stock
}

premaster_off() {
    rm -f "$HOOK_FLAG"
    remove_old_mount
    premaster_unset
}

do_start() {
    : > "$LOG"
    log "starting (mode $MODE)"
    modprobe libcomposite 2>/dev/null
    modprobe usb_f_uac2 2>/dev/null
    [ -d "$GADGETS" ] || mount -t configfs none "$CONFIGFS" 2>/dev/null
    if [ ! -d "$GADGETS" ]; then
        log "error: USB gadget support (configfs) is not available"
        return 0
    fi
    udc=$(udc_name)
    if [ -z "$udc" ]; then
        log "error: no USB device-mode controller found ($UDC_CLASS is empty)"
        return 0
    fi

    stop_pump
    remove_gadget

    if [ "$PREMASTER" = 0 ]; then premaster_off; else premaster_on; fi

    # Free the port: unbind any other gadget (Engine's own) and remember it.
    rm -f "$UNBOUND"
    for g in "$GADGETS"/*; do
        [ -d "$g" ] && [ "$g" != "$G" ] || continue
        if [ "$(cat "$g/UDC" 2>/dev/null)" = "$udc" ]; then
            echo "${g##*/}" >> "$UNBOUND"
            echo "" > "$g/UDC" 2>/dev/null && log "took USB-B from gadget '${g##*/}' (given back on stop)"
        fi
    done

    if ! create_gadget "$udc"; then
        remove_gadget
        rebind_previous
        return 0
    fi

    if [ "$MODE" = engine-probe ]; then
        # Give Engine time to notice the new sound card, then copy what it said.
        setsid sh -c '
            sleep 20
            journalctl -u engine --since "-2min" --no-pager 2>/dev/null |
                grep -i -E "gadget|UAC2|Playback Pitch|Capture Pitch" |
                while read -r line; do logger -t usb-audio-out -- "engine: $line"; echo "engine: $line" >> '"$LOG"'; done
            logger -t usb-audio-out -- "probe: done. If nothing above mentions the gadget, restart Engine and check again."
        ' < /dev/null > /dev/null 2>&1 &
        log "probe: waiting 20 s for Engine to react to the new sound card"
        return 0
    fi

    setsid "$DIR/bin/usbaudio-pump" -v ${USBAUDIO_PUMP_ARGS:-} < /dev/null >> "$LOG" 2>&1 &
    echo $! > "$PIDFILE"
    log "started usbaudio-pump ($!): set Live Stream to ON in the deck control centre"
    return 0
}

do_stop() {
    log "stopping"
    stop_pump
    remove_gadget
    rebind_previous
    premaster_off
    return 0
}

case "$ACTION" in
    start) do_start ;;
    stop) do_stop ;;
    watch-env) watch_env "${2:-}" ;;
    status)
        cat "$LOG" 2>/dev/null
        echo "--- hook taps (/tmp/enginex-premaster.status)"
        cat /tmp/enginex-premaster.status 2>/dev/null || echo "no status: the patched hook is not loaded"
        [ -d "$G" ] && echo "gadget: present, bound to '$(cat "$G/UDC" 2>/dev/null)'" || echo "gadget: absent"
        ;;
    *) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
exit 0
