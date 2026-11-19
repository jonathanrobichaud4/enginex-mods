#!/bin/sh
# rekordbox: runs the XDJ-RX3 rekordbox player (rblive4) on the SC LIVE 4.
#
# EngineX runs this with "start" (boot, enable) and "stop" (disable, uninstall). The rest is
# for the launcher (rekordbox-launcher --serve, port 8093), which the Settings button and the
# web page talk to:
#   check          work out the state: "missing" (no pack yet) or "ready"
#   setup          find rekordbox-pack.tgz on a USB drive and install it
#   install FILE   check and install a pack
#   session        stop Engine, run rekordbox until it quits, start Engine again
#   end            quit rekordbox (the session then brings Engine back)
#
# The pack (made on a computer by make-rekordbox-pack.py from your own XDJ-RX3 files) is the
# chroot rblive4 runs in, /data/rbx3-run. Everything that isn't Pioneer's (the shims, the
# patched DirectFB, the scripts) comes with this mod and is copied into the chroot each start.
# Logs: /tmp/rekordbox.log (and the journal, tag "rekordbox").

ACTION="${1:-start}"
DIR="${ENGINEX_MOD_DIR:-$(cd "$(dirname "$0")" && pwd)}"
ROOT="${RB_ROOT:-/data/rbx3-run}"
STATE_DIR=/tmp/rekordbox
STATE=$STATE_DIR/state
LOG=/tmp/rekordbox.log
UNIT=rekordbox-launcher
# Optional overrides for troubleshooting: RB_READY_WAIT, RB_PRELOAD (the shims rbp gets),
# RB_ENV (more variables for rbp, e.g. "SPEAKER_GAIN=0.5 JOG_VERBOSE=1")
[ -f /data/rekordbox.env ] && . /data/rekordbox.env
READY_WAIT="${RB_READY_WAIT:-30}"
PRELOAD="${RB_PRELOAD:-/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so}"
mkdir -p "$STATE_DIR"

log() { echo "$(date '+%F %T') $*" >>"$LOG"; logger -t rekordbox "$*" 2>/dev/null || true; }
state() { printf '%s\n%s\n' "$1" "$2" >"$STATE.new" && mv "$STATE.new" "$STATE"; log "state: $1 $2"; }

installed() { [ -f "$ROOT/.rekordbox-pack" ] && [ -f "$ROOT/root/pdj/rbp" ] && [ -e "$ROOT/lib/ld-linux.so.3" ]; }
pack_info() { head -n 1 "$ROOT/.rekordbox-pack" 2>/dev/null; }

# ---------------------------------------------------------------- the launcher
binary() {
    # some storage is mounted noexec; fall back to a copy in /tmp
    b="$DIR/rekordbox-launcher"
    chmod 755 "$b" "$DIR/timeout" 2>/dev/null
    if "$b" --version >/dev/null 2>&1; then echo "$b"; return; fi
    cp "$b" /tmp/rekordbox-launcher.bin && chmod 755 /tmp/rekordbox-launcher.bin && echo /tmp/rekordbox-launcher.bin
}

launcher_stop() {
    systemctl stop "$UNIT" 2>/dev/null
    pkill -f "rekordbox-launcher.* --serve" 2>/dev/null
    return 0
}

# ---------------------------------------------------------------- installing a pack
check() {
    if installed; then state ready "$(pack_info)"; else state missing "No rekordbox pack on the deck yet."; fi
}

install_pack() {
    f=$1
    [ -f "$f" ] || { state error "Can't find $f."; return 1; }
    state installing "Checking the pack..."
    if ! zcat "$f" | tar t 2>/dev/null | grep -qx './\.rekordbox-pack\|\.rekordbox-pack'; then
        state error "That file isn't a rekordbox pack. Make one with make-rekordbox-pack.py."
        return 1
    fi
    need=$(( $(wc -c <"$f") * 3 / 1024 ))
    free=$(df -k "$(dirname "$ROOT")" | awk 'NR==2 {print $4}')
    if [ -n "$free" ] && [ "$free" -lt "$need" ]; then
        state error "Not enough free space on /data (about $((need / 1024)) MB needed)."
        return 1
    fi
    state installing "Unpacking (this takes a minute)..."
    unmount_chroot
    rm -rf "$ROOT.new"
    mkdir -p "$ROOT.new"
    if ! zcat "$f" | tar x -C "$ROOT.new" 2>>"$LOG"; then
        rm -rf "$ROOT.new"
        state error "Unpacking failed (is the file complete?)."
        return 1
    fi
    chown -R root:root "$ROOT.new" 2>/dev/null
    rm -rf "$ROOT.old"
    [ -d "$ROOT" ] && mv "$ROOT" "$ROOT.old"
    mv "$ROOT.new" "$ROOT"
    rm -rf "$ROOT.old"
    case "$f" in /data/rekordbox-pack.tgz) rm -f "$f" ;; esac
    if installed; then state ready "Installed: $(pack_info)"; else state error "The pack is missing parts. Make it again."; return 1; fi
}

# A pack on a USB drive: drives Engine has mounted (/media/<label>), else any partition we
# can mount read-only for a moment.
find_pack() {
    for d in /media/* /run/media/* /run/media/*/*; do
        [ -d "$d" ] || continue
        for p in "$d"/rekordbox-pack*.tgz "$d"/rekordbox/rekordbox-pack*.tgz; do
            [ -f "$p" ] && { echo "$p"; return 0; }
        done
    done
    return 1
}

setup() {
    state working "Looking for rekordbox-pack.tgz on a USB drive..."
    p=$(find_pack)
    if [ -z "$p" ]; then
        mnt=$STATE_DIR/usb
        mkdir -p "$mnt"
        for dev in /dev/sd[a-z][0-9] /dev/sd[a-z] /dev/mmcblk1p[0-9]; do
            [ -b "$dev" ] || continue
            grep -q "^$dev " /proc/mounts && continue
            mount -o ro "$dev" "$mnt" 2>/dev/null || continue
            for c in "$mnt"/rekordbox-pack*.tgz "$mnt"/rekordbox/rekordbox-pack*.tgz; do
                [ -f "$c" ] || continue
                state installing "Copying the pack from the USB drive..."
                cp "$c" /data/rekordbox-pack.tgz && p=/data/rekordbox-pack.tgz
                break
            done
            umount "$mnt"
            [ -n "$p" ] && break
        done
    fi
    if [ -z "$p" ]; then
        if installed; then state ready "$(pack_info)"; return 0; fi
        ip=$(ip -4 -o addr show 2>/dev/null | awk '$2 != "lo" {sub("/.*", "", $4); print $4; exit}')
        [ -n "$ip" ] || ip="the deck's address"
        state error "No rekordbox-pack.tgz on a USB drive. Put it on the top level of the drive, or upload it at http://$ip:8093/"
        return 1
    fi
    log "pack found: $p"
    install_pack "$p"
}

# ---------------------------------------------------------------- the chroot
CHROOT_BINDS="dev proc sys tmp"
FIFOS="subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0"
FILES="printkdrv0 tsc2007_2-0048 gpiodrv"

unmount_chroot() {
    [ -d "$ROOT" ] || return 0
    for m in $(awk -v r="$ROOT/" 'index($2, r) == 1 {print $2}' /proc/mounts | sort -r); do umount -l "$m" 2>/dev/null; done
}

chroot_pids() {
    for p in /proc/[0-9]*; do
        [ "$(readlink "$p/root" 2>/dev/null)" = "$ROOT" ] && echo "${p#/proc/}"
    done
}

kill_chroot() {
    pids=$(chroot_pids)
    [ -n "$pids" ] && kill $pids 2>/dev/null
    sleep 1
    pids=$(chroot_pids)
    [ -n "$pids" ] && kill -9 $pids 2>/dev/null
    return 0
}

# What rblive4's fix-dev.sh and start-rb.sh copy and set up, from this mod's files.
prepare() {
    L=$DIR/lib
    mkdir -p "$ROOT/root/pdj" "$ROOT/usr/lib/directfb-1.4-6/systems" "$ROOT/usr/lib/directfb-1.4-6/inputdrivers" \
             "$ROOT/usr/lib/directfb-1.4-6/wm" "$ROOT/usr/etc" "$ROOT/etc" "$ROOT/root/settings" "$ROOT/media/usb1"
    for s in fbshim knobshim audioshim; do
        cp "$L/$s.so" "$ROOT/usr/lib/$s.so" && cp "$L/$s.so" "$ROOT/root/pdj/$s.so"
    done
    for l in directfb direct fusion; do
        cp "$L/dfb/lib$l-1.4.so.0.0.0" "$ROOT/usr/lib/" && ln -sfn "lib$l-1.4.so.0.0.0" "$ROOT/usr/lib/lib$l-1.4.so.0"
    done
    (cd "$L/dfb/directfb-1.4-6" && for m in */*.so; do cp "$m" "$ROOT/usr/lib/directfb-1.4-6/$m"; done)
    # This DirectFB looks for its plugins in /lib/directfb-1.4-6 (where it was built to live), so
    # point that at the ones above; without them it finds no screen driver and rbp quits at once
    if [ -d "$ROOT/lib/directfb-1.4-6" ] && [ ! -L "$ROOT/lib/directfb-1.4-6" ]; then
        cp -r "$L/dfb/directfb-1.4-6/." "$ROOT/lib/directfb-1.4-6/"
    else
        ln -sfn ../usr/lib/directfb-1.4-6 "$ROOT/lib/directfb-1.4-6"
    fi
    # no-hardware keeps DirectFB off the GPU driver; an oops there reboots the deck
    printf 'no-hardware\nno-cursor\nsystem=fbdev\nfbdev=/dev/fb0\n' >"$ROOT/usr/etc/directfbrc"
    cp "$ROOT/usr/etc/directfbrc" "$ROOT/etc/directfbrc"
    [ -f "$ROOT/root/settings/TouchCalib_User.dat" ] || printf '0\n0\n320\n200\n1280\n800\n' >"$ROOT/root/settings/TouchCalib_User.dat"
    [ -f "$ROOT/root/settings/TouchCalib_Factory.dat" ] || cp "$ROOT/root/settings/TouchCalib_User.dat" "$ROOT/root/settings/TouchCalib_Factory.dat"
    chmod 755 "$ROOT/root/pdj/rbp" "$ROOT/lib/ld-linux.so.3" "$ROOT"/lib/ld-*.so 2>/dev/null
    chmod -R 755 "$ROOT/bin" "$ROOT/sbin" "$ROOT/usr/bin" "$ROOT/usr/sbin" 2>/dev/null
    ln -sfn /proc/mounts "$ROOT/etc/mtab"

    # bind mounts (fix-dev.sh)
    for b in $CHROOT_BINDS; do
        mkdir -p "$ROOT/$b"
        mountpoint -q "$ROOT/$b" || mount --bind "/$b" "$ROOT/$b" || return 1
    done
    # FIFOs for the RX3's polled SPI/HID devices (else rbp spins at 100% CPU) and plain files for
    # its ioctl-only devices. Where the deck has a real device of that name, the stub is bound
    # over it inside the chroot only; otherwise it's made in /dev and removed again afterwards.
    mkdir -p "$STATE_DIR/dev"
    : >"$STATE_DIR/made"
    for d in $FIFOS $FILES; do
        s=$STATE_DIR/dev/$d
        case " $FIFOS " in *" $d "*) [ -p "$s" ] || { rm -f "$s"; mkfifo -m 666 "$s"; } ;;
                             *) [ -f "$s" ] || : >"$s"; chmod 666 "$s" ;; esac
        if [ -e "/dev/$d" ]; then
            mountpoint -q "$ROOT/dev/$d" || mount --bind "$s" "$ROOT/dev/$d"
        else
            cp -a "$s" "/dev/$d" && echo "$d" >>"$STATE_DIR/made"
        fi
    done
    [ -e /dev/paudiog0 ] && log "warning: /dev/paudiog0 exists; rblive4 expects it not to"
    # rbp maps i.MX6 registers through /dev/mem; on this board that's real hardware
    stat -c %a /dev/mem >"$STATE_DIR/mem.mode" 2>/dev/null && chmod 000 /dev/mem
    # FIFOs rbp reads USB drive events from (usb-watch.sh writes them)
    for f in udev_usb1 udev_usb2 udev_usbctn1 udev_usbctn2; do
        [ -p "/tmp/$f" ] || { rm -f "/tmp/$f"; mkfifo -m 666 "/tmp/$f"; }
    done
    rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer
    return 0
}

rbp_pid() { cat "$STATE_DIR/rbp.pid" 2>/dev/null; }
rbp_alive() { p=$(rbp_pid); [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }
rbp_ready() { p=$(rbp_pid); [ -n "$p" ] && ls -l "/proc/$p/fd" 2>/dev/null | grep -q udev_usb1; }

engine_back() {
    sh "$DIR/usb-watch.sh" stop >/dev/null 2>&1
    kill_chroot
    unmount_chroot
    while read -r d; do [ -n "$d" ] && rm -f "/dev/$d"; done <"$STATE_DIR/made" 2>/dev/null
    rm -f "$STATE_DIR/made"
    [ -s "$STATE_DIR/mem.mode" ] && chmod "$(cat "$STATE_DIR/mem.mode")" /dev/mem && rm -f "$STATE_DIR/mem.mode"
    rm -f "$STATE_DIR/rbp.pid" "$STATE_DIR/session"
    systemctl start edisksd.service 2>/dev/null
    systemctl start engine.service
    log "Engine started again"
}

session() {
    installed || { state missing "No rekordbox pack on the deck yet."; return 1; }
    echo $$ >"$STATE_DIR/session"
    trap 'state stopping "Going back to Engine..."; engine_back; check; exit 0' INT TERM
    state starting "Stopping Engine..."
    systemctl stop engine.service
    systemctl stop edisksd.service 2>/dev/null
    sleep 1
    kill_chroot
    if ! prepare; then
        state error "Couldn't set up the rekordbox folder. See /tmp/rekordbox.log."
        engine_back; return 1
    fi
    state starting "Starting rekordbox..."
    EDB_BIN=/usr/bin chroot "$ROOT" /lib/ld-linux.so.3 /usr/bin/edb_streamd >"$STATE_DIR/edb.log" 2>&1 &
    sleep 1
    sh "$DIR/usb-watch.sh" stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    chroot "$ROOT" env PATH=/bin:/sbin:/usr/bin:/usr/sbin DFB_ROTATE=left STARTUP_MUTE_MS=1500 STARTUP_FADE_MS=300 \
        ${RB_ENV:-} LD_PRELOAD="$PRELOAD" \
        /lib/ld-linux.so.3 /root/pdj/rbp -a </dev/null >"$STATE_DIR/rbp.log" 2>&1 &
    echo $! >"$STATE_DIR/rbp.pid"
    i=0
    while [ $i -lt $((READY_WAIT * 2)) ]; do
        rbp_ready && break
        rbp_alive || break
        sleep 0.5; i=$((i + 1))
    done
    if ! rbp_ready; then
        { echo "---- last lines from rekordbox (all of it: $STATE_DIR/rbp.log)"; tail -n 40 "$STATE_DIR/rbp.log"; echo "----"; } >>"$LOG" 2>/dev/null
        if rbp_alive; then why="rekordbox didn't finish starting within $READY_WAIT seconds"; else why="rekordbox quit while starting"; fi
        state stopping "$why. Going back to Engine..."
        engine_back
        state error "$why. Engine is back. Details in /tmp/rekordbox.log."
        return 1
    fi
    sh "$DIR/usb-watch.sh" start >/dev/null 2>&1
    state running "Hold SHIFT and BACK for two seconds to go back to Engine."
    while rbp_alive; do sleep 1; done
    state stopping "Going back to Engine..."
    engine_back
    check
}

end() {
    p=$(rbp_pid)
    [ -n "$p" ] || return 0
    log "quitting rekordbox (pid $p)"
    kill "$p" 2>/dev/null
    for _ in 1 2 3 4 5 6; do kill -0 "$p" 2>/dev/null || return 0; sleep 0.5; done
    kill -9 "$p" 2>/dev/null
    return 0
}

case "$ACTION" in
start)
    launcher_stop
    sleep 1
    B=$(binary) || { log "can't run $DIR/rekordbox-launcher"; exit 1; }
    if command -v systemd-run >/dev/null 2>&1 &&
       systemd-run --unit="$UNIT" --collect -p KillMode=process --setenv=RB_MOD_DIR="$DIR" "$B" --serve >>"$LOG" 2>&1; then
        log "launcher started (systemd unit $UNIT)"
    else
        RB_MOD_DIR="$DIR" setsid "$B" --serve >>"$LOG" 2>&1 </dev/null &
        log "launcher started (pid $!)"
    fi
    ;;
stop)
    s=$(cat "$STATE_DIR/session" 2>/dev/null)
    [ -n "$s" ] && kill "$s" 2>/dev/null
    end
    launcher_stop
    log "launcher stopped"
    ;;
check) check ;;
setup) setup ;;
install) install_pack "$2" ;;
session) session ;;
end) end ;;
status) cat "$STATE" 2>/dev/null; systemctl status "$UNIT" --no-pager 2>/dev/null || pgrep -af "rekordbox-launcher.* --serve" ;;
*) echo "usage: $0 start|stop|check|setup|install FILE|session|end|status" >&2; exit 2 ;;
esac
exit 0
