"""Embeds the remote-access web client into the binary.

The client has to be served by the editor itself (a phone browser has no other
way to fetch it), so the files are compiled in as byte arrays rather than read
from disk at runtime.
"""

import os


MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".json": "application/json; charset=utf-8",
    ".webmanifest": "application/manifest+json",
}


def make_webclient_header(target, source, env):
    entries = []
    with open(str(target[0]), "w", encoding="utf-8", newline="\n") as out:
        out.write("/* THIS FILE IS GENERATED — see modules/ai/remote/webclient_builders.py */\n")
        out.write("#pragma once\n\n#include <stdint.h>\n\n")

        for i, src in enumerate(source):
            path = str(src)
            name = os.path.basename(path)
            ext = os.path.splitext(name)[1].lower()
            mime = MIME_TYPES.get(ext, "application/octet-stream")

            with open(path, "rb") as f:
                data = f.read()

            symbol = "_ai_webclient_%d" % i
            out.write("static const unsigned char %s[] = {\n" % symbol)
            for chunk_start in range(0, len(data), 16):
                chunk = data[chunk_start : chunk_start + 16]
                out.write("\t" + ",".join("0x%02x" % b for b in chunk) + ",\n")
            # Trailing zero keeps the array valid when a file is empty.
            out.write("\t0x00\n};\n\n")
            entries.append((name, mime, symbol, len(data)))

        out.write("struct AIWebClientFile {\n")
        out.write("\tconst char *name;\n\tconst char *mime;\n")
        out.write("\tconst unsigned char *data;\n\tint size;\n};\n\n")
        out.write("static const AIWebClientFile AI_WEBCLIENT_FILES[] = {\n")
        for name, mime, symbol, size in entries:
            out.write('\t{ "%s", "%s", %s, %d },\n' % (name, mime, symbol, size))
        out.write("};\n\n")
        out.write("static const int AI_WEBCLIENT_FILE_COUNT = %d;\n" % len(entries))
