#!/usr/bin/env python3
"""Make libcarplay_altscreen.so add the alt screen to the SETUP response only when the
source asked for it. Wireless adapters never ask, and with the forced alt screen their
session never comes up.

AirPlayReceiverSessionSetup @0x2d8e4 (MU1329 build, also after patch_altscreen_fps.py):
    cmp r0, #0 / mov r5, #0 / beq skip / ... bl accept_alt_in_setup_response
r7 holds the `requested` flag (logged by SETUP_FEATURE_NEGOTIATION) and r5 is already 0,
so the mov becomes `cmpne r7, #0`.

  tools/patch_altscreen_requested.py <in> <out>
"""
import sys

OFF = 0x2D8E8
STOCK = bytes.fromhex("0050a0e3")    # mov r5, #0
PATCHED = bytes.fromhex("00005713")  # cmpne r7, #0

src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())
cur = bytes(data[OFF:OFF + 4])
if cur not in (STOCK, PATCHED):
    raise SystemExit(f"{src}: unexpected bytes {cur.hex()} at {OFF:#x}, unknown AltScreen build")
data[OFF:OFF + 4] = PATCHED
open(dst, "wb").write(data)
