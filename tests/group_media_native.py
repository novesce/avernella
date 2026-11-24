# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the 26.16.0 Android arm64 media contract and check synthetic sender headers."""

import os
import re
import struct
import subprocess


DECODER = "evs3_codec_header_decode"
WRITER = "_ZN9andromeda20PlanetVideoInterface10writeVideoEPNS_16VideoMediaStream14VideoMediaDataE"
PACKETIZER = "vns_evs_frag_pktize"
PRESENTATION = "jup_conf_request_presentation"
RESUME = "vns_pdtp_sess_request_resume"
# llvm-objdump labels the private STRM_CTRL serializer by the preceding export.
CONTROL = "vns_pdtp_pkt_ack_reason_get_name"
RATES = {
    "kEvs3SvcEncLyrTargetBrMaxKbps": (150, 300, 800, 3000),
    "kEvs3SvcEncLyrFramerateMaxFps": (15, 30, 30, 30),
}


def read_native(data):
    # Inspect only these exports in the pinned Android arm64 split. The library
    # stays in memory. This static path also runs without an emulator.
    exports = (DECODER, WRITER, PACKETIZER, PRESENTATION, RESUME, CONTROL)
    args = ["llvm-objdump", "-T", "-d", "--no-show-raw-insn",
            "--disassemble-symbols=" + ",".join(exports), "-"]
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
    # Room presentation selection uses a separate PDTP receive stream and
    # requests the presenter's source when LINE makes data available.
    operand(PRESENTATION, 0xe8, r"bl \S+ <jup_conf_room_info_get_member@plt>")
    operand(PRESENTATION, 0x13c, r"mov w1, #4")
    operand(PRESENTATION, 0x140, r"mov w2, #1")
    operand(PRESENTATION, 0x144, r"bl \S+ <jup_media_arr_find_media@plt>")
    stream = operand(PRESENTATION, 0x58c, r"mov w1, #(\d+)")
    operand(PRESENTATION, 0x594, r"bl \S+ <jup_pdtp_trans_get_recv_sess@plt>")
    operand(PRESENTATION, 0x5a8, r"bl \S+ <jup_pdtp_sess_resume@plt>")
    resume = operand(RESUME, 0x6d0, r"mov w5, #(\d+)")
    operand(RESUME, 0x6d4, r"bl \S+ <vns_pdtp_pkt_strmctrl_create@plt>")
    operand(RESUME, 0x6e8, r"bl \S+ <vns_pdtp_pkt_strmctrl_set_req_resume@plt>")
    # Both DATA_AVAIL and targeted REQ_RESUME carry source and source offset
    # as trailing varints. The source is a uint32, the offset is a uint64.
    for start, opcode in ((0x1e50, resume), (0x1ec4, 12)):
        operand(CONTROL, start, rf"cmp w8, #{opcode}")
        operand(CONTROL, start + 8, r"ldr w1, \[x20, #1048\]")
        operand(CONTROL, start + 16, r"bl \S+ <vns_pdtp_writer_varint62@plt>")
        operand(CONTROL, start + 24, r"ldr x1, \[x20, #1056\]")
        operand(CONTROL, start + 32, r"bl \S+ <vns_pdtp_writer_varint62@plt>")
    return gen, src, mask, mid, high, bias, stream, resume


def check_native(data, video_test=None, execute_codec=False):
    gen, src, mask, mid, high, bias, stream, resume = read_native(data)
    print("ARM64 ELF: capture-source tags, compact/wide EVS3 lengths and four camera rate classes")
    print(f"ARM64 ELF: room presentation stream {stream} and source-targeted resume {resume}")
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

    # Check the control actually returned to the relay by the Linux worker.
    call_test = video_test.with_name("test-call")
    path = "/call/group-share/native-presentation"
    run = subprocess.run([call_test, "--tap", "-p", path], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, timeout=60)
    records = re.findall(r"^[ \t]*(?:#[ \t]*)+group-resume ([0-9a-f]+) (\d+) (\d+)$", run.stdout, re.MULTILINE)
    if (run.returncode or [(int(source), int(offset)) for _, source, offset in records] !=
            [(801, 1234), (803, 0)] or
            not re.search(r"^ok \d+ " + re.escape(path) + "$", run.stdout, re.MULTILINE)):
        raise ValueError(f"native presentation receiver did not complete: {path}\n{run.stdout}")
    for wire, source, offset in records:
        expected = (varint(stream) + b"\x02" + varint(0) + varint(resume) +
                    varint(int(source)) + varint(int(offset)))
        if bytes.fromhex(wire) != expected:
            raise ValueError("Linux receiver supplied a different resume control from Android")
    print("Linux group receiver sent Android's targeted presentation resume layout")
    if execute_codec:
        from group_media_control import replay
        replay(data, records)


def varint(value):
    """Encode a minimal PDTP varint for the independent wire comparison."""
    for width, prefix in ((1, 0), (2, 1), (4, 2), (8, 3)):
        bits = width * 8 - 2
        if 0 <= value < 1 << bits:
            return ((prefix << bits) | value).to_bytes(width, "big")
    raise ValueError("PDTP varint exceeds 62 bits")
