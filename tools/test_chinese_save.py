#!/usr/bin/env python3
"""Exercise title -> Chinese story -> save -> load using player-owned files."""
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
    with tempfile.TemporaryDirectory(prefix="ore-chs-save-") as temporary:
        out = Path(temporary)
        command = [str(ROOT / "build/host/orehost"), "--data", str(args.data.resolve()),
                   "--patch", str(args.patch.resolve()), "--chs", "--save", str(out / "saves"),
                   "--frames", "30300", "--input", str(ROOT / "engine/tests/inputs/chs_save.input"),
                   "--out", str(out)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        log = result.stdout + result.stderr
        failures = ("engine stopped", "unimplemented native", "读取失败", "script error")
        if result.returncode or any(message in log for message in failures):
            raise RuntimeError(log)
        if "ran 30300 frames, script S01_01A" not in log:
            raise RuntimeError("Story did not resume after load:\n" + log)
        if not (out / "saves/DATA0000.SAV").is_file():
            raise RuntimeError("Save file was not created")
        print("Chinese save/load flow passed (30300 frames)")


if __name__ == "__main__":
    main()
