# Dbc_ner3e

大白菜制作

元梦之星 NR3E 一套：用户态工程、注入器、KernelPatch 模块源码、以及已经编好的成品。

## 目录

| 路径 | 内容 |
|------|------|
| `nr3e-task/` | 工程源码。悬浮窗、任务、注入器 `inject/`、邮箱模块 `kpm/nr3ehid.c` |
| `nr3e-task/outputs/arm64-v8a/` | 成品 `nr3e`、`libnr3e.so`、`nr3e_hud` |
| `nr3e-task/prebuilt/` | 成品 `stealth.kpm`、`kpatch` |
| `kernel/mkpms-stealth/kpms/` | `stealth.kpm` 用到的内核模块源码：stealth、kload、hide-maps、wxshadow、dbc-rw、nr3ehid、kfix、kpcloak、ktouch |
| `kernel/mkpms-stealth/tools/stealth_inject/` | 注入器源码 |
| `kernel/KernelPatch/` | 编 KPM 和 `kpatch` 用的 KernelPatch |

`stealth.kpm` 由 `kernel/mkpms-stealth/kpms/stealth/CMakeLists.txt` 把上面这些模块融成一个镜像。

## 设备上跑成品

```bash
su -c /data/local/tmp/nr3e --restart
```

成品在 `nr3e-task/outputs/arm64-v8a/nr3e`。它里面已经嵌了 so、HUD、`stealth.kpm` 和 `kpatch`。

## 重新编译

```bash
cd nr3e-task
make -j$(nproc)
make install
```

内核模块在装好 KernelPatch 头文件后：

```bash
cd kernel/mkpms-stealth
# kernel 符号链接指向 ../KernelPatch/kernel
cmake -S . -B build -G Ninja
cmake --build build -j --target stealth.kpm
```
