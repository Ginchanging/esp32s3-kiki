"""Exercise real LVGL pointer navigation against the shared simulator."""
import json
import pathlib
import subprocess
import sys
import tempfile

sim, fixture = sys.argv[1:]
now = json.loads(pathlib.Path(fixture).read_text())["_sim"]["now"]
today = int((now+28800)//86400)
with tempfile.TemporaryDirectory(prefix="kiki-calendar-ui-") as temp:
    frame = pathlib.Path(temp) / "frame.bmp"

    def run(*args):
        result = subprocess.run([sim, "--shot", str(frame),
                                 "--fixture", fixture, "--ms", "100", *args],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        return result.stdout

    out = run("--swipe", "tap:89,147,1")
    assert "page 0 -> 3  tool none -> calendar" in out, out
    out = run("--tool", "calendar", "--swipe", "tap:200,291,1;tap:40,291,1")
    assert f"day={today} offset=8 total=13" in out, out
    assert out.count(f"day={today} offset=0 total=13") >= 2, out
    out = run("--tool", "calendar", "--swipe", "tap:56,75,1;tap:216,75,1")
    assert f"day={today+1} offset=0 total=2" in out, out
    assert f"day={today+6} offset=0 total=0" in out, out
    run("--tool", "calendar")
    initial = frame.read_bytes()
    out = run("--tool", "calendar", "--swipe", "80,238,80,147")
    assert "page 3 -> 3  tool calendar -> calendar" in out and frame.read_bytes() != initial, out
    out = run("--tool", "calendar", "--swipe", "200,225,40,225;tap:26,37,1;200,225,40,225")
    assert "page 3 -> 3  tool calendar -> calendar" in out, out
    assert "page 3 -> 3  tool calendar -> none" in out and "page 3 -> 4  tool none -> none" in out, out
    out = run("--swipe", "40,225,200,225")
    assert "page 0 -> 6" in out, out
    out = run("--tool", "calendar", "--swipe", "tap:120,40,2")
    assert "page 3 -> 0  tool calendar -> none" in out, out
    out = run("--night", "--swipe", "tap:89,147,1;tap:89,147,1")
    assert "page 0 -> 0" in out and "page 0 -> 3  tool none -> calendar" in out, out
    out = run("--tool", "calendar", "--swipe", "tap:56,75,1;lang;wait:50")
    assert f"day={today+1} offset=0 total=2" in out and "page=3 tool=calendar" in out, out
print("wall entry, date selection, paging, wraparound, double tap, night wake and language rebuild passed")
