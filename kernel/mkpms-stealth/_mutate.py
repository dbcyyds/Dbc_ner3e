#!/usr/bin/env python3
"""Fingerprint-mutate mkpms-stealth → mkpms-veil.

Original clone is left intact. Variant lives next to it.
"""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

SRC = Path("/data/data/com.termux/files/home/src/mkpms-stealth")
DST = Path("/data/data/com.termux/files/home/src/mkpms-veil")

TEXT_EXT = {
    ".c", ".h", ".hpp", ".cpp", ".cc", ".cxx", ".cppm",
    ".txt", ".md", ".sh", ".py", ".bat", ".lds", ".cmake",
    ".gitignore", ".gitmodules",
}
TEXT_NAMES = {".gitignore", ".gitmodules", "LICENSE", "README", "CMakeLists.txt"}

# Longest-first string replacements (source ABI / IOCs).
REPLACEMENTS: list[tuple[str, str]] = [
    # compile flag
    ("STEALTH_FUSED", "VEIL_FUSED"),
    # ABI magics — full hex first
    ("0x1b1841fd2c1e0000ULL", "0x3c27a9e81d4b0000ULL"),
    ("0x1b1841fd2c1e0000", "0x3c27a9e81d4b0000"),
    ("0x20260725", "0x20260821"),
    ("0x484D0001", "0xA71C0001"),
    ("0x484D0002", "0xA71C0002"),
    ("0x484D0003", "0xA71C0003"),
    ("0x484D0004", "0xA71C0004"),
    ("0x484D0010", "0xA71C0010"),
    ("0x484D0011", "0xA71C0011"),
    ("0x484D0012", "0xA71C0012"),
    ("0x4B4C0001", "0xC93E0001"),
    ("0x4B4C0010", "0xC93E0010"),
    ("0x4B4C0011", "0xC93E0011"),
    ("0x4B4C0012", "0xC93E0012"),
    ("0x57580001", "0xB62F0001"),
    ("0x57580002", "0xB62F0002"),
    ("0x57580003", "0xB62F0003"),
    ("0x57580004", "0xB62F0004"),
    ("0x57580005", "0xB62F0005"),
    ("0x57580006", "0xB62F0006"),
    ("0x57580007", "0xB62F0007"),
    ("0x57580008", "0xB62F0008"),
    ("0x484Dxxxx", "0xA71Cxxxx"),
    ("0x4B4Cxxxx", "0xC93Exxxx"),
    ("0x5758xxxx", "0xB62Fxxxx"),
    ("0x484D", "0xA71C"),
    ("0x4B4C", "0xC93E"),
    ("0x5758", "0xB62F"),
    # BRK imm (wxshadow unique fingerprint)
    ("WXSHADOW_BRK_IMM        0x007", "WXSHADOW_BRK_IMM        0x013"),
    # default hide keywords / tags
    ("libdbc_fl", "libvk_fl"),
    ("libdbc.so", "libvkcore.so"),
    ("libdbc", "libvkcore"),
    ("libtcore.so", "libvcore.so"),
    ("libtcore", "libvcore"),
    ("DbcHK", "VkHK"),
    ("framelog", "vframe"),
    (".kload", ".kld"),
    # kpm / tool file names (before generic stealth)
    ("stealth_inject.h", "veil_inject.h"),
    ("stealth_inject.c", "veil_inject.c"),
    ("stealth_inject", "veil_inject"),
    ("stealth.kpm", "veilk.kpm"),
    ("stealth_kpm.kpm", "veilk_kpm.kpm"),
    ("hide-maps.kpm", "vmap.kpm"),
    ("kload.kpm", "kld.kpm"),
    ("wxshadow.kpm", "wxsh.kpm"),
    ("dbc-rw.kpm", "vrw.kpm"),
    ("wxshadow_client", "veil_wx"),
    # KPM metadata strings
    ('KPM_MODULE_INFO("stealth"', 'KPM_MODULE_INFO("veilk"'),
    ('KPM_MODULE_INFO("hide-maps"', 'KPM_MODULE_INFO("vmap"'),
    ('KPM_MODULE_INFO("kload"', 'KPM_MODULE_INFO("kld"'),
    ('KPM_NAME("wxshadow")', 'KPM_NAME("wxsh")'),
    ('KPM_MODULE_INFO("dbc"', 'KPM_MODULE_INFO("vrw"'),
    ('KPM_AUTHOR("wxshadow")', 'KPM_AUTHOR("nx")'),
    ('"dbc",', '"nx",'),
    # dmesg / ctl prefixes (keep identifiers like hide_maps_init)
    ("stealth: ", "vk: "),
    ("hide-maps: ", "vm: "),
    ("kload: ", "kl: "),
    ("wxshadow: ", "ws: "),
    ("dbc-rw: ", "vr: "),
    ('kpm_demo_log_init("stealth"', 'kpm_demo_log_init("veilk"'),
    ('kpm_demo_log_exit("stealth")', 'kpm_demo_log_exit("veilk")'),
    ('kpm_demo_log_init("hide-maps"', 'kpm_demo_log_init("vmap"'),
    ('kpm_demo_log_exit("hide-maps")', 'kpm_demo_log_exit("vmap")'),
    ('kpm_demo_log_init("kload"', 'kpm_demo_log_init("kld"'),
    ('kpm_demo_log_exit("kload")', 'kpm_demo_log_exit("kld")'),
    ('kpm_demo_log_init("dbc-rw"', 'kpm_demo_log_init("vrw"'),
    ('kpm_demo_log_exit("dbc-rw")', 'kpm_demo_log_exit("vrw")'),
    ('"stealth hm=', '"veilk hm='),
    ("add_kpm_module(stealth", "add_kpm_module(veilk"),
    ("target_compile_definitions(stealth_obj", "target_compile_definitions(veilk_obj"),
    ("target_include_directories(stealth_obj", "target_include_directories(veilk_obj"),
    ("add_kpm_module(hide-maps", "add_kpm_module(vmap"),
    ("add_kpm_module(kload", "add_kpm_module(kld"),
    ("add_kpm_module(wxshadow", "add_kpm_module(wxsh"),
    ("add_kpm_module(dbc-rw", "add_kpm_module(vrw"),
    ("add_executable(wxshadow_client", "add_executable(veil_wx"),
    ("kpms/stealth/", "kpms/veilk/"),
    ("kpms/hide-maps/", "kpms/vmap/"),
    ("kpms/kload/", "kpms/kld/"),
    ("kpms/wxshadow/", "kpms/wxsh/"),
    ("kpms/dbc-rw/", "kpms/vrw/"),
    ("tools/stealth_inject/", "tools/veil_inject/"),
    ("tools/dbc_rw/", "tools/veil_rw/"),
    ("mkpms-stealth", "mkpms-veil"),
    # leftover project name
    ("# stealth.kpm", "# veilk.kpm"),
    ("stealth.kpm —", "veilk.kpm —"),
    ("# stealth —", "# veilk —"),
    ("stealth_init", "veilk_init"),
    ("stealth_exit", "veilk_exit"),
    ("stealth_control0", "veilk_control0"),
    ("g_path_stealth", "g_path_veilk"),
    # ctl kpatch name
    ("kpm ctl0 stealth", "kpm ctl0 veilk"),
    ("kpm unload stealth", "kpm unload veilk"),
    ("kpm unload dbc", "kpm unload vrw"),
    # remaining obvious "stealth" as kpm name in user-facing strings
    ("loading stealth", "loading veilk"),
    ("Unified: hide-maps + kload + wxshadow + dbc-rw",
     "Unified: vmap + kld + wxsh + vrw"),
]


def is_text(path: Path) -> bool:
    if path.name in TEXT_NAMES or path.name.startswith("CMakeLists"):
        return True
    if path.suffix.lower() in TEXT_EXT:
        return True
    return False


def mutate_text(data: str) -> str:
    for old, new in REPLACEMENTS:
        data = data.replace(old, new)
    return data


def copy_tree() -> None:
    if DST.exists():
        shutil.rmtree(DST)
    def ignore(dirpath, names):
        skip = set()
        if ".git" in names:
            skip.add(".git")
        # skip heavy prebuilt binaries in the example
        base = Path(dirpath)
        rel = base.relative_to(SRC) if SRC in base.parents or base == SRC else None
        if rel is not None:
            s = str(rel).replace("\\", "/")
            if s.endswith("examples/dbc_lostgo_hook/outputs") or s.endswith(
                "examples/dbc_lostgo_hook/prebuilt"
            ):
                return set(names)
            if s.endswith("examples/dbc_lostgo_hook/DBC/libs"):
                return set()  # keep libdobby.a for rebuild
        if "_mutate.py" in names:
            skip.add("_mutate.py")
        return skip

    shutil.copytree(SRC, DST, ignore=ignore, symlinks=True)


def rewrite_files() -> int:
    n = 0
    for p in DST.rglob("*"):
        if not p.is_file() or p.is_symlink():
            continue
        if not is_text(p):
            continue
        raw = p.read_bytes()
        # skip obvious binaries
        if b"\0" in raw[:4096]:
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            try:
                text = raw.decode("latin-1")
            except Exception:
                continue
        new = mutate_text(text)
        if new != text:
            p.write_text(new, encoding="utf-8")
            n += 1
    return n


def rename_paths() -> list[str]:
    """Rename files/dirs after content rewrite. Deepest paths first."""
    mapping = [
        ("kpms/stealth/stealth.c", "kpms/stealth/veilk.c"),
        ("kpms/stealth", "kpms/veilk"),
        ("kpms/hide-maps", "kpms/vmap"),
        ("kpms/kload", "kpms/kld"),
        ("kpms/wxshadow/wxshadow_client.c", "kpms/wxshadow/veil_wx.c"),
        ("kpms/wxshadow", "kpms/wxsh"),
        ("kpms/dbc-rw", "kpms/vrw"),
        ("tools/stealth_inject/stealth_inject.c", "tools/stealth_inject/veil_inject.c"),
        ("tools/stealth_inject/stealth_inject.h", "tools/stealth_inject/veil_inject.h"),
        ("tools/stealth_inject", "tools/veil_inject"),
        ("tools/dbc_rw", "tools/veil_rw"),
        ("examples/dbc_lostgo_hook/launcher/stealth_inject.h",
         "examples/dbc_lostgo_hook/launcher/veil_inject.h"),
    ]
    done = []
    for old, new in mapping:
        src = DST / old
        dst = DST / new
        if not src.exists():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        src.rename(dst)
        done.append(f"{old} → {new}")
    return done


def patch_cmake_after_rename() -> None:
    p = DST / "kpms/veilk/CMakeLists.txt"
    if p.exists():
        t = p.read_text()
        t = t.replace("stealth.c", "veilk.c")
        t = t.replace("../hide-maps/", "../vmap/")
        t = t.replace("../kload/", "../kld/")
        t = t.replace("../wxshadow/", "../wxsh/")
        t = t.replace("../dbc-rw/", "../vrw/")
        p.write_text(t)

    p = DST / "kpms/vmap/CMakeLists.txt"
    if p.exists():
        t = p.read_text()
        t = t.replace("hidemaps.c", "hidemaps.c")
        p.write_text(t)

    p = DST / "kpms/kld/CMakeLists.txt"
    if p.exists():
        t = p.read_text()
        t = t.replace("kload.c", "kload.c")
        p.write_text(t)

    p = DST / "kpms/wxsh/CMakeLists.txt"
    if p.exists():
        t = p.read_text()
        t = t.replace("wxshadow_client.c", "veil_wx.c")
        p.write_text(t)

    p = DST / "tools/veil_inject/build.sh"
    if p.exists():
        t = p.read_text()
        t = t.replace("stealth_inject", "veil_inject")
        p.write_text(t)

    # launcher include rename
    launcher = DST / "examples/dbc_lostgo_hook/launcher"
    if launcher.exists():
        for f in launcher.glob("*"):
            if f.is_file() and is_text(f):
                t = f.read_text(encoding="utf-8", errors="ignore")
                nt = t.replace('"stealth_inject.h"', '"veil_inject.h"')
                nt = nt.replace("stealth_inject.h", "veil_inject.h")
                if nt != t:
                    f.write_text(nt)


def blank_hardcoded_key() -> None:
    """Do not carry the public example superkey into the variant."""
    p = DST / "examples/dbc_lostgo_hook/launcher/dbc_main.c"
    if not p.exists():
        return
    t = p.read_text()
    # original had a hardcoded KernelPatch superkey
    t = t.replace(
        'static const char *g_key = "Lkdj4K46iwowb3K+-+@+++";',
        'static const char *g_key = ""; /* pass --key */',
    )
    p.write_text(t)


def write_variant_doc() -> None:
    (DST / "VARIANT.md").write_text(
        """# mkpms-veil — fingerprint-mutated fork of mkpms-stealth

Local lab variant. Original clone: `../mkpms-stealth` (commit f81d21b).

This is **not** a functional rewrite. Control flow, hooks, and the four-in-one
KPM layout are unchanged. Only identifiers that anti-cheat / kpm-list / dmesg
/ YARA-style scans would match have been remapped.

## ABI remap

| Channel | Original | Variant |
|---------|----------|---------|
| Unified KPM name (`kpm list`) | `stealth` | `veilk` |
| Author | `dbc` | `nx` |
| Version | `1.1.0` | keep per-submodule; fused description rewritten |
| hide-maps prctl | `0x484Dxxxx` (`HM`) | `0xA71Cxxxx` |
| kload prctl | `0x4B4Cxxxx` (`KL`) | `0xC93Exxxx` |
| wxshadow prctl | `0x5758xxxx` (`WX`) | `0xB62Fxxxx` |
| r/w syscall | 41 (`socket`) | 41 (unchanged) |
| r/w magic | `0x1b1841fd2c1e0000` | `0x3c27a9e81d4b0000` |
| r/w version | `0x20260725` | `0x20260821` |
| BRK imm | `0x007` | `0x013` |
| dmesg prefix | `stealth:` / `hide-maps:` / `kload:` / `wxshadow:` / `dbc-rw:` | `vk:` / `vm:` / `kl:` / `ws:` / `vr:` |
| default so / log tag | `libdbc.so` / `DbcHK` | `libvkcore.so` / `VkHK` |
| injector | `stealth_inject` | `veil_inject` |
| r/w userspace | `dbc_rw` | `veil_rw` (dir) |

Standalone KPM names: `vmap` / `kld` / `wxsh` / `vrw`.

Kernel and userspace headers were mutated **together**. Mixing `stealth.kpm`
with `veil_inject` (or vice versa) will fail PING.

## Layout

```
kpms/veilk/veilk.c     fused KPM
kpms/vmap/hidemaps.c
kpms/kld/kload.c
kpms/wxsh/             W^X shadow + veil_wx client
kpms/vrw/dbcrw.c
tools/veil_inject/
tools/veil_rw/
```

## Build (same as upstream)

```bash
./setup-kp.sh
ln -sfn .kp/kernel kernel
mkdir -p build && cd build
cmake -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc ..
cmake --build . --target veilk.kpm -j
# product: build/kpms/veilk/veilk.kpm
```

Load:

```bash
kpatch KEY kpm load /data/local/tmp/veilk.kpm
kpatch KEY kpm ctl0 veilk
```

## Still original (intentionally)

- Syscall number 41 (socket) as the r/w transport. Changing NR is a bigger
  behavioral mutation; magic is the unique selector.
- Hook targets: `show_map_vma`, `show_smap`, `prctl`, `filldir64`,
  `clock_gettime`/`futex`/… after-hooks, `copy_process`/`exit_mmap`.
- Algorithm: seq_file line filter, syscall-exit dlopen hijack, W^X shadow
  pages, page-table walk r/w cache.
- GPL-3.0 license inherited from upstream.

## Detection leftover (honest)

Mutating strings/magics defeats **signature** matches on this family. It does
not hide:

- `kpm list` still shows a loaded module (`veilk`)
- hooked `sys_prctl` + hooked syscall 41
- maps filter behavioral (missing VMAs vs `/proc/pid/mem` / `dl_iterate_phdr`)
- kload hijack: sudden `pc=dlopen` on an unrelated syscall return
- wxshadow: BRK with a new imm, PTE execute/read split, extra pages

Those need a different design, not a rename.
""",
        encoding="utf-8",
    )


def main() -> int:
    print(f"[*] copy {SRC} -> {DST}")
    copy_tree()
    print("[*] rewrite text")
    n = rewrite_files()
    print(f"    {n} files rewritten")
    print("[*] rename paths")
    for line in rename_paths():
        print(f"    {line}")
    patch_cmake_after_rename()
    blank_hardcoded_key()
    write_variant_doc()
    print(f"[+] variant ready: {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
