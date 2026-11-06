#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
"""Extract the CA0132 DSP firmware (FLS image) for the Sound Blaster AE-9
from the Creative Windows driver (CtxHda.sys).

The AE-9 DSP firmware is the same FLS format mainline ca0132 loads as
"ctefx-desktop.bin". Creative embeds several FLS images in the kernel driver
CtxHda.sys (from the "Sound Blaster AE-Series Driver" package). Each image is
a sequence of segments:

    struct dsp_image_seg { u32 magic; u32 chip_addr; u32 count; u32 data[count]; }

starting with magic 0x4c46584d ("MXFL"), walked until a segment with count==0.
The main DSP OS image is 655436 bytes (matches mainline ctefx.bin size).

Usage:
    python3 extract_firmware.py /path/to/CtxHda.sys  [out.bin] [--force]

The extracted image is checked against the MD5 of the firmware this driver
was developed and tested with. A different image (another driver version)
or the "largest image" fallback is refused unless --force is given.

Source of CtxHda.sys (in the Windows install):
  Program Files (x86)/Creative/Sound Blaster AE-Series Driver/
      AESeriesDriver/Driver/AMD64/CtxHda.sys
"""
import hashlib, struct, sys

MAGIC = 0x4c46584d  # "MXFL"
KNOWN_MD5 = {"85c8f2b47180cd6623a2b0e8b8911fe0": "AE-9 DSP image the driver is tested with"}


def walk_image(data, start):
    """Return (size, nsegs) of the FLS image at `start`, or None."""
    if struct.unpack_from('<I', data, start)[0] != MAGIC:
        return None
    off, nseg = start, 0
    while off + 12 <= len(data):
        _magic, _chip, count = struct.unpack_from('<III', data, off)
        nseg += 1
        if count == 0:            # is_last
            return (off + 12 - start, nseg)
        off += 12 + count * 4
        if nseg > 100000:
            return None
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    force = "--force" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--force"]
    sys_path = args[0]
    out = args[1] if len(args) > 1 else "ctefx-desktop.bin"
    data = open(sys_path, "rb").read()

    # Find the main DSP image: the 655436-byte one (== mainline ctefx.bin size)
    best = None
    i = data.find(struct.pack('<I', MAGIC))
    while i >= 0:
        r = walk_image(data, i)
        if r and r[0] == 655436:
            best = (i, r[0])
            break
        i = data.find(struct.pack('<I', MAGIC), i + 1)

    fallback = not best
    if not best:
        # fall back to the largest clean image (only written with --force)
        i = data.find(struct.pack('<I', MAGIC))
        while i >= 0:
            r = walk_image(data, i)
            if r and (not best or r[0] > best[1]):
                best = (i, r[0])
            i = data.find(struct.pack('<I', MAGIC), i + 1)

    if not best:
        print("No FLS image found")
        sys.exit(1)

    off, size = best
    img = data[off:off + size]
    md5 = hashlib.md5(img).hexdigest()
    if md5 in KNOWN_MD5:
        print("MD5 %s: %s" % (md5, KNOWN_MD5[md5]))
    else:
        why = "largest-image fallback" if fallback else "unknown image"
        print("MD5 %s: %s, not the image this driver was tested with" % (md5, why))
        if not force:
            print("Refusing to write it. Use --force to write it anyway (untested).")
            sys.exit(2)
    open(out, "wb").write(img)
    magic, chip, count = struct.unpack_from('<III', img, 0)
    print("Extracted %d bytes from offset 0x%x -> %s" % (size, off, out))
    print("magic=0x%08x chip_addr=0x%08x first_count=%d" % (magic, chip, count))
    assert magic == MAGIC, "bad magic!"


if __name__ == "__main__":
    main()
