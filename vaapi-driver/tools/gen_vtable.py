#!/usr/bin/env python3
"""Extract function-pointer signatures from VADriverVTable in va_backend.h.

libva requires every checked vtable slot during vaInitialize. Extract
signatures directly from the header to avoid transcribing them by hand.

Output: JSON list [{name, ret, params: [{type, name, array}]}, ...].
"""
import json
import re
import sys

HEADER = sys.argv[1] if len(sys.argv) > 1 else "container-headers/va_backend.h"

with open(HEADER, encoding="utf-8") as fh:
    src = fh.read()

# 1) Extract struct VADriverVTable { ... };
m = re.search(r"struct VADriverVTable\s*\{(.*?)\n\};", src, re.S)
if not m:
    sys.exit("Could not find struct VADriverVTable")
body = m.group(1)

# 2) Strip block and line comments.
body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
body = re.sub(r"//[^\n]*", "", body)

# 3) Match function-pointer members, such as <ret>(*name)(params);
#    For example: VAStatus(*vaTerminate)(VADriverContextP ctx);
# The return type and (*name) may be on separate lines, as for
# vaExportSurfaceHandle. Allow newlines in the return type pattern.
pattern = re.compile(
    r"([A-Za-z_][A-Za-z_0-9 \t\*\r\n]*?)"       # Return type may span lines.
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
        # The vtable has no function-pointer parameters, so split on commas.
        for part in raw.split(","):
            part = part.strip()
            # The last identifier is the parameter name, optionally followed by dimensions.
            mm = re.match(r"^(.*?)([A-Za-z_][A-Za-z_0-9]*)\s*((?:\[[^\]]*\])*)$", part)
            if not mm:
                sys.exit(f"Could not parse parameter {part!r} in {name}")
            ptype, pname, arr = mm.group(1).strip(), mm.group(2), mm.group(3)
            plist.append({"type": ptype, "name": pname, "array": arr})
    funcs.append({"name": name, "ret": ret, "params": plist})

json.dump(funcs, sys.stdout, indent=1, ensure_ascii=False)
print()
print(f"# {len(funcs)} vtable slots", file=sys.stderr)
