#!/bin/sh
# streaming-check: collects what is needed to see why EngineX streaming fails.
# Run on the deck over SSH:  sh streaming-check.sh
# Writes /tmp/streaming-report.txt. Tokens, passwords, ARLs and cookies are masked.
OUT=/tmp/streaming-report.txt
SINCE="${1:--30min}"

mask() {
    sed -E \
        -e 's/(Bearer )[A-Za-z0-9._~+\/=-]+/\1***/g' \
        -e 's/((token|access_token|refresh_token|oauth_token|arl|ARL|password|passwd|secret|cookie|Cookie|authorization|Authorization|Bearer|client_secret|code)["'"'"' ]*[:=]["'"'"' ]*)[^"'"'"' &,;}]+/\1***/g'
}

section() { printf '\n===== %s =====\n' "$1"; }

probe() {
    host=$1; url=$2
    ip=$( (nslookup "$host" 2>/dev/null || getent hosts "$host" 2>/dev/null) | awk '/^Address/ && !/#/ {print $NF} /^[0-9]/ {print $1}' | head -n 1)
    res=$(curl -sS -o /dev/null --max-time 12 -w '%{http_code} %{time_total}s' "$url" 2>&1)
    printf '%-28s dns=%-16s https=%s\n' "$host" "${ip:-FAILED}" "$res"
}

{
    section "deck"
    date
    cat /data/enginex/bundle/release-tag 2>/dev/null
    uptime
    section "clock (TLS fails when the date is wrong)"
    timedatectl status 2>/dev/null || echo "no timedatectl"
    section "network"
    ip -4 addr show 2>/dev/null | awk '/inet / {print $NF, $2}'
    ip route 2>/dev/null | head -n 3
    cat /etc/resolv.conf 2>/dev/null | grep -v '^#'
    section "internet reachability"
    probe www.google.com https://www.google.com/generate_204
    probe api.spotify.com https://api.spotify.com/v1
    probe accounts.spotify.com https://accounts.spotify.com/
    probe apresolve.spotify.com https://apresolve.spotify.com/?type=accesspoint
    probe api.beatport.com https://api.beatport.com/v4/
    probe api-v2.soundcloud.com https://api-v2.soundcloud.com/
    probe soundcloud.com https://soundcloud.com/
    probe api.tidal.com https://api.tidal.com/v1/
    section "storage (downloads fail when /data is full)"
    df -h /data /tmp 2>/dev/null
    du -sh /data/enginex/Stream 2>/dev/null
    section "memory"
    free 2>/dev/null || head -n 3 /proc/meminfo
    section "services"
    for u in enginex-stream-sync engine enginex-sc5000-deemixd enginex-rb-bridge; do
        printf '%-26s %s\n' "$u" "$(systemctl is-active $u 2>/dev/null)"
    done
    systemctl status enginex-stream-sync --no-pager 2>/dev/null | head -n 12
    section "stream-sync local API"
    curl -sS --max-time 5 http://127.0.0.1:8080/boot-status 2>&1 | head -c 800; echo
    section "safe mode / guard"
    ls -l /data/enginex/ui/safe-mode /data/enginex/ui/last-crash 2>&1
    section "hook status this boot (gateway = TIDAL API redirected to stream-sync)"
    journalctl -b --no-pager 2>/dev/null | grep 'engine-ui-hook' | grep -vE 'PX unit' | tail -n 40
    section "Engine log about streaming ($SINCE)"
    journalctl -u engine --since "$SINCE" --no-pager 2>/dev/null |
        grep -iE 'tidal|stream|provider|spotify|beatport|soundcloud|deezer|login|carrier|http|ssl|tls|network|error|fail' | tail -n 150 | mask
    section "stream-sync log ($SINCE)"
    journalctl -u enginex-stream-sync --since "$SINCE" --no-pager 2>/dev/null | tail -n 300 | mask
    section "streaming accounts present (no contents)"
    ls -la /data/enginex/state 2>/dev/null | grep -iE 'spotify|beatport|soundcloud|tidal|deezer|account|token|session' 
} > "$OUT" 2>&1

echo "Report written to $OUT ($(wc -l < "$OUT") lines)."
echo "Copy it to your computer with:  scp root@<deck-ip>:$OUT ."
