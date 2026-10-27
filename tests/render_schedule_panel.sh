#!/usr/bin/env bash
# 无显示器渲染日程面板，产出 PNG 供人工核对（跨天显示起止、当天只显示开始）。
# 用法：nix develop --command tests/render_schedule_panel.sh [输出目录]
set -euo pipefail
out="${1:-/tmp}"
cd "$(dirname "$0")/.."
make -f Makefile.test test
make >/dev/null
SDL_VIDEODRIVER=dummy ./build/emu/emulator "$out/dash.ppm"
python3 - "$out" <<'PY'
import struct, sys, zlib
out = sys.argv[1]
data = open(f"{out}/dash.ppm", "rb").read()
head = data.index(b"255\n") + 4
px = data[head:]
w = 800

def crop(x0, y0, x1, y1, path):
    cw, ch = x1 - x0, y1 - y0
    rows = [px[(y * w + x0) * 3:(y * w + x1) * 3] for y in range(y0, y1)]
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", cw, ch, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)

crop(420, 8, 800, 190, f"{out}/schedule-panel.png")
print(f"{out}/schedule-panel.png")
PY
