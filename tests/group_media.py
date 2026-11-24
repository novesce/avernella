# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the APK contract and the offline conference media regression suite."""

import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "test-call-group": [
        "/group-call/video-answer",
        "/group-call/conference-members",
        "/group-call/channel-members",
    ],
    "test-call": [
        "/call/group-video",
        "/call/group-video-software-svc",
        "/call/group-video-legacy-svc",
        "/call/group-video-no-packet-version",
        "/call/group-video-native-uplink",
        "/call/group-camera-unavailable",
        "/call/group-video-voice-fallback",
        "/call/group-share/camera",
        "/call/group-share/no-camera",
        "/call/share-to-tablet/video",
        "/call/share-to-tablet/voice",
        "/call/share-from-tablet",
    ],
    "test-call-video": [
        "/video/conference-independent-stream-directions",
        "/video/conference-evs3-send-vfd",
        "/video/conference-evs3-send-ved",
        "/video/conference-vp8-compact-vfd",
        "/video/conference-vp8-compact-ved",
        "/video/conference-keyframe-feedback",
        "/video/screen-share-official-layout",
        "/video/screen-capture-conversion",
    ],
    "test-ui-call-fullscreen": [
        "/call/group-share-controls",
        "/call/fullscreen-peer",
    ],
}


def run_cases(binary, paths, env):
    args = [ROOT / "build" / binary, "--tap"]
    for path in paths:
        args.extend(("-p", path))
    run = subprocess.run(args, env=env, cwd=ROOT, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, timeout=180)
    # GLib can exit successfully when a filter matches zero cases or skips one.
    passed = re.findall(r"^ok [0-9]+ (/[^\s]+)$", run.stdout, re.MULTILINE)
    if run.returncode or sorted(passed) != sorted(paths):
        sys.stderr.write(run.stdout)
        missing = sorted(set(paths) - set(passed))
        raise SystemExit(f"{binary} failed (exit {run.returncode}, missing passes {missing})")
    print(f"{binary}: {len(passed)} cases passed", flush=True)


def main():
    apk = os.environ.get("LINE_APK")
    if not apk:
        raise SystemExit("Supply the local Android base APK with LINE_APK=/path/to/LINE.apk")
    subprocess.run([sys.executable, ROOT / "tests/group_media_apk.py", apk], check=True)
    env = {key: val for key, val in os.environ.items()
           if not key.startswith(("LINE_", "AVERNELLA_"))}
    env.update(LINE_DISABLE_GPU="1", SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    with TemporaryDirectory(prefix="avernella-group-media-") as tmp:
        for kind in ("CONFIG", "CACHE", "STATE", "DATA"):
            env[f"XDG_{kind}_HOME"] = str(Path(tmp) / kind.lower())
        for binary, paths in CASES.items():
            run_cases(binary, paths, env)
    print("Offline media checks passed. Android playback on a live call requires a handset test.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
