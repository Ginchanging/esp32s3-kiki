#!/usr/bin/env python3
"""Render OTA states in both languages; no networking or hardware writes."""
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "shots"
OUT.mkdir(exist_ok=True)
states = ("downloading", "verifying", "countdown:3", "countdown:2", "countdown:1",
          "validating", "success", "failed", "rollback")
sheet = Image.new("RGB", (5 * 240, 4 * 342), "#17151f")
draw = ImageDraw.Draw(sheet)
for row, lang in enumerate(("zh", "en")):
    for i, state in enumerate(states):
        stem = f"ota_{lang}_{state.replace(':', '_')}"
        bmp, png = OUT / (stem + ".bmp"), OUT / (stem + ".png")
        subprocess.run([str(ROOT / "build-sim/dash_sim"), "--fixture", str(ROOT / "fixtures/1_ok.json"),
                        "--shot", str(bmp), "--scale", "1", "--ota", state, "--lang", lang, "--page", "7", "--ms", "300"],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        im = Image.open(bmp).convert("RGB")
        im.save(png)
        x, y = i % 5 * 240, (row * 2 + i // 5) * 342
        sheet.paste(im, (x, y + 22))
        draw.text((x + 8, y + 5), f"{lang} {state}", fill="#f2e9dc")
sheet.save(OUT / "sheet_ota.png")
print(OUT / "sheet_ota.png")
