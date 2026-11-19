"""Exercise the health tool through real simulator pointer events and rendered frames."""
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

    _, move = run("move", "--tool", "health")
    _, sleep = run("sleep", "--tool", "health", "--swipe", "tap:120,69,1")
    _, body = run("body", "--tool", "health", "--swipe", "tap:195,69,1")
    assert len({move, sleep, body}) == 3, "tab taps must change the rendered content"
    _, english = run("english", "--tool", "health", "--lang", "en")
    assert english != move
    _, legacy = run("legacy", "--page", "8")
    assert legacy == move
    out, _ = run("wrap", "--page", "6", "--swipe", "200,225,40,225")
    assert "page 6 -> 0" in out, out
    out, _ = run("back", "--swipe", "40,225,200,225")
    assert "page 0 -> 6" in out, out
    out, _ = run("home", "--tool", "health", "--swipe", "tap:120,40,2")
    assert "page 3 -> 0  tool health -> none" in out, out
    _, rebuilt = run("rebuild", "--tool", "health", "--swipe", "tap:195,69,1;lang;lang")
    assert rebuilt == body, "language rebuild must retain the selected health tab"
print("health tabs, bilingual render, language rebuild, paging and double-tap passed")
