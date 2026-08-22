#!/usr/bin/env python3
"""从 va_backend.h 精确抽取 VADriverVTable 的全部函数指针签名。

用途：VA-API driver 必须填满 libva 强制校验的所有 vtable 槽位，
任一为 NULL 会导致 vaInitialize 失败。手抄 60+ 个签名易错，这里做确定性抽取。

输出：JSON 列表 [{name, ret, params:[{type, name}]}, ...]
"""
import json
import re
import sys

HEADER = sys.argv[1] if len(sys.argv) > 1 else "container-headers/va_backend.h"

with open(HEADER, encoding="utf-8") as fh:
    src = fh.read()

# 1) 切出 struct VADriverVTable { ... };
m = re.search(r"struct VADriverVTable\s*\{(.*?)\n\};", src, re.S)
if not m:
    sys.exit("未找到 struct VADriverVTable")
body = m.group(1)

# 2) 去注释（/* */ 与 //）
body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
body = re.sub(r"//[^\n]*", "", body)

# 3) 匹配函数指针成员：<ret>(*name)(params);
#    头文件里形如 "VAStatus(*vaTerminate)(VADriverContextP ctx);"
# 注意：头文件里返回类型与 (*name) 之间可能换行（如 vaExportSurfaceHandle），
# 所以返回类型部分要允许换行，否则会漏槽位。
pattern = re.compile(
    r"([A-Za-z_][A-Za-z_0-9 \t\*\r\n]*?)"       # 返回类型（可跨行）
    r"\(\s*\*\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)"  # (*name)
    r"\s*\((.*?)\)\s*;",                        # (params);
    re.S,
)

funcs = []
for ret, name, params in pattern.findall(body):
    ret = " ".join(ret.split())
    raw = " ".join(params.split())
    if raw in ("void", ""):
        plist = []
    else:
        plist = []
        # 参数间以逗号分隔；vtable 里没有函数指针型参数，简单切分即可
        for part in raw.split(","):
            part = part.strip()
            # 分离类型与参数名：最后一个标识符是名字（数组形如 name[3]）
            mm = re.match(r"^(.*?)([A-Za-z_][A-Za-z_0-9]*)\s*((?:\[[^\]]*\])*)$", part)
            if not mm:
                sys.exit(f"无法解析参数 {part!r} in {name}")
            ptype, pname, arr = mm.group(1).strip(), mm.group(2), mm.group(3)
            plist.append({"type": ptype, "name": pname, "array": arr})
    funcs.append({"name": name, "ret": ret, "params": plist})

json.dump(funcs, sys.stdout, indent=1, ensure_ascii=False)
print()
print(f"# 共 {len(funcs)} 个 vtable 槽位", file=sys.stderr)
