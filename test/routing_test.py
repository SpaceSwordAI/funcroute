#!/usr/bin/env python3
"""Offline routing test: no API keys, no network, deterministic.

Starts two mock OpenAI upstreams and a funcroute instance pointed at them
(test/config.test.json), sends one request per attachment kind, then asserts the
routed provider and model that the router actually recorded in its request log.
That log is the only thing that distinguishes routes which share an upstream,
so the assertion is on the database, not on the reply text.

    python3 test/routing_test.py           # or: make test

Exits non-zero on the first mismatch, so it works as a CI gate.
"""
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PYTHON = sys.executable or "python3"

B64 = base64.b64encode(b"routing-test-fixture-bytes").decode()
IMAGE_PART = {"type": "image_url",
              "image_url": {"url": "data:image/png;base64," + B64}}
AUDIO_PART = {"type": "input_audio",
              "input_audio": {"data": B64, "format": "wav"}}
FILE_PART = {"type": "file",
             "file": {"filename": "note.pdf",
                      "file_data": "data:application/pdf;base64," + B64}}


class Fail(Exception):
    pass


def start(cmd, cwd, log_path, env=None):
    log = open(log_path, "w")
    return subprocess.Popen(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                            env=env), log_path


def wait_for(path, needle, seconds=20):
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(0.2)
        try:
            with open(path) as fh:
                if needle in fh.read():
                    return True
        except OSError:
            pass
    return False


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def post(base, path, body, timeout=30):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")


def chat(text=None, *parts, stream=False):
    content = [{"type": "text", "text": text or "go"}] + list(parts) if parts else (text or "hello")
    return {"model": "bhag", "stream": stream, "max_tokens": 64,
            "messages": [{"role": "user", "content": content}]}


def reply_text(raw, stream=False):
    """Pluck the assistant text out of an OpenAI or Ollama shaped reply."""
    if stream:
        out = ""
        for line in raw.splitlines():
            if line.startswith("data: ") and line[6:].strip() != "[DONE]":
                delta = json.loads(line[6:]).get("choices", [{}])[0].get("delta", {})
                out += delta.get("content") or ""
        return out
    data = json.loads(raw)
    if "response" in data:
        return str(data.get("response") or "")
    msg = data.get("choices", [{}])[0].get("message") or data.get("message") or {}
    return msg.get("content") or ""


def main():
    base = "http://127.0.0.1:11434"
    workdir = tempfile.mkdtemp(prefix="funcroute-test-")
    db = os.path.join(workdir, "routing.db")
    router_log = os.path.join(workdir, "router.log")
    mocks = []
    router = None
    failures = []

    def check(name, ok, detail=""):
        print(f"{'PASS' if ok else 'FAIL'}  {name:34s} {detail[:80]}")
        if not ok:
            failures.append(name)

    try:
        for port, mock_name in (("9101", "deepseek"), ("9102", "openrouter")):
            log = os.path.join(workdir, f"mock-{mock_name}.log")
            mocks.append(start([PYTHON, "test/mock_upstream.py", port, mock_name],
                               ROOT, log))
        time.sleep(0.5)

        router, _ = start([os.path.abspath("./funcroute"), "test/config.test.json",
                           "--log", db], ROOT, router_log)
        if not wait_for(router_log, "listening"):
            raise Fail("router never started:\n" + open(router_log).read())

        # ---- one request per attachment kind, plus the mixed case ----
        want = []  # (label, provider, model, image, audio, file, stream)

        status, raw = post(base, "/v1/chat/completions", chat("hello"))
        check("text", status == 200 and "handled by deepseek" in raw, f"{status}")
        want.append(("text", "deepseek", "deepseek-v4-flash", 0, 0, 0, 0))

        status, raw = post(base, "/v1/chat/completions", chat("what is this?", IMAGE_PART))
        check("image", status == 200 and "handled by openrouter" in raw, f"{status}")
        want.append(("image", "deepseek-vl", "deepseek/deepseek-v4.1-flash", 1, 0, 0, 0))

        status, raw = post(base, "/v1/chat/completions", chat("read this", FILE_PART))
        check("file/PDF", status == 200, f"{status}")
        want.append(("file", "deepseek-vl", "deepseek/deepseek-v4.1-flash", 0, 0, 1, 0))

        status, raw = post(base, "/v1/chat/completions", chat("listen", AUDIO_PART))
        check("audio", status == 200, f"{status}")
        want.append(("audio", "qwen-omni", "qwen/qwen3.8-omni-flash", 0, 1, 0, 0))

        status, raw = post(base, "/v1/chat/completions",
                           chat("both?", IMAGE_PART, AUDIO_PART))
        check("image+audio (audio wins)", status == 200, f"{status}")
        want.append(("mixed", "qwen-omni", "qwen/qwen3.8-omni-flash", 1, 1, 0, 0))

        status, raw = post(base, "/v1/chat/completions", chat("stream please", IMAGE_PART, stream=True))
        streamed = reply_text(raw, stream=True)
        check("stream image (SSE)", status == 200 and "handled by" in streamed, f"{status}")
        want.append(("stream", "deepseek-vl", "deepseek/deepseek-v4.1-flash", 1, 0, 0, 1))

        # ---- Ollama dialect, which can only express images ----
        status, raw = post(base, "/api/chat", {"model": "bhag", "stream": False,
                                              "messages": [{"role": "user",
                                                            "content": "colour?",
                                                            "images": [B64]}]})
        msg = json.loads(raw).get("message", {}) if status == 200 else {}
        check("ollama /api/chat image", status == 200 and "handled by openrouter" in
              (msg.get("content") or ""), f"{status}")
        check("ollama reasoning surfaced", "reasoning" in msg and bool(msg["reasoning"]),
              repr(msg.get("reasoning"))[:60])
        want.append(("ollama-chat", "deepseek-vl", "deepseek/deepseek-v4.1-flash", 1, 0, 0, 0))

        status, raw = post(base, "/api/generate", {"model": "bhag", "stream": False,
                                                  "prompt": "colour?", "images": [B64]})
        check("ollama /api/generate image", status == 200, f"{status}")
        want.append(("ollama-generate", "deepseek-vl", "deepseek/deepseek-v4.1-flash", 1, 0, 0, 0))

        status, raw = post(base, "/api/embeddings", {"model": "bhag", "prompt": "x"})
        check("/api/embeddings is 501", status == 501, f"{status}")

        req = urllib.request.Request(base + "/api/version")
        with urllib.request.urlopen(req, timeout=10) as resp:
            version = json.loads(resp.read()).get("version", "")
        check("/api/version reports a version", bool(version), f"version={version}")

        # ---- the router's own log is the assertion that matters ----
        rows = sqlite3.connect(db).execute(
            "SELECT protocol, routed_provider, routed_model, has_image, has_audio,"
            " has_file, stream FROM requests ORDER BY id").fetchall()
        if len(rows) != len(want):
            check("logged one row per request", False, f"{len(rows)} rows, expected {len(want)}")
        else:
            for (label, prov, model, img, aud, fil, stream), row in zip(want, rows):
                got = (row[1], row[2], row[3], row[4], row[5], row[6])
                check(f"routed: {label}", got == (prov, model, img, aud, fil, stream),
                      f"{got[0]}/{got[1]} img={got[2]} aud={got[3]} file={got[4]} stream={got[5]}")
    except Fail as exc:
        check("harness", False, str(exc))
        print(open(router_log).read() if os.path.exists(router_log) else "")
    finally:
        stop(router)
        for proc, _ in mocks:
            stop(proc)

    print(f"\n{len(failures)} failure(s)")
    if failures:
        for name in failures:
            print("  -", name)
        print(f"router log: {router_log}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
