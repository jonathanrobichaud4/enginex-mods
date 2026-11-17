#!/bin/sh
# streaming-check2: second pass at the EngineX streaming failure.
#   sh streaming-check2.sh          look only: cached downloads + Spotify's error text
#   sh streaming-check2.sh --clear  also move the download cache aside (kept as a backup)
# Writes /tmp/streaming-report2.txt. Tokens and passwords are masked.
OUT=/tmp/streaming-report2.txt
CACHE=/data/enginex/Stream
mask() {
    sed -E -e 's/(Bearer )[A-Za-z0-9._~+\/=-]+/\1***/g' \
        -e 's/((token|access_token|refresh_token|oauth_token|arl|password|secret|cookie|authorization|client_secret)["'"'"' ]*[:=]["'"'"' ]*)[^"'"'"' &,;}]+/\1***/gI'
}
kind() { # what the first bytes say the file really is
    h=$(dd if="$1" bs=12 count=1 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' \n')
    [ -n "$h" ] || h=$(dd if="$1" bs=12 count=1 2>/dev/null | hexdump -v -e '12/1 "%02x"' 2>/dev/null)
    case "$h" in
        ????????66747970*) echo mp4 ;;
        664c6143*) echo flac ;;
        4f676753*) echo ogg ;;
        494433*|fffb*|fff3*|fff2*|fffa*) echo mp3 ;;
        fff1*|fff9*) echo aac-adts ;;
        52494646*) echo wav ;;
        7b*|3c*) echo "text/json/html (not audio)" ;;
        "") echo empty ;;
        *) echo "unknown ($h)" ;;
    esac
}
{
    echo "===== cached downloads (newest first) ====="
    ls -lt "$CACHE" 2>/dev/null | head -n 1
    ls -t "$CACHE" 2>/dev/null | head -n 40 | while read -r f; do
        [ -f "$CACHE/$f" ] || continue
        size=$(wc -c < "$CACHE/$f")
        printf '%10s  %-28s %s\n' "$size" "$(kind "$CACHE/$f")" "$f"
    done
    echo
    echo "===== Spotify: the error stream-sync gives Engine ====="
    ids=$(journalctl -u enginex-stream-sync --no-pager 2>/dev/null | grep 'provider=spotify step=stream result=failed' |
        sed -n 's/.*load track=\([0-9]*\).*/\1/p' | tail -n 2)
    [ -n "$ids" ] || echo "no failed Spotify load in the log: try loading a Spotify track first"
    for id in $ids; do
        echo "track $id:"
        curl -sS --max-time 60 -o /tmp/sc-spotify-test.bin -w 'http=%{http_code} type=%{content_type} bytes=%{size_download}\n' \
            "http://127.0.0.1:8080/v1/stream/$id.ogg" 2>&1
        if [ -f /tmp/sc-spotify-test.bin ]; then
            case "$(kind /tmp/sc-spotify-test.bin)" in
                text*) tr -d '\000' < /tmp/sc-spotify-test.bin | cut -c1-600 | mask; echo ;;
                *) echo "got audio: $(kind /tmp/sc-spotify-test.bin)" ;;
            esac
            rm -f /tmp/sc-spotify-test.bin
        fi
    done
    echo
    echo "===== stream-sync lines after that request ====="
    journalctl -u enginex-stream-sync --since "-2min" --no-pager 2>/dev/null | grep -iE 'spotify|librespot|premium|fail|error' | tail -n 30 | mask
    if [ "${1:-}" = --clear ]; then
        echo
        echo "===== clearing the download cache ====="
        stamp=$(date +%Y%m%d-%H%M%S)
        if [ -d "$CACHE" ]; then
            mv "$CACHE" "$CACHE.old-$stamp" && mkdir -p "$CACHE" && echo "moved to $CACHE.old-$stamp"
            systemctl restart enginex-stream-sync && echo "restarted stream-sync"
        fi
    fi
} > "$OUT" 2>&1
echo "Report written to $OUT"
echo "Copy it with:  scp root@<deck-ip>:$OUT ."
