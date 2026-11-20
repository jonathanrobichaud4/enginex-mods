#!/bin/sh
# fx-channel-indicator: loads the fxbands library into Engine, so the FX bar's
# LOW / MID / HIGH buttons can pick any combination (LOW + HIGH, MID alone, ...).
#
# Engine's unit preloads whatever the UI hook variable says, and reads it from EngineX's
# ui-scale.env at every start. Which variable and file depends on EngineX:
#     5.3.0.4 and earlier: SC5000_UI_HOOK  in /data/sc5000-custom/ui-scale.env
#     5.3.0.5 and later:   ENGINEX_UI_HOOK in /data/enginex/ui-scale.env
# (worked out below from Engine's unit and launcher). The line there is
#     <VAR>=<hook>[:<extra library>...]
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
# Which scheme this EngineX uses: 5.3.0.5's launcher preloads ENGINEX_UI_HOOK.
if grep -qs 'ENGINEX_UI_HOOK' ${FXBANDS_GUARDS:-/etc/enginex/engine-ui-guard /etc/sc5000/engine-ui-guard}; then
    VAR=ENGINEX_UI_HOOK
    DEFAULT_ENV=/data/enginex/ui-scale.env
    HOOKS="/etc/enginex/libengine-ui-hook.so /data/enginex/bundle/lib/libengine-ui-hook.so"
else
    VAR=SC5000_UI_HOOK
    DEFAULT_ENV=/data/sc5000-custom/ui-scale.env
    HOOKS="/data/enginex/bundle/lib/libengine-ui-hook.so /etc/sc5000/libengine-ui-hook.so"
fi
# The file Engine's unit really reads (EnvironmentFile=-.../ui-scale.env), else the default.
unit_env=$(systemctl cat engine.service 2>/dev/null | sed -n 's/^EnvironmentFile=-\{0,1\}\(.*ui-scale\.env\)$/\1/p' | tail -n 1)
ENVFILE="${FXBANDS_ENVFILE:-${unit_env:-$DEFAULT_ENV}}"
RUN=/tmp
WATCHPID=$RUN/fx-bands-env.pid
LOG=/tmp/fx-bands.log

log() { echo "$(date '+%F %T') $*" >>"$LOG"; logger -t fx-bands "$*" 2>/dev/null || true; }

# EngineX's own hook (what Engine preloads when the line isn't there).
default_hook() {
    h=$(systemctl show engine.service -p Environment 2>/dev/null | tr ' ' '\n' | sed -n "s/^$VAR=//p" | head -n 1)
    [ -n "$h" ] && { echo "$h"; return; }
    for h in $HOOKS; do
        [ -f "$h" ] && { echo "$h"; return; }
    done
    set -- $HOOKS
    echo "$1"
}

line_value() { sed -n "s/^$VAR=//p" "$ENVFILE" 2>/dev/null | head -n 1; }
has_lib() { case ":$(line_value):" in *":$LIB:"*) return 0 ;; esac; return 1; }

# Writes $VAR=$1 (no argument: removes the line), keeping every other line.
write_line() {
    mkdir -p "$(dirname "$ENVFILE")" 2>/dev/null
    tmp="$ENVFILE.fx-bands.$$"
    { grep -v "^$VAR=" "$ENVFILE" 2>/dev/null; [ -z "$1" ] || echo "$VAR=$1"; } >"$tmp" || { rm -f "$tmp"; return 1; }
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
    echo "file: $ENVFILE ($VAR)"
    echo "line: $(line_value)"
    engine_has_lib && echo "Engine: band library loaded" || echo "Engine: band library not loaded"
    cat /tmp/enginex-fxbands.status 2>/dev/null
    ;;
esac
exit 0
