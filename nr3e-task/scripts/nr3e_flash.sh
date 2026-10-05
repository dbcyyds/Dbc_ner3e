#!/system/bin/sh
# 开机后刷 stealth + nr3ehid。不要在未 reboot 的脏内核上 unload/reload。
set -e
export PATH=/system/bin:/system/xbin:/vendor/bin:/debug_ramdisk:/sbin:/data/local/tmp:$PATH
KEY="${KEY:-Lkdj4K46iwowb3K+-+@+++}"
KPATCH="${KPATCH:-/data/local/tmp/kpatch}"
ST="${ST:-/data/local/tmp/stealth.kpm}"
HID="${HID:-/data/local/tmp/nr3ehid.kpm}"
if [ "$(id -u)" != 0 ]; then
  echo "[-] need root"
  exit 1
fi
[ -x "$KPATCH" ] || KPATCH=/storage/emulated/0/SRZX/kpatch
[ -e "$ST" ] || ST=/storage/emulated/0/SRZX/stealth.kpm
[ -e "$HID" ] || HID=/storage/emulated/0/SRZX/nr3ehid.kpm
echo "[*] load stealth"
"$KPATCH" "$KEY" kpm load "$ST" || true
echo "[*] load nr3ehid"
"$KPATCH" "$KEY" kpm load "$HID" || true
"$KPATCH" "$KEY" kpm list
echo "[+] flash done. next: /data/local/tmp/nr3e.sh"
