#!/system/bin/sh
# legacy — 新流程用打包注入器：su -c /data/local/tmp/nr3e --restart
# nr3e — 开游戏 → 内核注入 libnr3e.so → 启动 ImGui HUD
# SRZX stealth_inject + stealth.kpm + APatch key
set -e
export PATH=/system/bin:/system/xbin:/vendor/bin:/debug_ramdisk:/sbin:/data/local/tmp:$PATH

SELF="$0"
case "$SELF" in
  /*) ;;
  *) SELF="$(pwd)/$SELF" ;;
esac
SCRIPT_DIR="${SELF%/*}"

PKG="${PKG:-com.tencent.letsgo}"
SO="${SO:-}"
INJ="${INJ:-}"
KPM="${KPM:-}"
KPATCH="${KPATCH:-}"
HUD="${HUD:-}"
HID="${HID:-}"
KEY="${KEY:-Lkdj4K46iwowb3K+-+@+++}"
WAIT="${WAIT:-libUE4.so}"
DELAY="${DELAY:-2000}"
TIMEOUT="${TIMEOUT:-90}"
RSS="${RSS:-500000}"
ACTIVITY="${ACTIVITY:-com.epicgames.ue4.GameActivityExt}"
DO_RESTART=0
SKIP_HUD=0

pick() {
  for C in "$@"; do
    [ -n "$C" ] && [ -e "$C" ] && { echo "$C"; return 0; }
  done
  return 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --restart) DO_RESTART=1 ;;
    --no-hud) SKIP_HUD=1 ;;
    --start) ;; # default already starts the game
    -p|--package) PKG="$2"; shift ;;
    -s|--so) SO="$2"; shift ;;
    --key) KEY="$2"; shift ;;
    *) echo "[-] unknown arg: $1"; exit 1 ;;
  esac
  shift
done

CACHE="${CACHE:-/data/adb/.7e91a4c2}"
SO=$(pick "$SO" \
  "$CACHE/c7f2a91b" \
  /data/adb/.cache/l64) || true
INJ=$(pick "$INJ" \
  "$CACHE/e" \
  /data/local/tmp/stealth_inject \
  /storage/emulated/0/SRZX/stealth_inject) || true
KPM=$(pick "$KPM" \
  "$CACHE/d" \
  /data/local/tmp/stealth.kpm \
  /storage/emulated/0/SRZX/stealth.kpm) || true
KPATCH=$(pick "$KPATCH" \
  "$CACHE/f" \
  /data/local/tmp/kpatch \
  /storage/emulated/0/SRZX/kpatch) || true
HUD=$(pick "$HUD" \
  "$CACHE/b" \
  /data/adb/.cache/uv \
  /data/local/tmp/update_verifier) || true

need() {
  [ -e "$1" ] || { echo "[-] missing $2: $1"; exit 1; }
}
need "$INJ" stealth_inject
need "$SO" payload
need "$KPM" stealth.kpm
need "$KPATCH" kpatch
need "$HUD" hud

chmod 755 "$INJ" "$KPATCH" "$HUD" 2>/dev/null || true

if [ "$(id -u)" != "0" ]; then
  echo "[-] need root"
  exit 1
fi

echo "[*] kpm load stealth"
"$KPATCH" "$KEY" kpm load "$KPM" >/dev/null 2>&1 || true
if ! "$KPATCH" "$KEY" kpm list 2>/dev/null | grep -q stealth; then
  echo "[-] stealth.kpm not loaded"
  "$KPATCH" "$KEY" kpm list || true
  exit 1
fi

wait_engine() {
  I=0
  MAX=$((TIMEOUT / 2))
  [ "$MAX" -lt 8 ] && MAX=8
  while [ "$I" -lt "$MAX" ]; do
    PID="$(pidof "$PKG" 2>/dev/null | awk '{print $1}' || true)"
    if [ -n "$PID" ]; then
      MAPS="$(grep -c "$WAIT" "/proc/$PID/maps" 2>/dev/null || true)"
      RSSNOW="$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null || true)"
      echo "[*] t=${I} pid=$PID rss=${RSSNOW}kB ${WAIT} maps=$MAPS"
      if [ "${RSSNOW:-0}" -gt "$RSS" ]; then
        echo "[+] engine ready"
        return 0
      fi
    else
      echo "[*] t=${I} no pid"
    fi
    I=$((I + 1))
    sleep 2
  done
  echo "[-] wait $WAIT timed out"
  return 1
}

PID="$(pidof "$PKG" 2>/dev/null | awk '{print $1}' || true)"
if [ "$DO_RESTART" = 1 ] || [ -z "$PID" ]; then
  if [ -n "$PID" ]; then
    echo "[*] force-stop $PKG"
    am force-stop "$PKG" >/dev/null 2>&1 || true
    sleep 1
  fi
  echo "[*] start $PKG"
  am start -n "$PKG/$ACTIVITY" >/dev/null
fi
wait_engine

echo "[*] inject $SO"
"$INJ" \
  -p "$PKG" \
  -s "$SO" \
  --wait-lib '' \
  --no-tinject \
  --delay "$DELAY" \
  --timeout "$TIMEOUT" \
  --kpm "$KPM" \
  --kpatch "$KPATCH" \
  --key "$KEY"
echo "[+] inject done"

GPID="$(pidof "$PKG" 2>/dev/null | awk '{print $1}' || true)"
if [ -n "$GPID" ]; then
  PATHU="$(dmesg 2>/dev/null | sed -n 's/.*path_u=\([0-9a-fA-F]*\).*/\1/p' | tail -1)"
  if [ -n "$PATHU" ]; then
    dd if=/dev/zero of=/proc/$GPID/mem bs=1 seek=$((0x$PATHU)) count=256 conv=notrunc 2>/dev/null || true
  fi
fi

CC="/data/data/$PKG/code_cache"
if [ -d "$CC" ]; then
  for F in "$CC"/*; do
    [ -f "$F" ] || continue
    BN="${F##*/}"
    case "$BN" in
      [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])
        rm -f "$F" ;;
    esac
  done
fi

dmesg -c >/dev/null 2>&1 || true
sleep 0.4

if [ "$SKIP_HUD" = 1 ]; then
  echo "[*] skip hud"
  exit 0
fi

pkill -f '/nr3e_hud$' >/dev/null 2>&1 || true
pkill -x nr3e_hud >/dev/null 2>&1 || true
pkill -x update_verifier >/dev/null 2>&1 || true
rm -f /data/data/$PKG/files/nr3e_dbg.txt /data/data/$PKG/files/.gs \
  /data/data/$PKG/files/mmkv/.crc \
  /data/local/tmp/nr3e_hud.log /data/local/tmp/nr3e_dbg.txt 2>/dev/null || true
sleep 0.2
echo "[*] hud $HUD"
mkdir -p /data/local/tmp
nohup "$HUD" >/dev/null 2>&1 &
sleep 0.5
HPID="$(pidof update_verifier nr3e_hud 2>/dev/null | awk '{print $1}' || true)"
if [ -n "$HPID" ]; then
  echo "[+] imgui up  pid=$HPID"
else
  echo "[-] hud failed"
  exit 1
fi
