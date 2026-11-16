#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
"""Extract the AE-9 headphone profiles (Sound Blaster Command's headphone
list) from Creative's installer files.

Usage: extract_hpeq.py <AE9HPEQ folder or .reg files> <out dir> [--force]

Creative's "AECMDMasterInstaller" carries one registry file per headphone
model in tmp/AE9HPEQ/ (unpack it with innoextract). Each holds the value
"<KEY>_SpeakerEQ_Y" (REG_BINARY, 4120 bytes): a speaker EQ image for the
DSP, one FLS segment of 1024 words at chip address 0x40000 plus the end
segment. Windows' installer writes it to the registry; the driver uploads
it into the DSP when "Headphone Profile" picks that model.

Each image is written as <out dir>/<KEY>.bin, after a structure check and
a check against the images this driver is tested with (MD5). Unknown
images are refused unless --force is given.
"""
import hashlib
import os
import re
import struct
import sys

MAGIC = 0x4c46584d  # "MXFL"
WORDS = 1024
SIZE = 12 + WORDS * 4 + 12

# The 32 profiles of AECMDMasterInstaller 3.4.92.00 (identical to what an
# installed Windows driver holds), tested with this driver.
KNOWN_MD5 = {
    "AKGM220": "4e3fb1625ec670d1d0c7fa21c22c02ad",
    "ATHM50": "d5428084de7779106dcfd9ee70d8583e",
    "CTAVNA": "227746a3ccfd89e19f8b9f296a0491ef",
    "CTAVNG": "2f3bda05b6c10a54ff54dbe003249997",
    "CTAVNL2": "f1a782e1a23651b98766a73bd34bc63e",
    "CTAVNL": "495fddb194e5a5354a79ba26d2109713",
    "CTEP3NC": "a383f12337c6eca6c408266a8485127c",
    "CTFATGHS": "269ef31ca2606c58b982bdb1e1585e71",
    "CTFlex": "44ecc726c6fb5299aea8acc2696d43b1",
    "CTSBINF": "acd6fc0f269a3814bc86daadd8451e9e",
    "EMUTEAK": "27313240d74eba7b1ad04835036dfd63",
    "ETHERC": "5966a440875a3ceae5887251476016ce",
    "GS3EP": "298e2dae813bc3e97764aeff715a5d2d",
    "H3": "269ef31ca2606c58b982bdb1e1585e71",
    "H5": "298e2dae813bc3e97764aeff715a5d2d",
    "H7": "3cfd9e062b00d8d04a552cda66544ff1",
    "HD201": "7b3089b6c275a89f2c2135e0eefab356",
    "HD280Pro": "80fda92c5302b9c3c6468113eeec7c06",
    "HD457": "2cc76b1f37a3582f2d5f073dbf50fe61",
    "HS50": "f22fe8b24748566ca0ae04f82b1d82e1",
    "HXHSCF": "ddf6cd5abad90145d758f808d2ce69ab",
    "IP3EP": "3cfd9e062b00d8d04a552cda66544ff1",
    "IP5EP": "0a99fbd49064e2c0c2b8f36d8dab2fd8",
    "MDR1R": "cca6d35b20d3a0a9a361c773d7123790",
    "OUTLIERB": "30dcb45f84754a653922cbe3aebd4a75",
    "OUTLIER": "6d0d34760b578ebc885df73ac1dae82b",
    "P5": "0a99fbd49064e2c0c2b8f36d8dab2fd8",
    "PFX2HR": "8cac9d0640458dadc8e21cf2911efc89",
    "SBBLAZE": "daa269490214f4e19beda52ce4a2471e",
    "SXFIAIRC": "a008db13d99914827e2d629c6e36c475",
    "SXFIAIR": "4d54a1dfb49deb06e593934470f24498",
    "WALNUT": "d895f2e0931770964192da9f6a9cc876",
}


def reg_text(raw):
    """A .reg file as text (Windows writes UTF-16 or plain ASCII ones)."""
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return raw.decode('utf-16')
    return raw.decode('latin-1')


def images(text):
    """(key, bytes) for every "<KEY>_SpeakerEQ_Y"=hex:... value."""
    text = re.sub(r'\\\r?\n\s*', '', text)          # joined continuation lines
    for key, hexdata in re.findall(r'"([A-Za-z0-9]+)_SpeakerEQ_Y"=hex:([0-9a-fA-F,]+)', text):
        yield key, bytes.fromhex(hexdata.replace(',', ''))


def valid(blob):
    if len(blob) != SIZE:
        return False
    magic, chip, count = struct.unpack_from('<III', blob, 0)
    end_magic, _, end_count = struct.unpack_from('<III', blob, 12 + WORDS * 4)
    return (magic, chip, count, end_magic, end_count) == (MAGIC, 0x40000, WORDS, MAGIC, 0)


def main():
    args = [a for a in sys.argv[1:] if a != '--force']
    force = '--force' in sys.argv
    if len(args) < 2:
        print(__doc__)
        return 2
    *srcs, out = args
    files = []
    for s in srcs:
        if os.path.isdir(s):
            files += sorted(os.path.join(s, f) for f in os.listdir(s) if f.lower().endswith('.reg'))
        else:
            files.append(s)
    os.makedirs(out, exist_ok=True)
    done, refused = [], []
    for f in files:
        with open(f, 'rb') as fh:
            text = reg_text(fh.read())
        for key, blob in images(text):
            md5 = hashlib.md5(blob).hexdigest()
            if not valid(blob):
                refused.append(f'{key} (not a speaker EQ image)')
                continue
            if KNOWN_MD5.get(key) != md5 and not force:
                refused.append(f'{key} (md5 {md5}: not a tested image; --force to use it)')
                continue
            with open(os.path.join(out, key + '.bin'), 'wb') as fh:
                fh.write(blob)
            done.append(key)
    print(f'{len(done)} headphone profiles written to {out}')
    for r in refused:
        print(f'  refused: {r}', file=sys.stderr)
    return 0 if done and not refused else 1


if __name__ == '__main__':
    sys.exit(main())
