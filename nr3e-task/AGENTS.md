# nr3e-task

Standalone LetsGo NR3E3 task overlay. **Not** Dumper-7.

- Single ELF: `outputs/arm64-v8a/nr3e` — embeds SO + HUD + stealth.kpm + kpatch
- Each run extracts to `/data/adb/.<rand>/`, loads KPM with prompted key, injects, then wipes

```
make -j$(nproc)
make install
su -c /data/local/tmp/nr3e --restart
```

Offsets from SDK `Basic.hpp` / Dumpspace; task layouts from `FNR3E3MyTaskStruct`.
