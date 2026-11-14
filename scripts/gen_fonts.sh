#!/usr/bin/env bash
# Generate the dashboard's LVGL fonts from Noto Sans SC (SIL OFL 1.1).
#   dash_font_zh16  : GB2312 level-1 (3755 common chars) + every UI char + ASCII, 16px
#                     -> Chinese UI body text and Muse messages (any UI language)
#   dash_font_zh12  : only the UI's own Chinese chars + ASCII, 12px (small labels)
#   dash_font_zh20  : only the UI's own Chinese chars + ASCII, 20px (titles)
#   dash_font_clock : digits and ':' only, 48px (big clock)
# LV_SYMBOL_* icons come from the Montserrat fallback set on each font.
# Re-run after adding Chinese text to components/dash_ui or components/dash_model,
# or after renaming a device in server/mihome.json.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FONTS="$ROOT/tools/fonts"
OUT="$ROOT/components/dash_ui/fonts"
BPP16="${BPP16:-3}"
CONV="npx --yes lv_font_conv@1.5.3"
mkdir -p "$FONTS" "$OUT"

fetch() { # name
  local f="$FONTS/$1"
  if [ ! -s "$f" ]; then
    echo "downloading $1"
    curl -fL --retry 3 -o "$f.part" \
      "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/$1"
    mv "$f.part" "$f"
  fi
}
fetch NotoSansSC-Regular.otf
fetch NotoSansSC-Medium.otf

python3 -I - "$ROOT" "$FONTS" <<'PY'
import pathlib, sys
root, fonts = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
ui = set()
for d in ("components/dash_ui", "components/dash_model", "sim"):
    for p in (root / d).rglob("*.c"):
        if "/fonts/" in str(p):
            continue
        ui.update(ch for ch in p.read_text(encoding="utf-8") if ord(ch) > 0x7F)
mihome = root / "server" / "mihome.json"  # Mi Home device names shown in small/title fonts
if mihome.exists():
    ui.update(ch for ch in mihome.read_text(encoding="utf-8") if ord(ch) > 0x7F)
punct = set("，。！？：；、“”‘’（）【】《》…—·～￥％＋－×÷℃°")
ui |= punct
gb = set()
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            gb.add(bytes([hi, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
full = sorted(gb | ui)
(fonts / "chars_ui.txt").write_text("".join(sorted(ui)), encoding="utf-8")
(fonts / "chars_full.txt").write_text("".join(full), encoding="utf-8")
print(f"ui chars: {len(ui)}  full set: {len(full)} (GB2312 L1: {len(gb)})")
PY

UI_CHARS="$(cat "$FONTS/chars_ui.txt")"
FULL_CHARS="$(cat "$FONTS/chars_full.txt")"

$CONV --size 16 --bpp "$BPP16" --format lvgl --font "$FONTS/NotoSansSC-Regular.otf" \
  -r 0x20-0x7E --symbols "$FULL_CHARS" \
  --lv-font-name dash_font_zh16 --lv-fallback lv_font_montserrat_16 --lv-include lvgl.h \
  -o "$OUT/dash_font_zh16.c"
$CONV --size 12 --bpp 4 --format lvgl --font "$FONTS/NotoSansSC-Regular.otf" \
  -r 0x20-0x7E --symbols "$UI_CHARS" \
  --lv-font-name dash_font_zh12 --lv-fallback lv_font_montserrat_12 --lv-include lvgl.h \
  -o "$OUT/dash_font_zh12.c"
$CONV --size 20 --bpp 4 --format lvgl --font "$FONTS/NotoSansSC-Medium.otf" \
  -r 0x20-0x7E --symbols "$UI_CHARS" \
  --lv-font-name dash_font_zh20 --lv-fallback lv_font_montserrat_20 --lv-include lvgl.h \
  -o "$OUT/dash_font_zh20.c"
$CONV --size 48 --bpp 4 --format lvgl --font "$FONTS/NotoSansSC-Medium.otf" \
  --symbols "0123456789:-" \
  --lv-font-name dash_font_clock --lv-include lvgl.h \
  -o "$OUT/dash_font_clock.c"
ls -l "$OUT"
