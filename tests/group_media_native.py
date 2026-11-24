# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the 26.16.0 Android arm64 media contract and check synthetic sender headers."""

import os
import re
import struct
import subprocess


DECODER = "evs3_codec_header_decode"
WRITER = "_ZN9andromeda20PlanetVideoInterface10writeVideoEPNS_16VideoMediaStream14VideoMediaDataE"
PACKETIZER = "vns_evs_frag_pktize"
RATES = {
    "kEvs3SvcEncLyrTargetBrMaxKbps": (150, 300, 800, 3000),
    "kEvs3SvcEncLyrFramerateMaxFps": (15, 30, 30, 30),
}


def read_native(data):
    # Inspect only these exports in the pinned Android arm64 split. The library
    # stays in memory and is never loaded as executable code.
    args = ["llvm-objdump", "-T", "-d", "--no-show-raw-insn",
            "--disassemble-symbols=" + ",".join((DECODER, WRITER, PACKETIZER)), "-"]
    try:
        result = subprocess.run(args, input=data, capture_output=True, timeout=60,
                                env={**os.environ, "LC_ALL": "C"})
    except FileNotFoundError as error:
        raise ValueError("The ARM64 APKM check requires llvm-objdump from LLVM.") from error
    if result.returncode:
        raise ValueError("llvm-objdump could not inspect the media library: "
                         + result.stderr.decode(errors="replace").strip())
    listing = result.stdout.decode()
    if "file format elf64-littleaarch64" not in listing:
        raise ValueError("expected the ARM64 ELF media library")
    # Symbol values are virtual addresses. Map each read-only object through a
    # file-backed PT_LOAD segment so section padding does not affect offsets.
    phoff, = struct.unpack_from("<Q", data, 32)
    stride, count = struct.unpack_from("<HH", data, 54)
    for name, expected in RATES.items():
        symbol = re.search(r"^([0-9a-f]+)\s+g\s+DO\s+\.rodata\s+0*10\s+"
                           + name + "$", listing, re.MULTILINE)
        if not symbol:
            raise ValueError(f"missing native layer table {name}")
        address = int(symbol[1], 16)
        for i in range(count):
            kind, _, start, base, _, size = struct.unpack_from("<II4Q", data, phoff + i * stride)
            if kind == 1 and base <= address and address + 16 <= base + size:
                values = struct.unpack_from("<4I", data, start + address - base)
                if values != expected:
                    raise ValueError(f"unexpected {name}: {values}")
                break
        else:
            raise ValueError(f"{name} is outside file-backed segments")

    functions = {}
    code = None
    for line in listing.splitlines():
        entry = re.fullmatch(r"([0-9a-f]+) <([^>]+)>:", line)
        if entry:
            base = int(entry[1], 16)
            code = functions.setdefault(entry[2], {})
        elif code is not None:
            line = line.partition("//")[0].strip()
            instruction = re.fullmatch(r"([0-9a-f]+):\s+(.+)", line)
            if instruction:
                asm = " ".join(instruction[2].split())
                asm = re.sub(r"#(-?0x[a-f0-9]+)", lambda m: "#" + str(int(m[1], 16)), asm)
                code[int(instruction[1], 16) - base] = asm

    def operand(function, offset, pattern):
        asm = functions.get(function, {}).get(offset, "")
        match = re.fullmatch(pattern, asm)
        if not match:
            raise ValueError(f"unexpected native instruction at {function}+{offset:#x}: {asm}")
        if match.lastindex:
            return int(match[1])

    # PlanetVideoInterface supplies generation and source as a four-byte tag/value record.
    gen = operand(WRITER, 0x1c, r"mov w8, #(\d+)")
    src = operand(WRITER, 0x58, r"mov w8, #(\d+)")
    operand(WRITER, 0x54, r"sturb w8, \[x29, #-12\]")
    operand(WRITER, 0x68, r"sturb w8, \[x29, #-10\]")
    operand(WRITER, 0x88, r"sturb w11, \[x29, #-11\]")
    operand(WRITER, 0x8c, r"sturb w10, \[x29, #-9\]")
    operand(PACKETIZER, 0x3c4, r"bl \S+ <evs3_pd_ext_make_user_data_value@plt>")
    operand(PACKETIZER, 0x444, r"bl \S+ <evs3_pd_ext_make_enc_delay_value@plt>")

    # Resolve how Android's decoder assembles the compressed picture length.
    # The second format reverses a 32-bit load on little-endian Android.
    mask = operand(DECODER, 0x74, r"and w8, w8, #(\d+)")
    mid = operand(DECODER, 0x78, r"orr w8, w8, w9, lsl #(\d+)")
    high = operand(DECODER, 0x7c, r"orr w8, w8, w10, lsl #(\d+)")
    bias = operand(DECODER, 0x80, r"sub w8, w8, #(\d+)")
    operand(DECODER, 0x88, r"mov w0, #3")
    operand(DECODER, 0x98, r"ldr w8, \[x2\]")
    operand(DECODER, 0x9c, r"rev w8, w8")
    operand(DECODER, 0xa4, r"mov w0, #4")
    if (gen, src, mask, mid, high, bias) != (1, 2, 3, 2, 10, 3):
        raise ValueError("unexpected Android source tags or compact VP8 length fields")
    return gen, src, mask, mid, high, bias


def check_native(data, video_test=None):
    gen, src, mask, mid, high, bias = read_native(data)
    print("ARM64 ELF: capture-source tags, compact/wide EVS3 lengths and four camera rate classes")
    if video_test is None:
        return
    env = {k: v for k, v in os.environ.items() if not k.startswith(("LINE_", "AVERNELLA_"))}
    env.update(LINE_DISABLE_GPU="1", AVERNELLA_TEST_MEDIA_CONTRACT="1")
    pictures = ((1, 0), (1, 0), (2, 1), (2, 1), (2, 1), (3, 0))
    profiles = (
        ("/video/conference-evs3-send", False),
        ("/video/conference-vp8-compact-vfd", False),
        ("/video/conference-vp8-compact-ved", False),
        ("/video/conference-evs3-send-vfd", True),
        ("/video/conference-evs3-send-ved", True),
    )
    for path, wide in profiles:
        proc = subprocess.run([video_test, "--tap", "-p", path], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        records = re.findall(r"^# group-media (\d+) (\d+) ([0-9a-f]{8}) ([0-9a-f]{8})$",
                             proc.stdout, re.MULTILINE)
        if (proc.returncode or len(records) != len(pictures) or
                not re.search(r"^ok \d+ " + re.escape(path) + "$", proc.stdout, re.MULTILINE)):
            raise ValueError(f"sender fixture failed or incomplete: {path}\n{proc.stdout}")
        for index, (generation, source) in enumerate(pictures):
            number, total, capture, encoded = records[index]
            header = bytes.fromhex(encoded)
            if wide:
                length = int.from_bytes(header, "big")
            else:
                length = ((header[0] & mask) | (header[1] << mid) | (header[2] << high)) - bias
            if (int(number) != index or bytes.fromhex(capture) != bytes((gen, generation, src, source))
                    or length != int(total) or length <= 0):
                raise ValueError(f"{path} picture {index} disagrees with Android's media layout")
    print(f"{len(profiles) * len(pictures)} synthetic Avernella camera/screen/camera pictures "
          "matched Android's header layout")
