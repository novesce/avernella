# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional instruction-level replay of the 26.16.0 ARM64 PLANET control codec."""

import struct


def replay(data, records):
    try:
        from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM
        from unicorn.arm64_const import (UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP,
                                         UC_ARM64_REG_TPIDR_EL0, UC_ARM64_REG_X0)
    except ImportError as error:
        raise ValueError("--execute-presentation-codec requires the Python Unicorn package") from error

    # Reached exclusively via the verified 26.16.0 bundle. No Android loader,
    # initializers, JNI or system calls are needed by these two packet routines.
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0, 0xe00000)
    phoff, = struct.unpack_from("<Q", data, 32)
    stride, count = struct.unpack_from("<HH", data, 54)
    for i in range(count):
        kind, _, offset, address, _, size = struct.unpack_from("<II4Q", data, phoff + i * stride)
        if kind == 1 and size:
            cpu.mem_write(address, data[offset:offset + size])
    # Resolve only the six internal PLT calls that the control codec needs.
    for entry, target in ((0xc01dd0, 0xa509dc), (0xc01de0, 0xa50718),  # varint decode/encode
                          (0xc0ab80, 0x82baf8), (0xc0abb0, 0x82be68),  # writer varint/u8
                          (0xc0abf0, 0x82c424), (0xc0ac10, 0x82c6d0)):  # reader varint/u8
        branch = 0x14000000 | (((target - entry) // 4) & 0x3ffffff)
        cpu.mem_write(entry, struct.pack("<I", branch))

    arena = 0x1000000
    cpu.mem_map(arena, 0x20000)
    control, cursor, buf = arena, arena + 0x1000, arena + 0x2000
    end_pc = arena + 0x1fffc

    def call(pc, *args):
        for index, value in enumerate(args):
            cpu.reg_write(UC_ARM64_REG_X0 + index, value)
        cpu.reg_write(UC_ARM64_REG_SP, arena + 0x1f000)
        cpu.reg_write(UC_ARM64_REG_TPIDR_EL0, arena + 0x3000)
        cpu.reg_write(UC_ARM64_REG_LR, end_pc)
        try:
            cpu.emu_start(pc, end_pc, count=10000)
        except UcError as error:
            raise ValueError(f"ARM64 control codec stopped at {cpu.reg_read(UC_ARM64_REG_PC):#x}: "
                             f"{error}") from error
        if cpu.reg_read(UC_ARM64_REG_PC) != end_pc:
            raise ValueError("ARM64 control codec exceeded its instruction budget")
        return cpu.reg_read(UC_ARM64_REG_X0)

    def put(at, fmt, *values):
        cpu.mem_write(at, struct.pack("<" + fmt, *values))

    def get(at, fmt):
        return struct.unpack("<" + fmt, cpu.mem_read(at, struct.calcsize("<" + fmt)))[0]

    def check(source, source_offset, kind, direction, linux_wire=None):
        cpu.mem_write(control, bytes(0x438))
        put(control + 0x34, "I", 4)
        put(control + 0x44, "I", direction)
        put(control + 0x410, "I", kind)
        setter = 0x8223c4 if kind == 9 else 0x822658
        call(setter, control, source, source_offset)
        call(0x82baec, cursor, buf, 64)  # vns_pdtp_writer_init
        if not call(0x828cf8, control, cursor):
            raise ValueError("Android could not write the presentation control")
        size = get(cursor + 16, "Q")
        if not 0 < size <= 64:
            raise ValueError("unexpected ARM64 writer output length")
        native_wire = bytes(cpu.mem_read(buf, size))
        if linux_wire is not None:
            if native_wire != linux_wire:
                raise ValueError("Android and Linux encoded different presentation controls")
            cpu.mem_write(buf, linux_wire)
        cpu.mem_write(control, bytes(0x438))
        call(0x82c418, cursor, buf, size)  # vns_pdtp_reader_init
        if not call(0x829414, control, cursor, 0):
            raise ValueError("Android rejected the presentation control")
        fields = (get(control + 0x34, "I"), get(control + 0x44, "I"), get(control + 0x408, "Q"),
                  get(control + 0x410, "I"), get(control + 0x418, "I"), get(control + 0x420, "Q"))
        if fields != (4, direction, 0, kind, source, source_offset) or get(cursor + 16, "Q") != size:
            raise ValueError(f"Android decoded a different presentation control: {fields}")

    for wire, source, offset in records:
        check(int(source), int(offset), 9, 2, bytes.fromhex(wire))
    # Exercise the announcement in the opposite direction and varint boundaries
    # beyond the normal duration of a call. These additional packets are synthetic.
    for source_offset in (0, 1234, 16384, 2**35 + 83):
        check(801, source_offset, 12, 1)
        check(801, source_offset, 9, 2)
    print(f"Android ARM64 reader and writer matched {len(records)} Linux receiver controls "
          "and round-tripped eight synthetic availability/resume controls")
