#!/bin/sh
# fx-channel-indicator: loads the fxbands library into Engine, so the FX bar's
# LOW / MID / HIGH buttons can pick any combination (LOW + HIGH, MID alone, ...).
#
# Engine's unit preloads whatever SC5000_UI_HOOK says, and reads that variable from
# /data/sc5000-custom/ui-scale.env at every start. The line there is
#     SC5000_UI_HOOK=<hook>[:<extra library>...]
# where <hook> is EngineX's hook (or usb-audio-out's patched build of it). This script
# adds ":<mod dir>/lib/libfxbands.so" to it and takes it off again on stop, leaving
# the rest alone. EngineX rewrites the file when it saves its UI settings and drops
# the line, so a small loop puts it back within 2 s (Engine only reads the file when
# it starts, so a short gap does nothing).
#
# start: add the library; restart Engine once if it's running without it.
# stop:  take the library off (Engine keeps it until its next restart; with the line
#        gone it simply isn't loaded again).
# Log: /tmp/fx-bands.log and the journal (tag fx-bands).

ACTION="${1:-start}"
DIR="${ENGINEX_MOD_DIR:-$(cd "$(dirname "$0")" && pwd)}"
LIB="$DIR/lib/libfxbands.so"
ENVFILE="${FXBANDS_ENVFILE:-/data/sc5000-custom/ui-scale.env}"
RUN=/tmp
WATCHPID=$RUN/fx-bands-env.pid
LOG=/tmp/fx-bands.log

log() { echo "$(date '+%F %T') $*" >>"$LOG"; logger -t fx-bands "$*" 2>/dev/null || true; }

# EngineX's own hook (what Engine preloads when the line isn't there).
default_hook() {
    h=$(systemctl show engine.service -p Environment 2>/dev/null | tr ' ' '\n' | sed -n 's/^SC5000_UI_HOOK=//p' | head -n 1)
    [ -n "$h" ] && { echo "$h"; return; }
    for h in /data/enginex/bundle/lib/libengine-ui-hook.so /etc/sc5000/libengine-ui-hook.so; do
        [ -f "$h" ] && { echo "$h"; return; }
    done
    echo /data/enginex/bundle/lib/libengine-ui-hook.so
}

line_value() { sed -n 's/^SC5000_UI_HOOK=//p' "$ENVFILE" 2>/dev/null | head -n 1; }
has_lib() { case ":$(line_value):" in *":$LIB:"*) return 0 ;; esac; return 1; }

# Writes SC5000_UI_HOOK=$1 (no argument: removes the line), keeping every other line.
write_line() {
    mkdir -p "$(dirname "$ENVFILE")" 2>/dev/null
    tmp="$ENVFILE.fx-bands.$$"
    { grep -v '^SC5000_UI_HOOK=' "$ENVFILE" 2>/dev/null; [ -z "$1" ] || echo "SC5000_UI_HOOK=$1"; } >"$tmp" || { rm -f "$tmp"; return 1; }
    if [ -s "$tmp" ]; then mv -f "$tmp" "$ENVFILE"; else rm -f "$tmp" "$ENVFILE"; fi
}

add_lib() {
    has_lib && return 0
    v=$(line_value)
    [ -n "$v" ] || v=$(default_hook)
    write_line "$v:$LIB"
}

remove_lib() {
    v=$(line_value)
    [ -n "$v" ] || return 0
    rest=$(echo ":$v:" | sed "s|:$LIB:|:|g; s|^:||; s|:\$||")
    # Only EngineX's own hook left: no line needed (that's Engine's default anyway).
    [ "$rest" = "$(default_hook)" ] && rest=
    write_line "$rest"
}

engine_has_lib() {
    for p in $(pidof Engine 2>/dev/null); do
        grep -q "libfxbands.so" "/proc/$p/maps" 2>/dev/null && return 0
    done
    return 1
}

stop_watch() {
    [ -f "$WATCHPID" ] && kill "$(cat "$WATCHPID")" 2>/dev/null
    rm -f "$WATCHPID"
}

case "$ACTION" in
start)
    if [ ! -f "$LIB" ]; then log "missing $LIB"; exit 0; fi
    stop_watch
    add_lib && log "added the band library to $ENVFILE" || log "cannot write $ENVFILE"
    setsid sh "$0" watch </dev/null >/dev/null 2>&1 &
    echo $! >"$WATCHPID"
    # Running without it (first enable): restart Engine once the line is there. EngineX may
    # restart Engine itself for the mod's screens; wait a little so we don't restart twice.
    setsid sh "$0" restart-if-needed </dev/null >/dev/null 2>&1 &
    ;;
restart-if-needed)
    sleep 8
    if [ -n "$(pidof Engine 2>/dev/null)" ] && ! engine_has_lib && has_lib; then
        log "restarting Engine so it loads the band library"
        systemctl restart engine
    fi
    ;;
watch)
    while :; do
        [ -f "$LIB" ] || { remove_lib; exit 0; } # mod removed without a stop
        has_lib || { add_lib && log "EngineX rewrote $ENVFILE; put the band library back"; }
        sleep 2
    done
    ;;
stop)
    stop_watch
    remove_lib && log "took the band library off $ENVFILE"
    rm -f /tmp/enginex-fxbands
    ;;
status)
    echo "line: $(line_value)"
    engine_has_lib && echo "Engine: band library loaded" || echo "Engine: band library not loaded"
    cat /tmp/enginex-fxbands.status 2>/dev/null
    ;;
esac
exit 0
