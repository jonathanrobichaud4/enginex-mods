#!/bin/sh
# system-stats: writes CPU and RAM use every 2 s for the status bar.
# EngineX runs this with "start" (boot, enable) and "stop" (disable, uninstall).
#   cpu  percent of all cores busy over the last 2 s (from /proc/stat)
#   ram  percent of memory in use: (MemTotal - MemAvailable) / MemTotal
#   temp CPU temperature in degrees C: the "cpu-thermal" zone, else the hottest zone
ACTION="${1:-start}"
THERMAL="${SYSSTATS_THERMAL:-/sys/class/thermal}"
OUT="${SYSSTATS_OUT:-/tmp/enginex-system-stats.ini}"
PIDFILE=/tmp/enginex-system-stats.pid
INTERVAL="${SYSSTATS_INTERVAL:-2}"

loop() {
    prev_total=0
    prev_idle=0
    while :; do
        read -r _ user nice system idle iowait irq softirq steal _ < /proc/stat
        idle_all=$((idle + iowait))
        total=$((user + nice + system + idle + iowait + irq + softirq + ${steal:-0}))
        cpu=0
        if [ "$prev_total" -gt 0 ] && [ "$total" -gt "$prev_total" ]; then
            dt=$((total - prev_total))
            di=$((idle_all - prev_idle))
            cpu=$(( (100 * (dt - di) + dt / 2) / dt ))
        fi
        prev_total=$total
        prev_idle=$idle_all
        mem_total=0
        mem_avail=0
        while read -r key value _; do
            case "$key" in
                MemTotal:) mem_total=$value ;;
                MemAvailable:) mem_avail=$value; break ;;
            esac
        done < /proc/meminfo
        ram=0
        [ "$mem_total" -gt 0 ] && ram=$(( (100 * (mem_total - mem_avail) + mem_total / 2) / mem_total ))
        temp=-1
        hottest=-1
        for zone in "$THERMAL"/thermal_zone*; do
            read -r t < "$zone/temp" 2>/dev/null || continue
            case "$t" in ''|*[!0-9-]*) continue ;; esac
            read -r type < "$zone/type" 2>/dev/null
            [ "$t" -gt "$hottest" ] && hottest=$t
            case "$type" in cpu-thermal|cpu_thermal|cpu) temp=$t ;; esac
        done
        [ "$temp" -lt 0 ] && temp=$hottest
        [ "$temp" -ge 0 ] && temp=$(( (temp + 500) / 1000 ))
        printf '[stats]\ncpu=%s\nram=%s\ntemp=%s\ntime=%s\n' "$cpu" "$ram" "$temp" "$(date +%s)" > "$OUT.tmp" && mv -f "$OUT.tmp" "$OUT"
        sleep "$INTERVAL"
    done
}

stop() {
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
    rm -f "$OUT" "$OUT.tmp"
}

case "$ACTION" in
    start)
        stop
        setsid sh "$0" run < /dev/null > /dev/null 2>&1 &
        echo $! > "$PIDFILE"
        ;;
    run) loop ;;
    stop) stop ;;
    *) echo "usage: $0 start|stop" >&2; exit 2 ;;
esac
exit 0
