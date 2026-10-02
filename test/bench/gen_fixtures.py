#!/usr/bin/env python3
"""Generate the fixtures used by the capability probes.

Writes one valid PNG, WAV and PDF, plus ready-to-send OpenAI payloads for each.
Nothing here is downloaded: the files are built byte by byte so the probes are
reproducible offline and the "did it really read it?" answers are checkable by
eye (red square, 440 Hz tone, one secret code).

    python3 test/bench/gen_fixtures.py [outdir]     # default: ./fixtures
"""
import base64
import io
import json
import math
import os
import struct
import sys
import wave
import zlib

SECRET = "SECRET-CODE-4271"


def make_png(path, w=64, h=64, rgb=(220, 30, 30)):
    """A real PNG: solid background with a white rectangle in the middle."""
    raw = b""
    for y in range(h):
        raw += b"\x00"
        for x in range(w):
            white = 20 <= x < 44 and 20 <= y < 44
            raw += bytes((255, 255, 255)) if white else bytes(rgb)

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)
    return png


def make_wav(path, secs=1.0, rate=16000, freq=440.0, amp=12000):
    """A real 16-bit PCM WAV: one second of a 440 Hz sine (A4)."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(rate * secs)):
            frames += struct.pack("<h", int(amp * math.sin(2 * math.pi * freq * i / rate)))
        w.writeframes(bytes(frames))
    data = buf.getvalue()
    with open(path, "wb") as fh:
        fh.write(data)
    return data


def make_pdf(path):
    """A minimal valid PDF with one extractable line of text."""
    text = f"BT /F1 14 Tf 20 50 Td ({SECRET} funcroute PDF) Tj ET".encode()
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
            b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"),
        4: b"<< /Length %d >>\nstream\n" % len(text) + text + b"\nendstream",
        5: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    }
    out = bytearray(b"%PDF-1.4\n")
    offs = {}
    for n in sorted(objs):
        offs[n] = len(out)
        out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n" % (len(objs) + 1) + b"0000000000 65535 f \n"
    for n in sorted(objs):
        out += b"%010d 00000 n \n" % offs[n]
    out += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, xref))
    with open(path, "wb") as fh:
        fh.write(bytes(out))
    return bytes(out)


def build(outdir):
    os.makedirs(outdir, exist_ok=True)
    png = make_png(os.path.join(outdir, "square.png"))
    wav = make_wav(os.path.join(outdir, "tone.wav"))
    pdf = make_pdf(os.path.join(outdir, "note.pdf"))

    b64 = lambda b: base64.b64encode(b).decode()  # noqa: E731

    probes = {
        "image": {
            "model": "unset", "max_tokens": 64,
            "messages": [{"role": "user", "content": [
                {"type": "text",
                 "text": "In one short sentence: what colour is the background square?"},
                {"type": "image_url",
                 "image_url": {"url": "data:image/png;base64," + b64(png)}},
            ]}],
        },
        "audio": {
            "model": "unset", "max_tokens": 64,
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": "In one short sentence: describe this audio."},
                {"type": "input_audio",
                 "input_audio": {"data": b64(wav), "format": "wav"}},
            ]}],
        },
        "file": {
            "model": "unset", "max_tokens": 64,
            "messages": [{"role": "user", "content": [
                {"type": "text",
                 "text": "What is the SECRET CODE printed in this PDF? "
                         "Answer with just the code."},
                {"type": "file",
                 "file": {"filename": "note.pdf",
                          "file_data": "data:application/pdf;base64," + b64(pdf)}},
            ]}],
        },
    }
    for kind, payload in probes.items():
        with open(os.path.join(outdir, kind + ".json"), "w") as fh:
            json.dump(payload, fh)

    print(f"fixtures in {outdir}/")
    for name, blob in (("square.png", png), ("tone.wav", wav), ("note.pdf", pdf)):
        print(f"  {name:12s} {len(blob):8d}B")
    for kind in probes:
        p = os.path.join(outdir, kind + ".json")
        print(f"  {kind + '.json':12s} {os.path.getsize(p):8d}B")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "fixtures")
