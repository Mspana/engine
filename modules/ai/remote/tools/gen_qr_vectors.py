#!/usr/bin/env python
"""Generate golden QR Code vectors for modules/ai/tests/test_ai_remote_qr.h.

The reference is the Python `qrcode` library (pip install qrcode). It runs the
same penalty rules for mask selection that AIRemoteQR does, so a correct
encoder reproduces the reference matrix module for module -- masking, format
bits and all.

Two things have to be pinned down for the comparison to be apples to apples:

  * Byte mode. `QRCode.add_data(text)` defaults to `optimize=20`, which happily
    re-segments the payload into numeric or alphanumeric mode when that is
    denser. AIRemoteQR is byte-mode only, so the payload is wrapped in a
    QRData with mode=MODE_8BIT_BYTE. Everything else about the configuration
    is the library default for `ERROR_CORRECT_M`.

  * Version. `make(fit=False)` forces the version passed to the constructor,
    `make(fit=True)` lets the library pick. AIRemoteQR::encode() has no version
    override -- it always takes the smallest version that fits -- so the
    emitted vectors are all fitted, and a payload sized into a version's band
    is how that version gets exercised. The forced path is still used by
    --sweep, which pins every per-version table against the reference
    independently of what automatic selection would have chosen.

Usage:
    python gen_qr_vectors.py                # print the C++ vector arrays
    python gen_qr_vectors.py --sweep PATH   # write a cross-check corpus
"""

import argparse
import random
import string
import sys
from importlib.metadata import version as package_version

import qrcode
from qrcode.util import MODE_8BIT_BYTE, QRData

# Level M data codewords per version, minus the mode indicator and character
# count (4 + 8 bits up to version 9, 4 + 16 from version 10).
MAX_PAYLOAD_PER_VERSION = {
    1: 14, 2: 26, 3: 42, 4: 62, 5: 84,
    6: 106, 7: 122, 8: 152, 9: 180, 10: 213,
}


def reference(text, version=None):
    """Encode with the reference library and return (version, matrix).

    version=None fits automatically; otherwise the version is forced.
    """
    qr = qrcode.QRCode(
        version=version,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=1,
        border=0,
    )
    qr.add_data(QRData(text.encode("utf-8"), mode=MODE_8BIT_BYTE))
    qr.make(fit=version is None)
    matrix = [[1 if cell else 0 for cell in row] for row in qr.modules]
    return qr.version, matrix


# ---------------------------------------------------------------------------
# Payloads
# ---------------------------------------------------------------------------

PAIRING_URL = "https://192.168.0.88:8420/#c=IytQMhgniCeDZXXGcgfqy7bV-o7pUDdFsj8-TjNAyb0"

VECTORS = [
    # (C++ identifier suffix, description, payload, forced version or None)
    ("v1_short", "the shortest useful payload, 2 bytes", "hi", None),
    ("v1_full", "14 bytes, exactly filling version 1", "abcdefghijklmn", None),
    (
        "v2_boundary",
        "15 bytes, one past version 1, so version 2 with its lone alignment pattern",
        "abcdefghijklmno",
        None,
    ),
    (
        "v3_utf8",
        "multi-byte UTF-8: 18 characters encode to 29 bytes",
        "Godot — 你好世界 — QR",
        None,
    ),
    (
        "v4_boundary",
        "62 bytes exactly filling version 4, the first version split into blocks",
        "Godot editor remote pairing: scan this code with the phone app",
        None,
    ),
    (
        "v5_pairing_url",
        "the realistic pairing URL",
        PAIRING_URL,
        None,
    ),
    (
        "v6_four_blocks",
        "106 bytes exactly filling version 6: four equal blocks",
        "Godot editor remote pairing over a self-signed certificate: "
        "scan the code, then approve the new device now",
        None,
    ),
    (
        "v7_version_info",
        "122 bytes exactly filling version 7, the first version carrying version information",
        "wss://192.168.0.88:8420/socket#session=IytQMhgniCeDZXXGcgfqy7bV-o7pUDdFsj8-TjNAyb0"
        "&v=1&fp=ab12cd34ef56ab12cd34ef56ab78cd90",
        None,
    ),
    (
        "v8_two_groups",
        "152 bytes exactly filling version 8: two block groups of unequal length",
        "The quick brown fox jumps over the lazy dog. " * 3 + "Pairing 012345678",
        None,
    ),
    (
        "v9_boundary",
        "180 bytes exactly filling version 9: five blocks across two groups",
        "Remote pairing session for the Godot editor bridge; scan to connect. " * 2
        + "abcdefghijklmnopqrstuvwxyz0123456789ABCDEF",
        None,
    ),
    (
        "v10_max",
        "213 bytes, the largest payload that fits: version 10, 16-bit character count",
        "https://192.168.0.88:8420/#c=" + "Q" * 184,
        None,
    ),
]


def _check_lengths():
    for name, _desc, text, forced in VECTORS:
        n = len(text.encode("utf-8"))
        if n > 213:
            sys.exit("payload %s is %d bytes, over the version 10 limit" % (name, n))
        if forced is None:
            fitted, _ = reference(text)
            expected = min(v for v, cap in MAX_PAYLOAD_PER_VERSION.items() if n <= cap)
            if fitted != expected:
                sys.exit("payload %s: fit gave v%d, table says v%d" % (name, fitted, expected))


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def cpp_literal(text):
    """Emit the payload as escaped bytes, so the source file stays pure ASCII."""
    out = []
    previous_was_hex_escape = False
    for byte in text.encode("utf-8"):
        ch = chr(byte)
        if 0x20 <= byte < 0x7F and ch not in '"\\':
            # \xNN escapes are greedy in C++, so a literal hex digit right after
            # one has to be pushed into a separate adjacent string literal.
            if previous_was_hex_escape and ch in string.hexdigits:
                out.append('" "')
            out.append(ch)
            previous_was_hex_escape = False
        elif ch == '"':
            out.append('\\"')
            previous_was_hex_escape = False
        elif ch == "\\":
            out.append("\\\\")
            previous_was_hex_escape = False
        else:
            out.append("\\x%02X" % byte)
            previous_was_hex_escape = True
    return '"%s"' % "".join(out)


def emit(stream):
    _check_lengths()
    stream.write("// Generated by modules/ai/remote/tools/gen_qr_vectors.py.\n")
    stream.write("// Reference: Python `qrcode` %s, ERROR_CORRECT_M, byte mode.\n\n"
                 % package_version("qrcode"))
    for name, desc, text, forced in VECTORS:
        version, matrix = reference(text, forced)
        size = len(matrix)
        assert size == version * 4 + 17
        flat = "".join(str(cell) for row in matrix for cell in row)
        stream.write("// %s\n" % desc)
        stream.write("// version %d, %dx%d, %d payload bytes%s\n"
                     % (version, size, size, len(text.encode("utf-8")),
                        ", version forced" if forced else ", version auto-selected"))
        stream.write("static const char *QR_%s_TEXT = %s;\n" % (name.upper(), cpp_literal(text)))
        stream.write("static const int QR_%s_SIZE = %d;\n" % (name.upper(), size))
        stream.write("static const char *QR_%s_MODULES =\n" % name.upper())
        for i in range(0, len(flat), size):
            stream.write('\t\t"%s"%s\n' % (flat[i:i + size], ";" if i + size >= len(flat) else ""))
        stream.write("\n")


# ---------------------------------------------------------------------------
# Sweep, used while developing the encoder
# ---------------------------------------------------------------------------


def sweep(dump_path):
    """Write a large randomised payload set plus reference matrices.

    Each payload is emitted twice: once fitted, and once forced to the version
    that fitting should have chosen. The two must agree, which is what pins
    automatic version selection against the reference.
    """
    random.seed(20260804)
    alphabet = string.ascii_letters + string.digits + "-_.:/#?=& %@!é中"
    payloads = []
    for _version, cap in MAX_PAYLOAD_PER_VERSION.items():
        payloads.append("x" * cap)  # exact boundary
        for _ in range(40):
            text = ""
            while len(text.encode("utf-8")) < cap:
                ch = random.choice(alphabet)
                if len((text + ch).encode("utf-8")) > cap:
                    break
                text += ch
            payloads.append(text)

    with open(dump_path, "w", encoding="utf-8") as handle:
        for text in payloads:
            fitted, matrix = reference(text)
            forced, forced_matrix = reference(text, fitted)
            if forced != fitted or forced_matrix != matrix:
                sys.exit("fit=True and fit=False disagree for a %d byte payload"
                         % len(text.encode("utf-8")))
            flat = "".join(str(cell) for row in matrix for cell in row)
            handle.write("%s\t%d\t%s\n" % (text.encode("utf-8").hex(), len(matrix), flat))
    return len(payloads)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sweep", metavar="PATH",
                        help="write a randomised cross-check corpus to PATH")
    args = parser.parse_args()
    if args.sweep:
        count = sweep(args.sweep)
        print("wrote %d sweep vectors to %s" % (count, args.sweep))
        return
    emit(sys.stdout)


if __name__ == "__main__":
    main()
