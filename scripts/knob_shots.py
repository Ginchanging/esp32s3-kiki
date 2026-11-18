#!/usr/bin/env python3
"""Render the selected A pet animation through the actual knob and shared LVGL UI."""
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw
import cat_art as art

root = Path(__file__).resolve().parents[1]
sim, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
gesture = "turn:1;wait:100;turn:-1;wait:100;turn:1;wait:100;turn:-1"
frames = []
with tempfile.TemporaryDirectory(prefix="kiki-pet-shots-") as temp:
    target = Path(temp)/"pet.bmp"
    for ms in range(0, 4401, 100):
        subprocess.run([str(sim), "--shot", str(target), "--fixture", str(root/"fixtures/1_ok.json"),
                        "--scale", "1", "--act", "8", "--knob", gesture, "--ms", str(ms)],
                       check=True, stdout=subprocess.DEVNULL)
        frames.append(Image.open(target).convert("RGB"))
frames[0].save(out/"kiki_belly_a_board.gif", save_all=True, append_images=frames[1:],
               duration=100, loop=0, disposal=2)
story = Image.new("RGB", (4*248, 2*348), (30, 27, 35))
story_draw = ImageDraw.Draw(story)
for n, i in enumerate((0, 3, 6, 10, 15, 22, 32, 39)):
    x, y = n % 4 * 248, n // 4 * 348
    story_draw.text((x+5, y+4), f"frame {i}", fill=(242, 233, 220))
    story.paste(frames[i], (x, y+22))
story.save(out/"kiki_belly_a_storyboard.png")
# Kiki's image is at (108, 44) within the tile, below the 24px top bar.
cats = [f.crop((108, 68, 236, 196)) for f in frames]
cats[0].save(out/"kiki_belly_a.gif", save_all=True, append_images=cats[1:],
             duration=100, loop=0, disposal=2)
looks = ["lean", "side", "belly", "rub", "rest"]
sheet = Image.new("RGB", (5*156, 202), (36, 33, 47))
draw = ImageDraw.Draw(sheet)
for i, name in enumerate(looks):
    L = art.body_pet(name)
    pose = Image.new("RGB", (32, 32), (36, 33, 47))
    for y, row in enumerate(L.g):
        for x, ch in enumerate(row):
            if ch != ".": pose.putpixel((x, y), tuple(bytes.fromhex(art.PALETTE[ch])))
    sheet.paste(pose.resize((128, 128), Image.Resampling.NEAREST), (i*156+14, 0))
    sheet.paste(pose, (i*156+62, 152))
    draw.text((i*156+14, 130), name, fill=(242, 233, 220))
sheet.save(out/"kiki_belly_a_pixels.png")
print("pet A: native pixel sheet and real knob animation GIFs generated")
