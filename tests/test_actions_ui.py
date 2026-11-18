"""Exercise the simulator's real LVGL pointer and knob paths."""
import pathlib
import subprocess
import sys
import tempfile

sim, fixture = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="kiki-actions-ui-") as temp:
    def run(*args):
        result = subprocess.run([sim, "--shot", str(pathlib.Path(temp) / "frame.bmp"),
                                 "--fixture", fixture, "--page", "1", "--ms", "100", *args],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        return result.stdout

    out = run("--swipe", "120,286,120,232;tap:180,290,1")
    assert "runs=5" in out and "action query run=6 attempt=1" in out, out
    out = run("--swipe", "205,244,40,244")
    assert "page 1 -> 2" in out and "action query" not in out, out
    out = run("--swipe", "tap:200,99,1;tap:182,148,1;tap:210,302,1;tap:27,22,1;tap:27,22,1")
    assert out.count("run=4 attempt=2 jobs=1 job=0 steps=0 rows=3 total=3") == 2, out
    assert "job=402 steps=1 rows=6 total=8" in out and "job=402 steps=2 rows=2 total=8" in out, out
    assert "tap x1 at 27,22  page 1 -> 1" in out, out
    out = run("--knob", "turn:4;click;wait:80;click;wait:80;click;wait:80;turn:-9;click;hold:1")
    assert out.count("run=6 attempt=1 jobs=1 job=0 steps=0 rows=1") == 2, out
    assert "job=601 steps=1 rows=2" in out, out
