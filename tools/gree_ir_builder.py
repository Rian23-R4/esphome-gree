#!/usr/bin/env python3
"""Build Gree (YAW1F/YX1F family) IR frames as ESPHome Pronto strings.

Frames are built with canonical timings (IRremoteESP8266):
hdr 9000/4500 us, bit mark 620 us, zero 540 us, one 1600 us, gap 620+19980 us.

Usage:
    python3 gree_ir_builder.py --list                 # show the recipe frames
    python3 gree_ir_builder.py --temp 26 --eco        # climate-family eco frame
    python3 gree_ir_builder.py --temp 26 --dred d2    # remote-family state + C frame
"""
import argparse

def checksum(bs):
    s = 10
    for i in range(4):
        s += bs[i] & 0xF
    for i in range(4, 7):
        s += bs[i] >> 4
    return (s & 0xF) << 4

def to_stream(bs):
    st = "".join(format(x, "08b")[::-1] for x in bs[:4]) + "010"
    st += "".join(format(x, "08b")[::-1] for x in bs[4:])
    return st

def build_pronto(bs):
    if len(bs) == 7:                              # checksum not supplied
        bs = bs + [checksum(bs)]
    st = to_stream(bs[:8])
    words = ["0156", "00AB"]                      # 9000 / 4500 us
    for i in range(67):
        if i == 35:
            words += ["0018", "02F8"]              # footer mark 620 + gap 19980
        words += ["0018", "003D" if st[i] == "1" else "0015"]
    words += ["0018", "02F8"]
    n = len(words) // 2
    return " ".join(["0000", "006D", "%04X" % n, "0000"] + words)

TEMP_BASE = 16     # byte1 = temp - 16 (24 C -> 0x08, 27 C -> 0x0B)

DRED_C = {
    "off": [0x00, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x00],
    "d3":  [0x01, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x00],
    "d2":  [0x00, 0x00, 0x00, 0xA4, 0x00, 0x00, 0x00],
}

def state_frame(temp, family="remote"):
    b1 = temp - TEMP_BASE
    if family == "remote":
        return [0x09, b1, 0x60, 0x50, 0x06, 0x80, 0x00]
    return [0x09, b1, 0x20, 0x50, 0x00, 0x20, 0x20]

def eco_frame(temp, on=True):
    base = state_frame(temp, "climate")
    low = 0x4 if on else 0x0                     # Econo bit = byte7 bit 2
    return base + [checksum(base) | low]         # high nibble = checksum

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--temp", type=int, default=24)
    p.add_argument("--eco", action="store_true")
    p.add_argument("--dred", choices=list(DRED_C))
    p.add_argument("--list", action="store_true")
    a = p.parse_args()
    if a.list or not (a.eco or a.dred):
        print("SE on  (Cool %d):" % a.temp, build_pronto(eco_frame(a.temp, True)))
        print("SE off (Cool %d):" % a.temp, build_pronto(eco_frame(a.temp, False)))
        print("State  (Cool %d):" % a.temp, build_pronto(state_frame(a.temp)))
        for lvl in ("d2", "d3", "off"):
            print("C %-4s :" % lvl, build_pronto(DRED_C[lvl]))
        return
    if a.eco:
        print(build_pronto(eco_frame(a.temp, True)))
    if a.dred:
        print("# 1) state frame")
        print(build_pronto(state_frame(a.temp)))
        print("# 2) DRED %s frame C" % a.dred)
        print(build_pronto(DRED_C[a.dred]))

if __name__ == "__main__":
    main()
