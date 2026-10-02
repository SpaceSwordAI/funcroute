#!/usr/bin/env python3
"""End-to-end check against real upstreams: does each kind reach the right model?

Starts ./funcroute on a scratch database, sends one request per attachment kind
plus a mixed request, streams one of them, exercises the Ollama routes, then
prints the logged routing decisions so you can see where each request went.

    export DEEPSEEK_API_KEY=... OPENROUTER_API_KEY=...
    python3 test/bench/gen_fixtures.py test/bench/fixtures
    python3 test/bench/e2e_live.py

Requires a built ./funcroute and a valid config.json. Costs a few cents.
Exits non-zero if any check fails, so it works in a Makefile or CI.
"""
import argparse
import base64
import json
import os
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_env_file(path):
    """Make a .env available to the child process without exporting it."""
    env = dict(os.environ)
    if os.path.exists(path):
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    env.setdefault(key, value)
    return env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--base-url", default="http://127.0.0.1:11434")
    ap.add_argument("--fixtures", default="test/bench/fixtures")
    ap.add_argument("--binary", default="./funcroute")
    ap.add_argument("--env-file", default=".env")
    args = ap.parse_args()

    fix = args.fixtures
    with open(f"{fix}/image.json") as fh:
        img = json.load(fh)
    with open(f"{fix}/audio.json") as fh:
        aud = json.load(fh)
    with open(f"{fix}/file.json") as fh:
        fil = json.load(fh)
    with open(f"{fix}/square.png", "rb") as fh:
        png_b64 = base64.b64encode(fh.read()).decode()

    workdir = tempfile.mkdtemp(prefix="funcroute-bench-")
    db = os.path.join(workdir, "bench.db")
    log = os.path.join(workdir, "server.log")

    def post(path, body, timeout=180):
        req = urllib.request.Request(
            args.base_url + path, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = resp.read().decode("utf-8", "replace")
            if body.get("stream"):
                return resp.status, raw
            data = json.loads(raw)
            # Ollama /api/generate uses top-level "response"; /api/chat and
            # OpenAI both hang the text off a message object.
            if "response" in data:
                return resp.status, str(data.get("response") or "").strip().replace("\n", " ")
            msg = data.get("choices", [{}])[0].get("message") or data.get("message") or {}
            return resp.status, (msg.get("content") or "").strip().replace("\n", " ")
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", "replace")
            try:
                err = json.loads(raw).get("error", raw)
                raw = err.get("message") if isinstance(err, dict) else err
            except ValueError:
                pass
            return exc.code, "ERR: " + str(raw)[:150]

    def mk(payload, max_tokens=400, **extra):
        body = json.loads(json.dumps(payload))
        body["model"] = "bhag"
        body["max_tokens"] = max_tokens
        body.update(extra)
        return body

    def mixed():
        """image + audio: the most restrictive kind has to win."""
        parts = img["messages"][0]["content"] + aud["messages"][0]["content"]
        return {"messages": [{"role": "user", "content": parts}]}

    results = []

    def check(name, ok, detail):
        results.append((name, ok))
        print(f"{'PASS' if ok else 'FAIL'}  {name:34s} {detail[:88]}")

    proc = subprocess.Popen(
        [os.path.abspath(args.binary), args.config, "--log", db],
        cwd=ROOT, env=load_env_file(os.path.join(ROOT, args.env_file)),
        stdout=open(log, "w"), stderr=subprocess.STDOUT)
    try:
        for _ in range(60):
            time.sleep(0.25)
            with open(log) as fh:
                if "listening" in fh.read():
                    break
        else:
            sys.exit("server never started:\n" + open(log).read())

        print("--- startup banner ---")
        print("".join(l for l in open(log) if "->" in l or "part types" in l))
        print("--- checks ---")

        code, txt = post("/v1/chat/completions",
                         mk({"messages": [{"role": "user", "content": "Reply with exactly: PONG"}]}))
        check("text", code == 200 and "PONG" in txt.upper(), f"{code} {txt}")

        code, txt = post("/v1/chat/completions", mk(img))
        check("image", code == 200 and "red" in txt.lower(), f"{code} {txt}")

        code, txt = post("/v1/chat/completions", mk(fil))
        check("file/PDF", code == 200 and "4271" in txt, f"{code} {txt}")

        code, txt = post("/v1/chat/completions", mk(aud))
        check("audio", code == 200 and bool(txt) and not txt.startswith("ERR"), f"{code} {txt}")

        code, txt = post("/v1/chat/completions", mk(mixed()))
        check("image+audio (audio wins)", code == 200 and bool(txt) and not txt.startswith("ERR"),
              f"{code} {txt}")

        code, raw = post("/v1/chat/completions", mk(img, max_tokens=1500, stream=True))
        chunks = [l for l in raw.splitlines() if l.startswith("data: ")]
        content = ""
        for line in chunks:
            if line[6:].strip() == "[DONE]":
                continue
            delta = json.loads(line[6:]).get("choices", [{}])[0].get("delta", {})
            content += delta.get("content") or ""
        check("stream image (SSE)", code == 200 and "red" in content.lower(),
              f"{code} chunks={len(chunks)} content={content.strip()[:60]}")

        code, txt = post("/api/chat", {"model": "bhag", "stream": False,
                                       "messages": [{"role": "user",
                                                     "content": "What colour is the background square?",
                                                     "images": [png_b64]}]})
        check("ollama /api/chat image", code == 200 and "red" in str(txt).lower(), f"{code} {txt}")

        code, txt = post("/api/generate", {"model": "bhag", "stream": False,
                                           "prompt": "What colour is the background square?",
                                           "images": [png_b64]})
        check("ollama /api/generate image", code == 200 and "red" in str(txt).lower(), f"{code} {txt}")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("--- logged routing decisions ---")
    try:
        con = sqlite3.connect(db)
        for row in con.execute("SELECT protocol, routed_provider, routed_model, has_image,"
                               " has_audio, has_file, stream, status FROM requests ORDER BY id"):
            print("  proto=%-11s %-16s %-30s img=%d aud=%d file=%d stream=%d status=%d" % row)
    except sqlite3.Error as exc:
        print("  db error:", exc)

    print("--- summaries ---")
    for line in open(log):
        if "req=" in line:
            print("  " + line.strip()[:150])

    failed = [name for name, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    print(f"server log: {log}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
