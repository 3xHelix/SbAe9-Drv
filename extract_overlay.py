#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
"""Extract the AE-9 DSP overlay image from the Creative Windows driver.

Usage: extract_overlay.py /path/to/CtxHda.sys [out.bin]
       (default out: ctefx-ae9-overlay.bin; install to /lib/firmware/)

CtxHda.sys embeds several FLS images (see extract_firmware.py). The first
(655436 bytes) is the main DSP image, ctefx-desktop.bin. The second (432
bytes: 78 words to chip 0x0b7b10, 21 words to chip 0x03f3a0) is the overlay
Windows transfers during the MASTERCONTROL boot handshake (GET 0x80/0x3a +
0x3b, between boot states 3 and 4). The driver loads it with overlay=1.
"""
import struct
import sys

MAGIC = 0x4c46584d  # "MXFL"


def images(data):
    pos, out = 0, []
    while True:
        i = data.find(struct.pack('<I', MAGIC), pos)
        if i < 0:
            return out
        off, segs, ok = i, [], True
        while off + 12 <= len(data):
            m, addr, cnt = struct.unpack_from('<III', data, off)
            if m != MAGIC:
                ok = False
                break
            segs.append((addr, cnt))
            off += 12 + cnt * 4
            if cnt == 0:
                break
        if ok and segs and segs[-1][1] == 0:
            out.append((i, data[i:off], segs))
            pos = off
        else:
            pos = i + 4


def main():
    data = open([a for a in sys.argv[1:] if a != '--force'][0], 'rb').read()
    args = [a for a in sys.argv[1:] if a != '--force']
    out = args[1] if len(args) > 1 else 'ctefx-ae9-overlay.bin'
    imgs = images(data)
    if len(imgs) < 2:
        sys.exit("fewer than two FLS images found")
    off, blob, segs = imgs[1]
    want = [(0x0b7b10, 78), (0x03f3a0, 21)]
    if [(a, c) for a, c in segs[:2]] != want:
        sys.exit(f"second image doesn't look like the overlay: {segs}")
    import hashlib
    md5 = hashlib.md5(blob).hexdigest()
    if md5 != '0e222c941f708a2caf93d50d2aba5ef8' and '--force' not in sys.argv:
        sys.exit(f"MD5 {md5} is not the tested overlay image; use --force to write it anyway")
    open(out, 'wb').write(blob)
    print(f"wrote {out}: {len(blob)} bytes from offset 0x{off:x}, segments {segs[:2]}")


if __name__ == '__main__':
    main()
