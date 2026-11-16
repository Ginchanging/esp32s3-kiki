#!/usr/bin/env python3
"""Render all eleven holiday rooms with a fresh, deterministic fixture."""
import copy
import datetime as dt
import json
import pathlib
import subprocess
import sys
from gen_holidays import dates

root = pathlib.Path(__file__).resolve().parents[1]
sim, out = sys.argv[1], pathlib.Path(sys.argv[2])
base = json.loads((root / "fixtures/1_ok.json").read_text())
year, lunar = next(dates())
holidays = dict(zip(["spring", "lantern", "dragon", "qixi", "chongyang", "solstice"], lunar))
holidays.update(national=dt.date(2026, 10, 1), halloween=dt.date(2026, 10, 31),
                thanksgiving=dt.date(2026, 11, 26), christmas=dt.date(2026, 12, 25), birthday=dt.date(2026, 8, 7))
fixture = out / "holiday_fixture.json"
try:
    for name, day in holidays.items():
        for night in (False, True):
            f = copy.deepcopy(base)
            stamp = int(dt.datetime.combine(day, dt.time(23 if night else 12, 30), dt.timezone(dt.timedelta(hours=8))).timestamp())
            f["_sim"]["now"] = stamp
            f["generated_at"] = f["github"]["fetched_at"] = stamp-10
            for server in f["servers"]:
                server["reported_at"] = stamp-5
                server["online"] = True
            f["events"] = []
            fixture.write_text(json.dumps(f, ensure_ascii=False))
            path = out / f"holiday_{name}_{'night' if night else 'day'}.bmp"
            args = [sim, "--shot", str(path), "--fixture", str(fixture), "--act", "0" if night else "8", "--ms", "700"]
            if night:
                args += ["--night"]
            subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
            subprocess.run(["sips", "-s", "format", "png", str(path), "--out", str(out / ("s_"+path.stem+".png"))], check=True, stdout=subprocess.DEVNULL)
            path.unlink()
finally:
    fixture.unlink(missing_ok=True)

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit(0)
for mode in ("day", "night"):
    sheet = Image.new("RGB", (1000, 1046), (35, 35, 35))
    draw = ImageDraw.Draw(sheet)
    for i, path in enumerate(sorted(out.glob(f"s_holiday_*_{mode}.png"))):
        im = Image.open(path).resize((240, 320), Image.Resampling.NEAREST)
        x, y = 8+(i%4)*248, 8+(i//4)*346
        draw.text((x, y), path.stem[10:], fill=(230, 230, 230))
        sheet.paste(im, (x, y+18))
    sheet.save(out/f"sheet_holidays_{mode}.png")
