#!/data/data/com.termux/files/usr/bin/sh
set -e
DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
KPM="$DIR/../../../build-kpcloak/kpms/kpcloak/kpcloak.kpm"
KPATCH="/data/data/com.termux/files/home/src/nr3e-task/prebuilt/kpatch"
KEY=$(sed -n 's/^KEY="${KEY:-\(.*\)}".*/\1/p' /data/data/com.termux/files/home/src/nr3e-task/kload.sh | head -n 1)
[ -n "$KEY" ] || { echo "missing key"; exit 1; }
[ -f "$KPM" ] || { echo "missing $KPM"; exit 1; }
cp "$KPM" /data/local/tmp/kpcloak.kpm
chmod 644 /data/local/tmp/kpcloak.kpm
"$KPATCH" "$KEY" kpm unload kpcloak >/dev/null 2>&1 || true
"$KPATCH" "$KEY" kpm load /data/local/tmp/kpcloak.kpm
"$KPATCH" "$KEY" kpm ctl0 kpcloak status
