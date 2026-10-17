#!/usr/bin/env python3
"""Exercise native quick-save and quick-load commands using player-owned files."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data", type=Path)
    parser.add_argument("patch", type=Path)
    args = parser.parse_args()
    # Reuse the existing title/story fixture; replace its normal save/load tail.
    setup = (ROOT / "engine/tests/inputs/chs_save.input").read_text().splitlines()
    setup = [line for line in setup if int(line.split()[0]) < 24000]
    setup += ["24200 command 0", "24600 click 400 500", "24900 click 400 500",
              "25300 command 1", "25800 click 350 340"]
    with tempfile.TemporaryDirectory(prefix="ore-quick-flow-") as temporary:
        out = Path(temporary)
        input_file = out / "quick.input"
        input_file.write_text("\n".join(setup) + "\n")
        command = [str(ROOT / "build/host/orehost"), "--data", str(args.data.resolve()),
                   "--patch", str(args.patch.resolve()), "--chs", "--save", str(out / "saves"),
                   "--frames", "27000", "--input", str(input_file), "--trace", "2a,2b,2c", "--out", str(out)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        log = result.stdout + result.stderr
        if result.returncode or any(s in log for s in ("engine stopped", "unimplemented native", "script error")):
            raise RuntimeError(log)
        expected = [r"N_2a\(11, -1,.*\) = 0", r"N_2c\(\d+, 11, -1, 1\) = 0",
                    r"N_2b\(11, -1,.*\) = 0"]
        if not all(re.search(pattern, log) for pattern in expected):
            raise RuntimeError("Quick save, confirmation or load did not succeed:\n" + log)
        if "ran 27000 frames, script S01_01A" not in log:
            raise RuntimeError("Story did not resume after quick load:\n" + log)
        if not (out / "saves/DATA1105.SAV").is_file():
            raise RuntimeError("Quick save file was not created")
        print("Native quick save/confirm/load flow passed (27000 frames)")


if __name__ == "__main__":
    main()
