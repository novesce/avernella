#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check group media in a local LINE Android base APK or split APKM bundle."""

import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


APK_SHA256 = "5432c8ad902609b4ce4773d834206da63f5c441610346bec0249235116b2e4c5"
BASE = "Lcom/linecorp/andromeda/"
SESSION = BASE + "core/session/ServiceSession;"
JNI = BASE + "jni/SessionJNIImpl;"
EXT = BASE + "jni/SessionExtensionJNIImpl;"
STRING = "Ljava/lang/String;"
INFO = BASE + "VideoControl$StreamInfo;"
SOURCE = BASE + "video/p;"
PARTY = BASE + "VideoControl$Group$b$a;"
DIRECTION = BASE + "core/session/a$b;"
FRAME = BASE + "core/session/event/data/VideoFrameEventData;"

# Each offset is in 16-bit code units from the first instruction of the
# *declared* caller, as reported by baksmali --code-offsets on the pinned APK.
# Each test resolves the operand using that DEX file's field or method table.
IGET_OBJECT = 0x54
SGET_OBJECT = 0x62
VIRTUAL = 0x6e
DIRECT = 0x70
STATIC = 0x71
INTERFACE = 0x72
INTERFACE_RANGE = 0x78


class CheckError(Exception):
    pass


class Dex:
    """Read class_data and the references used by the selected instructions."""

    def __init__(self, data):
        self.data = data
        self.strings = self.u32(0x3c)
        self.types = self.u32(0x44)
        self.protos = self.u32(0x4c)
        self.fields = self.u32(0x54)
        self.methods = self.u32(0x5c)
        self.classes = {}
        count, start = struct.unpack_from("<II", data, 0x60)
        for offset in range(start, start + 32 * count, 32):
            owner = self.type_name(self.u32(offset))
            self.classes[owner] = self.u32(offset + 24)

    def u16(self, offset):
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from("<I", self.data, offset)[0]

    def uleb(self, offset):
        value = 0
        for shift in range(0, 35, 7):
            byte = self.data[offset]
            offset += 1
            value |= (byte & 0x7f) << shift
            if byte < 128:
                return value, offset
        raise CheckError("invalid DEX variable-length integer")

    def string(self, index):
        start = self.u32(self.strings + 4 * index)
        _, start = self.uleb(start)
        # All checked descriptors are ASCII. Unrelated DEX strings may be MUTF-8.
        return self.data[start:self.data.index(0, start)].decode("ascii", "backslashreplace")

    def type_name(self, index):
        return self.string(self.u32(self.types + 4 * index))

    def method_name(self, index):
        owner, proto, name = struct.unpack_from("<HHI", self.data, self.methods + 8 * index)
        _, result, params = struct.unpack_from("<III", self.data, self.protos + 12 * proto)
        args = ""
        if params:
            args = "".join(self.type_name(self.u16(params + 4 + 2 * i))
                           for i in range(self.u32(params)))
        return f"{self.type_name(owner)}->{self.string(name)}({args}){self.type_name(result)}"

    def field_name(self, index):
        owner, kind, name = struct.unpack_from("<HHI", self.data, self.fields + 8 * index)
        return f"{self.type_name(owner)}->{self.string(name)}:{self.type_name(kind)}"

    def declared(self, owner, signature):
        offset = self.classes.get(owner)
        if not offset:
            raise CheckError(f"no class data for {owner}")
        counts = []
        for _ in range(4):
            count, offset = self.uleb(offset)
            counts.append(count)
        for _ in range(2 * (counts[0] + counts[1])):
            _, offset = self.uleb(offset)
        for count in counts[2:]:
            index = 0
            for _ in range(count):
                delta, offset = self.uleb(offset)
                flags, offset = self.uleb(offset)
                code, offset = self.uleb(offset)
                index += delta
                if self.method_name(index) == owner + "->" + signature:
                    return flags, code
        raise CheckError(f"missing declaration {owner}->{signature}")

    def check(self, owner, signature, sites):
        _, code = self.declared(owner, signature)
        for offset, opcode, target in sites:
            if not code or offset + 2 > self.u32(code + 12):
                raise CheckError(f"missing instruction in {owner}->{signature} at {offset:#x}")
            pc = code + 16 + 2 * offset
            if self.data[pc] != opcode:
                raise CheckError(f"unexpected opcode in {owner}->{signature} at {offset:#x}")
            index = self.u16(pc + 2)
            if opcode in (IGET_OBJECT, SGET_OBJECT):
                reference = self.field_name(index)
            else:
                reference = self.method_name(index)
            if reference != target:
                raise CheckError(f"{owner}->{signature} at {offset:#x}: expected {target}, got {reference}")


def group_video(dex, command, requester, get_frame, update_info):
    cmd = command + "$a;"
    dispatch = SESSION + "->g(" + command + ";)Z"
    request = BASE + "core/session/ServiceSession$UserVideoRequest"
    request_constructor = (request + ";-><init>(" + STRING + STRING + request
                           + "$RequestType;" + BASE + "core/session/constant/VideoResolution;)V")
    dex.check(SESSION, "h(ZZ)V", [
        (0x02, SGET_OBJECT, cmd + "->SET_VIDEO_SEND_STATE:" + cmd),
        (0x14, VIRTUAL, dispatch),
    ])
    dex.check(BASE + "core/" + requester + ";", "k(Ljava/util/HashSet;)Z", [
        (0x2c, DIRECT, request_constructor),
        (0x65, SGET_OBJECT, cmd + "->REQUEST_PARTICIPANT_VIDEO:" + cmd),
        (0x92, VIRTUAL, dispatch),
    ])
    dex.check(JNI, get_frame + "(J" + STRING + ")Ljava/lang/Object;", [
        (0x00, STATIC, JNI + "->nVideoStreamGetUserFrame(J" + STRING + ")Ljava/lang/Object;"),
    ])
    dex.check(JNI, update_info + "(J" + INFO + ")V", [
        (0x00, STATIC, JNI + "->nVideoStreamUpdateLocalStreamInfo(JLjava/lang/Object;)V"),
    ])


def presentation_controls(dex, wrappers):
    controls = (
        ("(J)Z", 0, "nSessionExtensionIsPresentationSupported(J)Z"),
        ("(J)Z", 1, "nSessionExtensionStartPresentation(JZ)Z"),
        ("(IJ)Z", 0, "nSessionExtensionStopPresentation(JI)Z"),
        ("(J" + STRING + ")Z", 0, "nSessionExtensionPausePresentation(J" + STRING + ")Z"),
        ("(J" + STRING + ")Z", 0, "nSessionExtensionResumePresentation(J" + STRING + ")Z"),
    )
    for index, (signature, offset, function) in enumerate(controls):
        flags, code = dex.declared(EXT, function)
        if not flags & 0x100 or code:
            raise CheckError(f"expected native declaration for {function}")
        dex.check(EXT, wrappers[index] + signature, [(offset, STATIC, EXT + "->" + function)])


def group_screen_source(dex, manager, participant):
    group_change = BASE + "VideoControl$Group$StreamChangeEvent;"
    dex.check(manager + "$b$a;", "streamSourceEvent(" + group_change + ")V", [
        (0x09, SGET_OBJECT, PARTY + "->PARTICIPANT:" + PARTY),
        (0x0d, VIRTUAL, INFO + "->d()" + SOURCE),
        (0x11, SGET_OBJECT, SOURCE + "->VIRTUAL_DISPLAY:" + SOURCE),
        (0x22, VIRTUAL, manager + "$b;->d(" + STRING + ")" + participant),
    ])


def presentation_events(dex, manager):
    receive = (0x05, SGET_OBJECT, DIRECTION + "->RX:" + DIRECTION)
    for event, parameter, callback in (("Start", "e", "b"), ("Stop", "f", "a")):
        dex.check(manager + "$d;", "onPresentation" + event + "(" + BASE + "e$" + parameter + ";)V", [
            receive,
            (0x11, INTERFACE, manager + "$a;->" + callback + "(" + STRING + ")V"),
        ])
    dex.check(manager + "$d;", "onPresentationFrameEvent(" + BASE + "e$c;)V", [
        receive,
        (0x11, IGET_OBJECT, FRAME + "->userId:" + STRING),
        (0x19, INTERFACE_RANGE, manager + "$a;->c(IJI" + STRING + ")V"),
    ])


def check_managed(apk, version):
    # R8 moved the same contracts between DEX files and renamed their wrappers.
    if version == "26.16.0":
        core_file = events_file = "classes8.dex"
        source_file = "classes4.dex"
        camera = ("Lj40/d", "n", "C0", "G0")
        controls = ("K", "e0", "f0", "V", "Z")
        manager, participant = "Lbv7/c", "Luw8/e3;"
    elif version == "26.18.2":
        core_file = "classes4.dex"
        source_file = events_file = "classes12.dex"
        camera = ("Le40/d", "k", "S0", "W0")
        controls = ("o1", "Q1", "R1", "z1", "K1")
        manager, participant = "Luw7/b", "Liy8/c3;"
    else:
        raise CheckError(f"no managed media contract for LINE {version}")

    files = {name: Dex(apk.read(name)) for name in {core_file, source_file, events_file}}
    group_video(files[core_file], *camera)
    presentation_controls(files[core_file], controls)
    group_screen_source(files[source_file], manager, participant)
    presentation_events(files[events_file], manager)
    print(f"{core_file}: camera send state, participant video requests and per-user frames")
    print(f"{core_file}: presentation capability, start, pause, resume and stop JNI calls")
    print(f"{source_file}: group virtual-display selection")
    print(f"{events_file}: per-user presentation callbacks")


BUNDLE_SHA256 = "4a82c3173a935c52dbe96b8768b73a2dda7284b860f37e4982d72b5672dbf1af"


def check_bundle(bundle, video_test, execute_codec):
    from group_media_native import check_native

    # Inner APKs are deflated. Spool one at a time so seeking to DEX or ELF
    # members neither inflates the outer member repeatedly nor buffers it in RAM.
    for name in ("base.apk", "split_config.arm64_v8a.apk"):
        with tempfile.TemporaryFile() as tmp:
            with bundle.open(name) as member:
                shutil.copyfileobj(member, tmp, length=1024 * 1024)
            tmp.seek(0)
            with zipfile.ZipFile(tmp) as apk:
                if name == "base.apk":
                    check_managed(apk, "26.16.0")
                else:
                    check_native(apk.read("lib/arm64-v8a/libandromeda.so"), video_test, execute_codec)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", help="26.18.2 base APK or 26.16.0 APKM")
    parser.add_argument("--check-sender", action="store_true",
                        help="compare Linux sender packets when the native ARM64 split is present")
    parser.add_argument("--execute-presentation-codec", action="store_true",
                        help="run the 26.16.0 ARM64 presentation codec on Linux receiver output "
                             "(requires Python Unicorn, test-call and test-call-video)")
    args = parser.parse_args()
    with open(args.artifact, "rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        identity = digest.hexdigest()
        if identity == APK_SHA256:
            version = "26.18.2"
        elif identity == BUNDLE_SHA256:
            version = "26.16.0"
        else:
            raise CheckError(f"unsupported APK or APKM SHA-256 {identity}")
        source.seek(0)
        with zipfile.ZipFile(source) as artifact:
            if version == "26.16.0":
                video_test = Path(__file__).resolve().parents[1] / "build/test-call-video"
                check = args.check_sender or args.execute_presentation_codec
                check_bundle(artifact, video_test if check else None, args.execute_presentation_codec)
            else:
                if args.execute_presentation_codec:
                    raise CheckError("ARM64 control replay needs the 26.16.0 APKM split bundle")
                check_managed(artifact, version)
    print(f"PASS LINE Android {version} ({identity})")
    print("Relay and handset playback require a live call.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, zipfile.BadZipFile, CheckError, ValueError,
            subprocess.SubprocessError) as error:
        sys.exit(f"APK check failed: {error}")
