#!/usr/bin/env python3
"""Assembles the built-in TinySID demo tune and writes assets/demo.sid plus
src/dsp/demo_sid.h.

The tune is original (a minor-key bass / arp / lead loop with a filter sweep),
written as 6502 code through the tiny assembler below so the repo has a
default asset that needs no third-party music. Subtune 1 runs at 125 BPM,
subtune 2 at ~187 BPM.
"""
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOAD = 0x1000
PAL = 985248.0

# mnemonic -> {mode: opcode}
OPS = {
    "LDA": {"imm": 0xA9, "zp": 0xA5, "abs": 0xAD, "absy": 0xB9, "absx": 0xBD},
    "STA": {"zp": 0x85, "abs": 0x8D},
    "LDX": {"imm": 0xA2}, "LDY": {"imm": 0xA0, "zp": 0xA4},
    "CMP": {"imm": 0xC9, "zp": 0xC5}, "AND": {"imm": 0x29},
    "INC": {"zp": 0xE6}, "TAX": {"imp": 0xAA}, "TAY": {"imp": 0xA8},
    "LSR": {"imp": 0x4A}, "JMP": {"abs": 0x4C}, "RTS": {"imp": 0x60},
    "BEQ": {"rel": 0xF0}, "BNE": {"rel": 0xD0}, "BCS": {"rel": 0xB0}, "BCC": {"rel": 0x90},
}
SIZE = {"imp": 1, "imm": 2, "zp": 2, "rel": 2, "abs": 3, "absx": 3, "absy": 3}


def parse_operand(mn, text):
    if text is None:
        return "imp", None
    if mn in ("BEQ", "BNE", "BCS", "BCC"):
        return "rel", text
    suffix = ""
    if text.endswith(",X"):
        suffix, text = "x", text[:-2]
    elif text.endswith(",Y"):
        suffix, text = "y", text[:-2]
    if text.startswith("#"):
        return "imm", int(text[1:].lstrip("$"), 16) if text[1] == "$" else int(text[1:])
    if text.startswith("$"):
        digits = len(text) - 1
        value = int(text[1:], 16)
        if digits <= 2 and not suffix:
            return "zp", value
        return "abs" + suffix, value
    return "abs" + suffix, text  # label


def assemble(lines, org):
    items, labels, pc = [], {}, org
    for raw in lines:
        line = raw.split(";")[0].strip()
        if not line:
            continue
        if line.endswith(":"):
            labels[line[:-1]] = pc
            continue
        if line.startswith(".byte"):
            data = [int(v.strip(), 0) for v in line[5:].split(",")]
            items.append(("data", pc, data))
            pc += len(data)
            continue
        parts = line.split(None, 1)
        mn = parts[0].upper()
        mode, val = parse_operand(mn, parts[1].strip() if len(parts) > 1 else None)
        items.append(("op", pc, mn, mode, val))
        pc += SIZE[mode]
    out = bytearray()
    for it in items:
        if it[0] == "data":
            out += bytes(it[2])
            continue
        _, pc0, mn, mode, val = it
        out.append(OPS[mn][mode])
        if mode == "imp":
            continue
        if mode == "rel":
            off = labels[val] - (pc0 + 2)
            assert -128 <= off <= 127, f"branch to {val} out of range"
            out.append(off & 0xFF)
            continue
        if isinstance(val, str):
            val = labels[val]
        if mode in ("imm", "zp"):
            out.append(val & 0xFF)
        else:
            out += struct.pack("<H", val)
    return bytes(out), labels


def build():
    first_midi = 24
    notes = 60
    freq = [round(440.0 * 2 ** ((first_midi + i - 69) / 12) * 16777216 / PAL) for i in range(notes)]
    sweep = [int(20 + 210 * (0.5 - 0.5 * math.cos(2 * math.pi * i / 64))) for i in range(64)]

    def idx(midi):
        return midi - first_midi

    REST = 0xFF
    roots = [45, 41, 48, 43]                      # Am F C G
    third = [3, 4, 4, 4]
    bass, arp, lead = [], [], []
    melody = [
        [69, REST, 72, REST, 76, REST, 72, 69],
        [65, REST, 69, REST, 72, REST, 69, 65],
        [72, REST, 76, 79, 76, REST, 72, REST],
        [71, REST, 74, REST, 79, 74, 71, REST],
    ]
    for bar in range(4):
        r = roots[bar]
        for step in range(8):
            bass.append(idx(r) if step % 2 == 0 else REST)
            tones = [r + 12, r + 12 + third[bar], r + 19, r + 24]
            arp.append(idx(tones[step % 4]))
            m = melody[bar][step]
            lead.append(REST if m == REST else idx(m))

    return freq, sweep, bass, arp, lead


def main():
    freq, sweep, bass, arp, lead = build()
    L = []
    a = L.append

    # zero page: $FB tick, $FC step, $FD ticks-per-step, $FE frame counter

    a("init:")
    a("CMP #$00")
    a("BEQ slow")
    a("LDA #$04")
    a("STA $FD")
    a("JMP go")
    a("slow:")
    a("LDA #$06")
    a("STA $FD")
    a("go:")
    a("LDA #$00")
    a("STA $FB"); a("STA $FC"); a("STA $FE")
    # voice setup: (base, pw_lo, pw_hi, ad, sr)
    for base, pwlo, pwhi, ad, sr in ((0xD400, 0x00, 0x08, 0x08, 0xA9),
                                      (0xD407, 0x00, 0x06, 0x0A, 0xA4),
                                      (0xD40E, 0x00, 0x08, 0x09, 0x84)):
        a(f"LDA #${pwlo:02X}"); a(f"STA ${base + 2:04X}")
        a(f"LDA #${pwhi:02X}"); a(f"STA ${base + 3:04X}")
        a(f"LDA #${ad:02X}");   a(f"STA ${base + 5:04X}")
        a(f"LDA #${sr:02X}");   a(f"STA ${base + 6:04X}")
    a("LDA #$F3"); a("STA $D417")              # res 15, voices 1+2 through the filter
    a("LDA #$1F"); a("STA $D418")              # low-pass, volume 15
    a("RTS")

    a("play:")
    a("INC $FE")
    a("LDA $FE"); a("LSR"); a("LSR"); a("AND #$3F"); a("TAY")
    a("LDA sweeptbl,Y"); a("STA $D416")
    a("INC $FB")
    a("LDA $FB"); a("CMP $FD")
    a("BCS tick")
    a("RTS")
    a("tick:")
    a("LDA #$00"); a("STA $FB")
    a("LDY $FC")
    for k, (base, pat, wave) in enumerate(((0xD400, "leadpat", 0x20),
                                            (0xD407, "basspat", 0x40),
                                            (0xD40E, "arppat", 0x10))):
        a(f"LDA {pat},Y")
        a("CMP #$FF")
        a(f"BEQ off{k}")
        a("TAX")
        a("LDA freqlo,X"); a(f"STA ${base:04X}")
        a("LDA freqhi,X"); a(f"STA ${base + 1:04X}")
        a(f"LDA #${wave | 1:02X}"); a(f"STA ${base + 4:04X}")
        a(f"JMP next{k}")
        a(f"off{k}:")
        a(f"LDA #${wave:02X}"); a(f"STA ${base + 4:04X}")
        a(f"next{k}:")
    a("INC $FC")
    a("LDA $FC"); a("AND #$1F"); a("STA $FC")
    a("RTS")

    def table(name, values):
        a(f"{name}:")
        for i in range(0, len(values), 16):
            a(".byte " + ", ".join(str(v) for v in values[i:i + 16]))

    table("freqlo", [f & 0xFF for f in freq])
    table("freqhi", [f >> 8 for f in freq])
    table("sweeptbl", sweep)
    table("leadpat", lead)
    table("basspat", bass)
    table("arppat", arp)

    code, labels = assemble(L, LOAD)

    def field(text):
        return text.encode("ascii").ljust(32, b"\0")

    hdr = b"PSID" + struct.pack(">HHHHHHHI", 2, 0x7C, 0, labels["init"], labels["play"], 2, 1, 0)
    hdr += field("TinySID Demo") + field("TinySID") + field("2026 public domain")
    hdr += struct.pack(">HBBBB", 0x0004, 0, 0, 0, 0)       # flags: PAL
    assert len(hdr) == 0x7C
    sid = hdr + struct.pack("<H", LOAD) + code

    os.makedirs(os.path.join(ROOT, "assets"), exist_ok=True)
    with open(os.path.join(ROOT, "assets", "demo.sid"), "wb") as f:
        f.write(sid)
    write_header(os.path.join(ROOT, "src", "dsp", "demo_sid.h"), sid, "demo")
    print(f"demo.sid: {len(sid)} bytes (code {len(code)}), init=${labels['init']:04X} play=${labels['play']:04X}")


def write_header(path, data, label):
    with open(path, "w", newline="\n") as f:
        f.write(f"/* generated by tools/make_demo_sid.py or tools/embed_sid.py: {label} */\n")
        f.write("static const unsigned char default_sid_data[] = {\n")
        for i in range(0, len(data), 16):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write(f"static const unsigned int default_sid_len = {len(data)};\n")


if __name__ == "__main__":
    main()
