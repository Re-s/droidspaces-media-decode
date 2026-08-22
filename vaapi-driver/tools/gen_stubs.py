#!/usr/bin/env python3
"""生成 VA-API driver 的 vtable 桩函数与装配代码。

libva 在 vaInitialize 时强制校验 39 个 vtable 槽位非 NULL（va/va.c 的 CHECK_VTABLE），
任一为 NULL 则整个初始化失败并 dlclose。因此未实现的槽位必须指向返回
VA_STATUS_ERROR_UNIMPLEMENTED 的桩函数，而不能留空。

本脚本从 vtable.json（由 gen_vtable.py 从 va_backend.h 抽取）生成：
  - stubs.c  ：所有未实现槽位的桩函数定义
  - vtable.inc：vtable 装配的初始化列表片段

已实现的槽位在 IMPLEMENTED 里列出，脚本会跳过它们的桩生成，
但仍会在 vtable.inc 里引用其真实函数名（约定为 dmd_<去掉 va 前缀>）。
"""
import json
import sys

# 由 driver 真实实现的槽位（其余生成 UNIMPLEMENTED 桩）
IMPLEMENTED = {
    "vaTerminate",
    "vaQueryConfigProfiles",
    "vaQueryConfigEntrypoints",
    "vaGetConfigAttributes",
    "vaCreateConfig",
    "vaDestroyConfig",
    "vaQueryConfigAttributes",
    "vaQuerySurfaceAttributes",
    "vaQueryImageFormats",
    "vaQueryDisplayAttributes",
    "vaGetDisplayAttributes",
    "vaSetDisplayAttributes",
    "vaQuerySubpictureFormats",
    # ---- 解码数据路径（decode.c） ----
    "vaCreateSurfaces",
    "vaCreateSurfaces2",
    "vaDestroySurfaces",
    "vaCreateContext",
    "vaDestroyContext",
    "vaCreateBuffer",
    "vaBufferSetNumElements",
    "vaMapBuffer",
    # 必须实现：libva 只在槽位为 NULL 时才回落到 vaMapBuffer，
    # 而我们所有槽位都填了桩，留桩会让 ffmpeg 的回读路径直接失败。
    "vaMapBuffer2",
    "vaUnmapBuffer",
    "vaDestroyBuffer",
    "vaBufferInfo",
    "vaBeginPicture",
    "vaRenderPicture",
    "vaEndPicture",
    "vaSyncSurface",
    "vaSyncSurface2",
    "vaQuerySurfaceStatus",
    # ---- VAImage 出口（image.c） ----
    "vaCreateImage",
    "vaDeriveImage",
    "vaDestroyImage",
    "vaGetImage",
}

funcs = json.load(open(sys.argv[1], encoding="utf-8"))


def impl_name(va_name: str) -> str:
    """vaQueryConfigProfiles -> dmd_QueryConfigProfiles"""
    assert va_name.startswith("va")
    return "dmd_" + va_name[2:]


def render_params(params):
    """渲染形参列表，未使用的参数加 (void) 消警告需在函数体处理。"""
    if not params:
        return "void"
    out = []
    for p in params:
        t = p["type"]
        # 指针类型与变量名之间不留空格更贴合项目风格
        sep = "" if t.endswith("*") else " "
        out.append(f"{t}{sep}{p['name']}{p['array']}")
    return ", ".join(out)


# ---------- stubs.c ----------
lines = [
    "/* 本文件由 tools/gen_stubs.py 自动生成，请勿手工修改。",
    " *",
    " * libva 在 vaInitialize 中逐个校验 vtable 槽位非 NULL（va/va.c CHECK_VTABLE），",
    " * 任一为 NULL 会导致整个初始化失败。未实现的入口因此必须存在并返回",
    " * VA_STATUS_ERROR_UNIMPLEMENTED，而不能留空指针。",
    " */",
    "",
    '#include "driver.h"',
    "",
]

stub_names = []
for f in funcs:
    if f["name"] in IMPLEMENTED:
        continue
    name = impl_name(f["name"])
    stub_names.append((f["name"], name))
    lines.append(f"{f['ret']} {name}({render_params(f['params'])})")
    lines.append("{")
    for p in f["params"]:
        lines.append(f"    (void){p['name']};")
    lines.append("    return VA_STATUS_ERROR_UNIMPLEMENTED;")
    lines.append("}")
    lines.append("")

with open("out/stubs.c", "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))

# ---------- stubs.h ----------
hl = [
    "/* 本文件由 tools/gen_stubs.py 自动生成，请勿手工修改。 */",
    "",
    "#ifndef DMD_STUBS_H",
    "#define DMD_STUBS_H",
    "",
    '#include <va/va_backend.h>',
    "",
]
for va_name, name in stub_names:
    f = next(x for x in funcs if x["name"] == va_name)
    hl.append(f"{f['ret']} {name}({render_params(f['params'])});")
hl += ["", "#endif /* DMD_STUBS_H */", ""]
with open("out/stubs.h", "w", encoding="utf-8") as fh:
    fh.write("\n".join(hl))

# ---------- vtable.inc ----------
vl = [
    "/* 本文件由 tools/gen_stubs.py 自动生成，请勿手工修改。",
    " * vtable 装配：覆盖 va_backend.h 中 VADriverVTable 的全部 %d 个槽位。" % len(funcs),
    " */",
    "",
]
for f in funcs:
    vl.append(f"    vtable->{f['name']} = {impl_name(f['name'])};")
vl.append("")
with open("out/vtable.inc", "w", encoding="utf-8") as fh:
    fh.write("\n".join(vl))

print(f"槽位总数 {len(funcs)}，真实现 {len(funcs)-len(stub_names)}，桩 {len(stub_names)}")
