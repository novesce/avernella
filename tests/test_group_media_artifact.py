# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise both supported artifacts through the public group-media checker."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile


CHECKER = Path(__file__).with_name("group_media_apk.py").resolve()


def check(path, version=None, error=None):
    run = subprocess.run([sys.executable, CHECKER, path], capture_output=True,
                         text=True, timeout=90)
    passes = re.findall(r"^PASS LINE Android (\d+\.\d+\.\d+)\b", run.stdout,
                        re.MULTILINE)
    if version is None:
        expected = "a clean rejection"
        ok = (run.returncode > 0 and not passes and
              error in run.stderr and "Traceback " not in run.stderr)
    else:
        expected = f"a pass for {version}"
        ok = (run.returncode == 0 and passes == [version] and
              "per-user presentation callbacks" in run.stdout and
              ("ARM64 ELF:" in run.stdout) == (version == "26.16.0"))
    if not ok:
        raise SystemExit(f"{path.name}: expected {expected}, exit {run.returncode}\n"
                         f"{run.stdout}{run.stderr}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="official 26.16.0 APKM")
    parser.add_argument("base", type=Path, help="official 26.18.2 base APK")
    args = parser.parse_args()
    check(args.bundle, "26.16.0")
    check(args.base, "26.18.2")

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / args.bundle.name
        check(path, error="No such file")
        # Preserve every original member. ZIP readers accept this edit, but
        # the caller supplied a different artifact and it must not pass.
        shutil.copyfile(args.bundle, path)
        with path.open("ab") as copy:
            copy.write(b"x")
        check(path, error="unsupported APK or APKM SHA-256")
        path.unlink()

        # A genuine manifest and base can survive a broken split download.
        with zipfile.ZipFile(args.bundle) as original, zipfile.ZipFile(path, "w") as incomplete:
            incomplete.writestr("info.json", original.read("info.json"))
            with original.open("base.apk") as src, incomplete.open("base.apk", "w") as dst:
                shutil.copyfileobj(src, dst)
        check(path, error="unsupported APK or APKM SHA-256")
        with zipfile.ZipFile(path, "a") as wrong:
            wrong.writestr("split_config.arm64_v8a.apk", b"unrelated payload")
        check(path, error="unsupported APK or APKM SHA-256")
    print("Artifact CLI passed both version checks and all four rejection checks.")


if __name__ == "__main__":
    main()
