"""Exercise the ninth page through real simulator pointer events and rendered frames."""
from pathlib import Path
import subprocess
import sys
import tempfile

sim, fixture = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="kiki-health-ui-") as tmp:
    def run(name, *args):
        path = Path(tmp)/(name+".bmp")
        result = subprocess.run([sim, "--shot", str(path), "--fixture", fixture,
                                 "--ms", "100", *args], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=True)
        data = path.read_bytes()
        assert data[:2] == b"BM" and len(data) > 10000
        return result.stdout, data

    _, move = run("move", "--page", "8")
    _, sleep = run("sleep", "--page", "8", "--swipe", "tap:120,69,1")
    _, body = run("body", "--page", "8", "--swipe", "tap:195,69,1")
    assert len({move, sleep, body}) == 3, "tab taps must change the rendered content"
    _, english = run("english", "--page", "8", "--lang", "en")
    assert english != move
    out, _ = run("wrap", "--page", "8", "--swipe", "200,225,40,225")
    assert "page 8 -> 0" in out, out
    out, _ = run("back", "--swipe", "40,225,200,225")
    assert "page 0 -> 8" in out, out
    out, _ = run("home", "--page", "8", "--swipe", "tap:120,40,2")
    assert "page 8 -> 0" in out, out
    _, rebuilt = run("rebuild", "--page", "8", "--swipe", "tap:195,69,1;lang;lang")
    assert rebuilt == body, "language rebuild must retain the selected health tab"
print("health tabs, bilingual render, language rebuild, paging and double-tap passed")
