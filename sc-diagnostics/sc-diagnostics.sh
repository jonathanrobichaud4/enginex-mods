#!/bin/sh
# sc-diagnostics: hardware test app for the SC LIVE 4.
#
# EngineX runs this with "start" (boot, enable) and "stop" (disable, uninstall).
#   start: starts the launcher ("sc-diagnostics --serve") on port 8092. It answers
#          Settings > Advanced > Hardware Diagnostics, and http://<deck-ip>:8092/ in a browser.
#          Starting the test stops Engine, runs the app on the deck's screen, and starts
#          Engine again when the app closes (EXIT on the touchscreen, or hold BROWSE 3 s).
#   stop:  ends the app if it's running (Engine comes back) and stops the launcher.
#
# The launcher runs in its own systemd unit (sc-diagnostics.service, transient) so that
# stopping Engine, or EngineX's script timeout, can't take it down with it.
# Logs: journal tag "sc-diagnostics", and /tmp/sc-diagnostics.log.

ACTION="${1:-start}"
DIR="${ENGINEX_MOD_DIR:-$(cd "$(dirname "$0")" && pwd)}"
LOG=/tmp/sc-diagnostics.log
UNIT=sc-diagnostics

log() { echo "$(date '+%F %T') $*" >>"$LOG"; logger -t sc-diagnostics "$*" 2>/dev/null || true; }

# The binary has to be executable; some storage is mounted noexec, so fall back to a copy in /tmp.
binary() {
    b="$DIR/sc-diagnostics"
    chmod 755 "$b" 2>/dev/null
    if "$b" --version >/dev/null 2>&1; then echo "$b"; return; fi
    cp "$b" /tmp/sc-diagnostics.bin && chmod 755 /tmp/sc-diagnostics.bin && echo /tmp/sc-diagnostics.bin
}

stop_all() {
    pid=$(cat /tmp/sc-diagnostics.pid 2>/dev/null)
    [ -n "$pid" ] && kill "$pid" 2>/dev/null && log "ended the running test"
    systemctl stop "$UNIT" 2>/dev/null
    pkill -f "sc-diagnostics.* --serve" 2>/dev/null
    return 0
}

case "$ACTION" in
start)
    stop_all
    sleep 1
    B=$(binary) || { log "can't run $DIR/sc-diagnostics"; exit 1; }
    if command -v systemd-run >/dev/null 2>&1 &&
       systemd-run --unit="$UNIT" --collect -p KillMode=process "$B" --serve >>"$LOG" 2>&1; then
        log "launcher started (systemd unit $UNIT)"
    else
        setsid "$B" --serve >>"$LOG" 2>&1 </dev/null &
        log "launcher started (pid $!)"
    fi
    ;;
stop)
    stop_all
    log "launcher stopped"
    ;;
status)
    systemctl status "$UNIT" --no-pager 2>/dev/null || pgrep -af "sc-diagnostics.* --serve"
    ;;
esac
exit 0
