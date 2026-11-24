#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guided receiver checks for a group call placed by the participants."""

import argparse
from getpass import getpass
import secrets
import sys


PHONES = ("Android A", "Android B")
NATIVE = "Android A native group presentation on Linux"
CLEAR = "\033[2J\033[H"


def observations(native):
    rows = {}
    for phone in PHONES:
        rows[f"Laptop camera on {phone}"] = "unobserved"
        rows[f"{phone} camera focused on Linux"] = "unobserved"
        for number in (1, 2):
            rows[f"Screen code {number} on {phone}"] = "unobserved"
        rows[f"Laptop camera restored on {phone}"] = "unobserved"
    rows[NATIVE] = "unobserved" if native else "not requested"
    return rows


def ask(result, key, question):
    while True:
        response = input(f"{question} [y/n/s to skip] ").strip().lower()
        if response in {"y", "n", "s"}:
            result[key] = {"y": "verified", "n": "failed", "s": "skipped"}[response]
            return


def read_screen_code(result, number, code):
    # Keep the sender's code off the terminal during the readback.
    print(CLEAR, end="")
    print(f"SHARED SCREEN   {number}/2\n\n          {code}\n")
    input("When both viewers have checked their screens, press Enter to hide the code. ")
    print(CLEAR, end="")
    for phone in PHONES:
        while True:
            reply = getpass(
                f"Enter the six digits read by {phone}, n for a blank or stale "
                "screen, or s to skip: "
            ).strip().lower()
            if reply in {"s", "n"} or (
                len(reply) == 6 and reply.isascii() and reply.isdigit()
            ):
                break
        if reply == "s":
            outcome = "skipped"
        else:
            outcome = "verified" if reply == code else "failed"
        result[f"Screen code {number} on {phone}"] = outcome


def run(result, native):
    print("Use Avernella and two participants running the official Android app. "
          "Call them Android A and Android B. Start a group video call yourselves "
          "and turn on all three cameras.")
    print("Answer from each receiving screen. Use s for anything you cannot observe.")
    input("Once all three have joined, press Enter. ")
    print("\nOn the laptop, wave and then cover and uncover its camera.")
    for phone in PHONES:
        ask(result, f"Laptop camera on {phone}",
            f"Did {phone} see that sequence in the laptop's tile?")
    for phone, gesture in zip(PHONES, ("wave", "hold up two fingers, then lower them")):
        ask(result, f"{phone} camera focused on Linux",
            f"Focus {phone} in Avernella and ask that person to {gesture}. "
            "Did the focused tile show that person doing it?")

    print("\nStart sharing in Avernella. In the portal, choose this terminal window "
          "or its display. Keep the terminal visible. Each Android viewer should "
          "note each code privately from the laptop's tile, then give you their "
          "own reading after the code is hidden.")
    while True:
        reply = input("Press Enter when sharing, or s to skip the two codes. ").strip().lower()
        if reply in {"", "s"}:
            break
    if reply == "s":
        for number in (1, 2):
            for phone in PHONES:
                result[f"Screen code {number} on {phone}"] = "skipped"
    else:
        codes = secrets.SystemRandom().sample(range(1_000_000), 2)
        for number, value in enumerate(codes, 1):
            read_screen_code(result, number, f"{value:06d}")

    print("\nStop sharing in Avernella. Hold three fingers up to the laptop camera, "
          "then lower them.")
    for phone in PHONES:
        ask(result, f"Laptop camera restored on {phone}",
            f"Did {phone} see the shared screen change back to the moving camera?")
    if native:
        print("\nOn Android A, start the official app's screen share and scroll "
              "a test page up and down. Look for a separate shared-screen tile in "
              "Avernella, then stop sharing on Android A.")
        ask(result, NATIVE, "Did the separate tile show that movement and close on stop?")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-presentation", action="store_true",
                        help="also require Android A's separate screen-share tile on Linux")
    args = parser.parse_args()
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        print("Run this check in an interactive terminal with the participants present.",
              file=sys.stderr)
        return 2
    result = observations(args.native_presentation)
    canceled = False
    try:
        run(result, args.native_presentation)
    except (EOFError, KeyboardInterrupt):
        print("\nCheck interrupted.")
        canceled = True
    print("\nLive group media results")
    for check, outcome in result.items():
        print(f"  {outcome:13} {check}")
    if canceled:
        return 2
    return int(any(status not in {"verified", "not requested"}
                   for status in result.values()))


if __name__ == "__main__":
    sys.exit(main())
