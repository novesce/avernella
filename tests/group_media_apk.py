#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the group media paths in a locally supplied LINE 26.18.2 Android APK."""

import argparse
import hashlib
import struct
import sys
import zipfile


APK_SHA256 = "5432c8ad902609b4ce4773d834206da63f5c441610346bec0249235116b2e4c5"
BASE = "Lcom/linecorp/andromeda/"
SESSION = BASE + "core/session/ServiceSession;"
JNI = BASE + "jni/SessionJNIImpl;"
EXT = BASE + "jni/SessionExtensionJNIImpl;"
STRING = "Ljava/lang/String;"
INFO = BASE + "VideoControl$StreamInfo;"
CMD = "Le40/d$a;"
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


def group_video(dex):
    dispatch = SESSION + "->g(Le40/d;)Z"
    request = BASE + "core/session/ServiceSession$UserVideoRequest"
    request_constructor = (request + ";-><init>(" + STRING + STRING + request
                           + "$RequestType;" + BASE + "core/session/constant/VideoResolution;)V")
    dex.check(SESSION, "h(ZZ)V", [
        (0x02, SGET_OBJECT, CMD + "->SET_VIDEO_SEND_STATE:" + CMD),
        (0x14, VIRTUAL, dispatch),
    ])
    dex.check(BASE + "core/k;", "k(Ljava/util/HashSet;)Z", [
        (0x2c, DIRECT, request_constructor),
        (0x65, SGET_OBJECT, CMD + "->REQUEST_PARTICIPANT_VIDEO:" + CMD),
        (0x92, VIRTUAL, dispatch),
    ])
    dex.check(JNI, "S0(J" + STRING + ")Ljava/lang/Object;", [
        (0x00, STATIC, JNI + "->nVideoStreamGetUserFrame(J" + STRING + ")Ljava/lang/Object;"),
    ])
    dex.check(JNI, "W0(J" + INFO + ")V", [
        (0x00, STATIC, JNI + "->nVideoStreamUpdateLocalStreamInfo(JLjava/lang/Object;)V"),
    ])


def presentation_controls(dex):
    controls = (
        ("o1(J)Z", 0, "nSessionExtensionIsPresentationSupported(J)Z"),
        ("Q1(J)Z", 1, "nSessionExtensionStartPresentation(JZ)Z"),
        ("R1(IJ)Z", 0, "nSessionExtensionStopPresentation(JI)Z"),
        ("z1(J" + STRING + ")Z", 0, "nSessionExtensionPausePresentation(J" + STRING + ")Z"),
        ("K1(J" + STRING + ")Z", 0, "nSessionExtensionResumePresentation(J" + STRING + ")Z"),
    )
    for wrapper, offset, function in controls:
        flags, code = dex.declared(EXT, function)
        if not flags & 0x100 or code:
            raise CheckError(f"expected native declaration for {function}")
        dex.check(EXT, wrapper, [(offset, STATIC, EXT + "->" + function)])


def receiving_screen_share(dex):
    group_change = BASE + "VideoControl$Group$StreamChangeEvent;"
    dex.check("Luw7/b$b$a;", "streamSourceEvent(" + group_change + ")V", [
        (0x09, SGET_OBJECT, PARTY + "->PARTICIPANT:" + PARTY),
        (0x0d, VIRTUAL, INFO + "->d()" + SOURCE),
        (0x11, SGET_OBJECT, SOURCE + "->VIRTUAL_DISPLAY:" + SOURCE),
        (0x22, VIRTUAL, "Luw7/b$b;->d(" + STRING + ")Liy8/c3;"),
    ])
    receive = (0x05, SGET_OBJECT, DIRECTION + "->RX:" + DIRECTION)
    for event, parameter, callback in (("Start", "e", "b"), ("Stop", "f", "a")):
        dex.check("Luw7/b$d;", "onPresentation" + event + "(" + BASE + "e$" + parameter + ";)V", [
            receive,
            (0x11, INTERFACE, "Luw7/b$a;->" + callback + "(" + STRING + ")V"),
        ])
    dex.check("Luw7/b$d;", "onPresentationFrameEvent(" + BASE + "e$c;)V", [
        receive,
        (0x11, IGET_OBJECT, FRAME + "->userId:" + STRING),
        (0x19, INTERFACE_RANGE, "Luw7/b$a;->c(IJI" + STRING + ")V"),
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", help="path to your local LINE.apk")
    args = parser.parse_args()
    with open(args.apk, "rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != APK_SHA256:
            raise CheckError(f"unsupported APK SHA-256 {digest.hexdigest()} (expected {APK_SHA256})")
        source.seek(0)
        with zipfile.ZipFile(source) as apk:
            dex = Dex(apk.read("classes4.dex"))
            group_video(dex)
            presentation_controls(dex)
            receiving_screen_share(Dex(apk.read("classes12.dex")))
    print(f"PASS LINE Android 26.18.2 ({APK_SHA256})")
    print("classes4.dex: camera send state, participant video requests and per-user frames")
    print("classes4.dex: presentation capability, start, pause, resume and stop JNI calls")
    print("classes12.dex: group virtual-display selection and per-user presentation callbacks")
    print("These are static Android code paths. Relay and device delivery require a live call.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, zipfile.BadZipFile, CheckError) as error:
        sys.exit(f"APK check failed: {error}")
