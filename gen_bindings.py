# SPDX-License-Identifier: Apache-2.0 OR MIT
"""Emit the GDExtension symbols this extension needs, straight from extension_api.json.

WHY THERE IS NO godot-cpp HERE. GDExtension is a C ABI, not a C++ library. godot-cpp is a
convenience layer that generates every class in the engine and takes minutes to compile. This
extension overrides six virtuals and calls seven methods. Binding those by hand against
`gdextension_interface.h` costs one small header and no build dependency.

`classdb_get_method_bind` takes a class name, a method name and a HASH. The hash is derived
from the signature, so it is the thing that decides whether a binding is still valid. Those
hashes live in `extension_api.json`, which is what "use the sigs to provide the symbols"
means in practice.

THE CLAIM THIS SCRIPT EXISTS TO GATE. Every hash we depend on is identical in Godot 4.0
through 4.5, so one binary serves the whole line. That is a measurement and measurements
drift, so it is re-checked here rather than written in a comment. If a future Godot changes
one of these signatures, this script fails and the build stops.

Run:  python gen_bindings.py                 # check against the pinned api files, emit header
      python gen_bindings.py --check         # check only, write nothing
"""

import argparse
import json
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "src", "gdextension_symbols.h")

# The versions one binary must serve. 4.0 is listed so the hash comparison covers it, but the
# extension does not target 4.0. Godot 4.0 calls the entry point with `const GDExtensionInterface *`
# and 4.1 replaced that with `GDExtensionInterfaceGetProcAddress`. Godot 4.1 declares a
# `GDExtensionLegacyInitializationFunction` typedef and never uses it, so a 4.0 build does not
# load in 4.1. The floor is therefore 4.1, and it was read from gdextension.cpp, not recalled.
SOURCES = {
    "4.0": "https://raw.githubusercontent.com/godotengine/godot-cpp/4.0/gdextension/extension_api.json",
    "4.1": "https://raw.githubusercontent.com/godotengine/godot-cpp/4.1/gdextension/extension_api.json",
    "4.2": "https://raw.githubusercontent.com/godotengine/godot-cpp/4.2/gdextension/extension_api.json",
    "4.3": "https://raw.githubusercontent.com/godotengine/godot-cpp/master/gdextension/extension_api-4-3.json",
    "4.4": "https://raw.githubusercontent.com/godotengine/godot-cpp/master/gdextension/extension_api-4-4.json",
    "4.5": "https://raw.githubusercontent.com/godotengine/godot-cpp/master/gdextension/extension_api-4-5.json",
}
FLOOR = "4.2"

# Exactly what the extension calls. Keeping this list short is the point: every entry is a
# symbol we must keep working, so an unused binding is a maintenance cost with no benefit.
#
# `get_width` and `get_height` come back with the SAME hash, and that is correct rather than a
# collision to fix. Godot derives the hash from the signature, not the name, and both are
# `int f() const`. `classdb_get_method_bind` takes the name AND the hash, so the pair resolves.
METHODS = [
    ("MovieWriter", "add_writer"),
    ("OS", "get_processor_count"),
    ("Image", "create_from_data"),
    ("Image", "convert"),
    ("Image", "get_data"),
    ("Image", "get_format"),
    ("Image", "get_width"),
    ("Image", "get_height"),
]

# The virtuals Godot calls on us. These carry no hash. The engine asks for them by name
# through the class's `get_virtual` callback, so the names are the contract.
VIRTUALS = [
    "_handles_file",
    "_get_audio_mix_rate",
    "_get_audio_speaker_mode",
    "_write_begin",
    "_write_frame",
    "_write_end",
]


def load(url):
    with urllib.request.urlopen(
            urllib.request.Request(url, headers={"User-Agent": "weftspun"})) as fh:
        return json.load(fh)


def hashes(api):
    out = {}
    for cls in api["classes"]:
        for want_cls, want_m in METHODS:
            if cls["name"] != want_cls:
                continue
            for m in cls.get("methods", []):
                if m["name"] == want_m:
                    out[(want_cls, want_m)] = m.get("hash")
    return out


def virtual_names(api):
    got = set()
    for cls in api["classes"]:
        if cls["name"] == "MovieWriter":
            for m in cls.get("methods", []):
                if m.get("is_virtual"):
                    got.add(m["name"])
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="verify only, write no header")
    args = ap.parse_args()

    tables, virts = {}, {}
    for ver, url in SOURCES.items():
        api = load(url)
        tables[ver] = hashes(api)
        virts[ver] = virtual_names(api)

    problems = []
    for key in METHODS:
        seen = {v: tables[v].get(key) for v in SOURCES}
        missing = [v for v, h in seen.items() if h is None]
        if missing:
            problems.append("%s.%s absent in %s" % (key[0], key[1], ", ".join(missing)))
            continue
        distinct = set(seen.values())
        mark = "same" if len(distinct) == 1 else "DIFFERS"
        print("  %-28s %-12s %s" % ("%s.%s" % key, seen[FLOOR], mark))
        if len(distinct) != 1:
            problems.append("%s.%s hash differs across versions: %s" % (key[0], key[1], seen))

    for name in VIRTUALS:
        absent = [v for v in SOURCES if name not in virts[v]]
        if absent:
            problems.append("virtual %s absent in %s" % (name, ", ".join(absent)))

    if problems:
        print("\nFAIL. The single binary assumption no longer holds:")
        for p in problems:
            print("   " + p)
        # A generator that emits a header anyway would bake the drift into the build and the
        # failure would surface as a crash inside the engine instead of here.
        return 1

    print("\n%d methods and %d virtuals identical across %s"
          % (len(METHODS), len(VIRTUALS), ", ".join(SOURCES)))

    if args.check:
        return 0

    lines = [
        "// SPDX-License-Identifier: Apache-2.0 OR MIT",
        "// GENERATED by gen_bindings.py. Do not edit.",
        "//",
        "// Method hashes come from extension_api.json. Each one is identical in Godot %s,"
        % ", ".join(SOURCES),
        "// so one binary serves the whole line. gen_bindings.py re-checks that and fails if",
        "// a future release changes a signature.",
        "#ifndef GDEXTENSION_SYMBOLS_H",
        "#define GDEXTENSION_SYMBOLS_H",
        "",
        '#define CINEFORM_GODOT_FLOOR "%s"' % FLOOR,
        "",
    ]
    for cls, m in METHODS:
        macro = "HASH_%s_%s" % (cls.upper(), m.upper())
        lines.append("#define %-34s %sULL" % (macro, tables[FLOOR][(cls, m)]))
    lines += ["", "#endif // GDEXTENSION_SYMBOLS_H", ""]

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf8") as fh:
        fh.write("\n".join(lines))
    print("wrote %s" % os.path.relpath(OUT, HERE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
