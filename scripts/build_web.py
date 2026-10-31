#!/usr/bin/env python3
"""Fold the browser test bench into ONE self-contained file: dist/quark.html.

Inlines build/quark.wasm (base64), web/worklet.js, the generated META, the shared UI helpers and
the device's own canvas.js / cards.js, so the page opens straight from disk (file://) with no
server. Run scripts/build.sh first (it compiles the wasm and runs tools/gen.py)."""
import base64
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*p):
    with open(os.path.join(ROOT, *p), encoding="utf-8") as f:
        return f.read()


def main():
    wasm = base64.b64encode(open(os.path.join(ROOT, "build", "quark.wasm"), "rb").read()).decode()
    html = read("web", "app.html")
    scripts = "\n".join([
        read("build", "web", "meta_gen.js"),
        read("src", "ui", "common.js"),
        read("build", "ui", "canvas.js"),
        read("build", "ui", "cards.js"),
        "var QUARK_WORKLET = " + json.dumps(read("web", "worklet.js")) + ";",
        'var QUARK_WASM_B64 = "' + wasm + '";',
        read("web", "app.js"),
    ])
    assert "</script" not in scripts.replace("<\\/script", ""), "a script contains </script>"
    html = html.replace("/*@@STYLE@@*/", read("web", "style.css")).replace("/*@@SCRIPTS@@*/", scripts)
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", "quark.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote %s (%d KB)" % (out, len(html) // 1024))


if __name__ == "__main__":
    main()
