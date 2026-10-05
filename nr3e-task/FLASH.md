# 单 ELF

产物：`outputs/arm64-v8a/nr3e`

每次运行：解压 HUD / stealth.kpm / SO / kpatch 到 `/data/adb/.<rand>/`（随机文件名）→ 输入 APatch 密钥刷 KPM → 开游戏 → 注入 → 启动 HUD → **删掉解压目录**。下次再跑会重新解压。

```sh
su -c /data/local/tmp/nr3e --restart
```

无 TTY 时用 `--key '你的密钥'`。`--no-hud` 只注入。

**禁止在未 reboot 的内核上 load/unload KPM。** `kpm list` 只应看到 `stealth`。
