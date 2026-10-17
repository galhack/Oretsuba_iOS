#!/usr/bin/env python3
"""Exercise opening and closing history using player-owned Chinese game files."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data", type=Path)
    parser.add_argument("patch", type=Path)
    args = parser.parse_args()
    # Reuse the existing title/story fixture; replace its save/load tail.
    setup = (ROOT / "engine/tests/inputs/chs_save.input").read_text().splitlines()
    setup = [line for line in setup if int(line.split()[0]) < 24000]
    setup += ["24200 command 7", "25000 command 7"]
    with tempfile.TemporaryDirectory(prefix="ore-history-flow-") as temporary:
        out = Path(temporary)
        input_file = out / "history.input"
        input_file.write_text("\n".join(setup) + "\n")
        command = [str(ROOT / "build/host/orehost"), "--data", str(args.data.resolve()),
                   "--patch", str(args.patch.resolve()), "--chs", "--save", str(out / "saves"),
                   "--frames", "26000", "--input", str(input_file), "--out", str(out)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        log = result.stdout + result.stderr
        if result.returncode or any(s in log for s in ("engine stopped", "unimplemented native", "script error")):
            raise RuntimeError(log)
        expected = ["frame 24200: command 7 in script S01_01A",
                    "frame 25000: command 7 in script HISTORY"]
        if not all(message in log for message in expected):
            raise RuntimeError("History did not open from the story:\n" + log)
        if "ran 26000 frames, script S01_01A" not in log:
            raise RuntimeError("Story did not resume after closing history:\n" + log)
        print("History command open/close flow passed (26000 frames)")


if __name__ == "__main__":
    main()
