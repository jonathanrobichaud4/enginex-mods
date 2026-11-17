#!/bin/sh
# streaming-debug: turns on Engine's own streaming debug log to see why it signs
# out of TIDAL after loading an EngineX streaming track.
#   sh streaming-debug.sh on       turn the extra logging on and restart Engine
#   (load a Beatport / SoundCloud / Spotify track on the deck, wait for the
#    "logged out of TIDAL" message)
#   sh streaming-debug.sh collect  write /tmp/streaming-debug.txt
#   sh streaming-debug.sh off      put the logging back as it was and restart Engine
CONF=/data/logging.ini
BACKUP=/data/logging.ini.streaming-debug-backup
MARK=/tmp/streaming-debug.since
OUT=/tmp/streaming-debug.txt
RULES='air.planck.streaming*=true
air.planck.eaas*=true
air.planck.network.download=true
qt.network.access.debug=false'

mask() {
    sed -E -e 's/(Bearer )[A-Za-z0-9._~+\/=-]+/\1***/g' \
        -e 's/((token|access_token|refresh_token|oauth_token|arl|password|secret|cookie|authorization|client_secret|code)["'"'"' ]*[:=]["'"'"' ]*)[^"'"'"' &,;}]+/\1***/gI'
}

case "${1:-}" in
on)
    if [ ! -e "$BACKUP" ]; then
        if [ -e "$CONF" ]; then cp -p "$CONF" "$BACKUP"; else echo "# none" > "$BACKUP"; fi
    fi
    {
        grep -v '^\[Rules\]' "$BACKUP" 2>/dev/null | grep -v '^# none$' | grep -vE '^air\.planck\.(streaming|eaas)|^air\.planck\.network\.download|^qt\.network\.access'
    } > /tmp/logging.rest
    # Qt applies rules in order, so ours go last and win over a "*.debug=false".
    { echo "[Rules]"; cat /tmp/logging.rest; echo "$RULES"; } > "$CONF"
    rm -f /tmp/logging.rest
    date '+%Y-%m-%d %H:%M:%S' > "$MARK"
    systemctl restart engine
    echo "Debug logging on, Engine restarting. Load a streaming track, wait for the TIDAL message, then run: sh $0 collect"
    ;;
collect)
    since=$(cat "$MARK" 2>/dev/null || echo "-15min")
    {
        echo "===== Engine streaming log since $since ====="
        journalctl -u engine --since "$since" --no-pager 2>/dev/null |
            grep -iE 'stream|tidal|eaas|sso|provider|logout|logged|login|auth|token|http|download|carrier|EngineX' |
            grep -vE 'AlbumArt|BreadcrumbViewListEntryDelegate' | tail -n 600 | mask
        echo
        echo "===== stream-sync since $since ====="
        journalctl -u enginex-stream-sync --since "$since" --no-pager 2>/dev/null | grep -v 'guard: tid=' | tail -n 200 | mask
    } > "$OUT" 2>&1
    echo "Written $OUT ($(wc -l < "$OUT") lines). Copy it with: scp root@<deck-ip>:$OUT ."
    ;;
off)
    if [ -e "$BACKUP" ]; then
        if grep -qx '# none' "$BACKUP"; then rm -f "$CONF"; else cp -p "$BACKUP" "$CONF"; fi
        rm -f "$BACKUP"
    fi
    rm -f "$MARK"
    systemctl restart engine
    echo "Logging back to normal, Engine restarting."
    ;;
*) echo "usage: sh $0 on | collect | off" >&2; exit 2 ;;
esac
