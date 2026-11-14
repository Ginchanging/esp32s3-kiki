#!/usr/bin/env bash
# Render simulator screenshots into shots/ plus contact sheets (shots/sheet_*.png):
#   sheet_pages_zh / _en   the 6 pages
#   sheet_routine          Kiki through the day (every activity)
#   sheet_reactions        CI pass / fail / start, server down / up, Muse, touch, morning
#   sheet_moods            worried / nervous / confused, night, offline, boot
#   sheet_details          failures, Mi Home notices, Kiki's diary, knob toasts
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIM="$ROOT/build-sim/dash_sim"
OUT="$ROOT/shots"
FX="$ROOT/fixtures"
cmake --build "$ROOT/build-sim" >/dev/null
mkdir -p "$OUT"
rm -f "$OUT"/s_*.png "$OUT"/sheet_*.png

shot() { # name, args...
  local name="$1"; shift
  "$SIM" --shot "$OUT/$name.bmp" "$@" >/dev/null
  sips -s format png "$OUT/$name.bmp" --out "$OUT/s_$name.png" >/dev/null
  rm "$OUT/$name.bmp"
}

for lang in zh en; do
  for page in 0 1 2 3 4 5; do
    shot "page_${lang}_$page" --fixture "$FX/1_ok.json" --page "$page" --lang "$lang"
  done
done
acts=(sleep exercise breakfast class lunch nap code dinner free wash sunbathe)
for i in "${!acts[@]}"; do
  shot "act_${i}_${acts[$i]}" --fixture "$FX/1_ok.json" --act "$i" --ms 300
done
reacts=(none ci_pass ci_fail ci_start server_down server_up muse touch morning pomo_done)
for i in 1 2 3 4 5 7 8; do
  shot "react_${i}_${reacts[$i]}" --fixture "$FX/1_ok.json" --react "$i" --ms 160
done
shot "react_6_muse" --fixture "$FX/1_ok.json" --muse "Muse：今晚十点前把论文初稿发给老师，别忘了附上参考文献列表。" --ms 100
shot "mood_worried" --fixture "$FX/2_gh_failed.json" --act 6 --ms 300
shot "mood_nervous" --fixture "$FX/4_server_down.json" --act 6 --ms 300
shot "mood_confused" --fixture "$FX/5_stale.json" --act 6 --ms 300
shot "mood_night" --fixture "$FX/1_ok.json" --act 0 --night --ms 300
shot "mood_offline" --fixture "$FX/6_offline.json" --ms 300
shot "mood_boot" --fixture "$FX/7_boot.json" --ms 300
shot "det_gh_failed" --fixture "$FX/2_gh_failed.json" --page 1
shot "det_srv_down" --fixture "$FX/4_server_down.json" --page 2
shot "det_mail" --fixture "$FX/8_muse_sensor.json" --page 4
shot "det_mi_done" --fixture "$FX/1_ok.json" --page 5 --mi-done
shot "det_mi_none" --fixture "$FX/2_gh_failed.json" --page 5 --lang en
shot "det_diary_zh" --fixture "$FX/1_ok.json" --page 4 --diary day
shot "det_weekly_en" --fixture "$FX/1_ok.json" --page 4 --diary week --lang en
shot "det_knob_bri" --fixture "$FX/1_ok.json" --page 5 --knob hold:3
shot "det_knob_pomo" --fixture "$FX/1_ok.json" --page 3 --knob long --lang en

python3 - "$OUT" <<'PY'
import sys, pathlib
try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit(0)
out = pathlib.Path(sys.argv[1])
def sheet(name, prefix, cols=4):
    files = sorted(out.glob(f"s_{prefix}*.png"), key=lambda p: [int(t) if t.isdigit() else t for t in p.stem.split("_")])
    if not files:
        return
    ims = [Image.open(p) for p in files]
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    s = Image.new("RGB", (cols * (w + 8) + 8, rows * (h + 26) + 8), (40, 40, 40))
    d = ImageDraw.Draw(s)
    for i, (im, p) in enumerate(zip(ims, files)):
        x, y = 8 + (i % cols) * (w + 8), 8 + (i // cols) * (h + 26)
        d.text((x, y), p.stem[2:], fill=(220, 220, 220))
        s.paste(im, (x, y + 18))
    s.save(out / f"sheet_{name}.png")
sheet("pages_zh", "page_zh", 6)
sheet("pages_en", "page_en", 6)
sheet("routine", "act_", 6)
sheet("reactions", "react_", 4)
sheet("moods", "mood_", 3)
sheet("details", "det_", 3)
PY
ls "$OUT"/sheet_*.png | xargs -n1 basename
