#!/usr/bin/env python3
"""Review free-time poses from the real selector, plus pixel/animation sheets."""
import copy
import json
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw
import cat_art as art

root = pathlib.Path(__file__).resolve().parents[1]
sim, out = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
base = json.loads((root/"fixtures/1_ok.json").read_text())
names = ["sit", "window", "lie", "stretch", "groom", "yawn", "yarn"]
samples = subprocess.check_output([str(sim.parent/"test_idle"), "--samples"], text=True)
timeline = subprocess.check_output([str(sim.parent/"test_idle"), "--timeline"], text=True)
(out/"idle_timeline.csv").write_text("time,kind,duration_s,base\n"+"\n".join(
    line for line in timeline.splitlines() if line[:8].count(":") == 2)+"\n")
with tempfile.TemporaryDirectory(prefix="kiki-idle-shots-") as temp:
    fixture = pathlib.Path(temp)/"fixture.json"
    for line in samples.splitlines():
        if not line.startswith("sample "):
            continue
        _, kind, stamp = line.split()
        kind, stamp = int(kind), int(stamp)
        for lang in ("zh", "en"):
            f = copy.deepcopy(base)
            f["_sim"]["now"] = stamp
            f["generated_at"] = f["github"]["fetched_at"] = stamp-5
            for server in f["servers"]:
                server["reported_at"] = stamp-5
                server["online"] = True
            f["events"] = []
            fixture.write_text(json.dumps(f, ensure_ascii=False))
            path = out/f"s_idle_{kind}_{names[kind]}_{lang}.png"
            bmp = pathlib.Path(temp)/"frame.bmp"
            subprocess.run([str(sim), "--shot", str(bmp), "--fixture", str(fixture),
                            "--lang", lang, "--ms", "150"], check=True, stdout=subprocess.DEVNULL)
            Image.open(bmp).save(path)

for lang in ("zh", "en"):
    sheet = Image.new("RGB", (1000, 700), (35, 35, 35))
    draw = ImageDraw.Draw(sheet)
    for kind, name in enumerate(names):
        frame = Image.open(out/f"s_idle_{kind}_{name}_{lang}.png").convert("RGB")
        x, y = 8+(kind%4)*248, 8+(kind//4)*346
        draw.text((x, y), name, fill=(230, 230, 230))
        sheet.paste(frame.resize((240, 320), Image.Resampling.NEAREST), (x, y+18))
    sheet.save(out/f"sheet_idle_{lang}.png")


def pixels(label, scale=4):
    _, *layers = next(look for look in art.LOOKS if look[0] == label)
    pose = art.compose(*(art.SPRITES[layer] for layer in layers))
    frame = Image.new("RGB", (32, 32), (36, 33, 47))
    for y, row in enumerate(pose.g):
        for x, ch in enumerate(row):
            if ch != ".":
                frame.putpixel((x, y), tuple(bytes.fromhex(art.PALETTE[ch])))
    return frame.resize((32*scale, 32*scale), Image.Resampling.NEAREST)


labels = ["wash 2", "wash", "window", "window blink", "rest", "rest 2",
          "rest blink", "stretch", "stretch 2", "drowsy", "yarn", "yarn 2"]
sheet = Image.new("RGB", (6*156, 2*196), (23, 21, 31))
draw = ImageDraw.Draw(sheet)
for i, label in enumerate(labels):
    x, y = (i%6)*156, (i//6)*196
    sheet.paste(pixels(label), (x+12, y+8))
    sheet.paste(pixels(label, 1), (x+12, y+146))
    draw.text((x+54, y+151), label, fill=(242, 233, 220))
sheet.save(out/"kiki_idle_pixels.png")

# Short cycles at the board's actual 4x scale, without interpolated pixels.
for name, poses, durations in [
    ("groom", ["wash 2", "wash", "wash 2", "wash", "wash 2"], [650, 250, 650, 250, 900]),
    ("stretch", ["stretch", "stretch 2", "stretch"], [1200, 1000, 800]),
    ("yarn", ["yarn", "yarn 2", "yarn"], [550, 650, 900]),
    ("window", ["window", "window tail", "window"], [16000, 850, 8000]),
    ("quiet", ["idle", "blink", "idle", "tail", "idle", "window", "window blink", "window", "rest", "rest 2", "rest blink"],
               [16000, 150, 4000, 850, 4000, 16000, 150, 6000, 8000, 1100, 150]),
]:
    frames = [pixels(label) for label in poses]
    frames[0].save(out/f"kiki_{name}.gif", save_all=True, append_images=frames[1:],
                   duration=durations, loop=0, disposal=2)
print("idle selector screenshots, timeline, 1x/4x pixel sheet and animation previews:", out)
