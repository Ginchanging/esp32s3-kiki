"""Exercise the toolbox, live radar view and reminder on the board's shared UI."""

import json
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory


def main():
    sim, fixture = sys.argv[1:]
    with TemporaryDirectory() as tmp:
        bmp = Path(tmp) / "frame.bmp"
        snapshot = Path(tmp) / "sit.json"
        base = json.loads(Path(fixture).read_text())

        def run(*args, sit=None):
            source = fixture
            if sit is not None:
                base.setdefault("_sim", {})["sit"] = sit
                snapshot.write_text(json.dumps(base))
                source = str(snapshot)
            result = subprocess.run([sim, "--shot", str(bmp), "--scale", "1", "--fixture", source, *args],
                                    check=True, capture_output=True, text=True)
            return bmp.read_bytes(), result.stdout

        def frame(*args, sit=None):
            return run(*args, sit=sit)[0]

        for x, y, tool in ((62, 107, "pomodoro"), (176, 107, "calendar"), (62, 226, "sit"), (176, 226, "health")):
            _, out = run("--page", "3", "--swipe", f"tap:{x},{y},1;tap:26,37,1")
            assert f"page 3 -> 3  tool none -> {tool}" in out, out
            assert f"page 3 -> 3  tool {tool} -> none" in out, out

        live = {"link": "live", "phase": "present", "distance_cm": 86, "elapsed_s": 1694}
        reference = frame("--tool", "sit", sit=live)
        reopened, _ = run("--tool", "sit", "--swipe",
                          "tap:26,37,1;tap:62,226,1;tap:214,12,1;tap:214,12,1", sit=live)
        assert reopened == reference, "reopening and switching the live tool must retain the snapshot"
        _, out = run("--tool", "sit", "--swipe", "tap:120,38,2", sit=live)
        assert "page 3 -> 0  tool sit -> none" in out, out

        cases = ({"link": "waiting"}, {"link": "live"}, live,
                 {**live, "phase": "away", "distance_cm": 0},
                 {**live, "elapsed_s": 3720, "reminded": True}, {**live, "link": "stale"},
                 {**live, "enabled": False}, {"link": "error"})

        def lit(bmp):
            # Sample the centers of the tracks in the RGB565-expanded 240 × 320 BMP.
            row = 54 + (319 - 201) * 720
            return sum(bmp[row + (22 + i * 37) * 3 + 2] > 200 for i in range(6))

        for lang in ("zh", "en"):
            options = ("--tool", "sit", "--lang", lang)
            rendered = [frame(*options, sit=case) for case in cases]
            assert len(set(rendered)) == len(cases)
            assert rendered[5] == frame(*options, sit={"link": "stale"}), "stale state must hide cached readings"
            assert [lit(rendered[i]) for i in (0, 2, 3, 4, 5, 6)] == [0, 3, 3, 6, 0, 0]

        for lang in ("zh", "en"):
            for page in ("0", "3"):
                options = ("--page", page, "--lang", lang)
                assert frame(*options, "--sit-demo", "--ms", "850") != frame(*options, "--ms", "850")

        card = ("--page", "3", "--lang", "en")
        assert frame(*card, "--sit-demo", "--ms", "8000") == frame(*card, "--ms", "8000")
        for event in ("muse", "lang"):
            after = ("--swipe", f"wait:350;{event}")
            assert frame(*card, "--sit-demo", *after) == frame(*card, *after)

        for alert in (("--flyover",), ("--ota", "countdown:3")):
            assert frame("--sit-demo", *alert, "--ms", "950") == frame(*alert, "--ms", "950")


if __name__ == "__main__":
    main()
