#!/usr/bin/env python3
"""Ask live upstreams which attachment kinds each model actually accepts.

This is the script behind the capability table in the README. It sends the same
three fixtures (red square, 440 Hz tone, PDF with a secret code) to each
candidate model and prints the status code plus the model's own words, so you
can see for yourself which claims hold.

    export DEEPSEEK_API_KEY=... OPENROUTER_API_KEY=...
    python3 test/bench/gen_fixtures.py test/bench/fixtures
    python3 test/bench/probe_capabilities.py            # whole matrix
    python3 test/bench/probe_capabilities.py audio      # only audio cells

Costs a few cents. Read-only: nothing here writes to your database.
"""
import json
import os
import sys
import urllib.error
import urllib.request

ENDPOINTS = {
    "deepseek": ("https://api.deepseek.com/v1/chat/completions", "DEEPSEEK_API_KEY"),
    "openrouter": ("https://openrouter.ai/api/v1/chat/completions", "OPENROUTER_API_KEY"),
}

# (endpoint, model, [kinds to probe])
MATRIX = [
    ("openrouter", "deepseek/deepseek-v4.1-flash",           ["image", "audio", "file"]),
    ("openrouter", "deepseek/deepseek-v4-flash-vision-exp",  ["image"]),
    ("openrouter", "qwen/qwen3.7-flash",                     ["image", "audio", "file"]),
    ("openrouter", "qwen/qwen3.8-omni-flash",                ["image", "audio", "file"]),
    ("deepseek",   "deepseek-flash",                         ["image", "audio", "file"]),
    ("deepseek",   "deepseek-v4-flash",                      ["image"]),
]


def probe(fixdir, ep, model, kind, timeout=90):
    url, keyenv = ENDPOINTS[ep]
    with open(os.path.join(fixdir, kind + ".json")) as fh:
        body = json.load(fh)
    body["model"] = model
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + os.environ[keyenv]},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read())
        msg = data.get("choices", [{}])[0].get("message", {})
        text = (msg.get("content") or "").strip().replace("\n", " ")
        return resp.status, text[:110] or "(empty)"
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "replace")
        # OpenRouter buries the real reason inside error.metadata.raw
        detail = raw[:110]
        try:
            err = json.loads(raw).get("error")
            if isinstance(err, dict):
                detail = err.get("message") or detail
                meta = (err.get("metadata") or {}).get("raw") or ""
                if meta:
                    inner = json.loads(meta[6:] if meta.startswith("data: ") else meta)
                    detail = inner.get("error", {}).get("message", detail)
        except (ValueError, KeyError, AttributeError):
            pass
        return exc.code, "ERR: " + str(detail).replace("\n", " ")[:110]
    except Exception as exc:  # noqa: BLE001 - report and keep going
        return "---", f"FAIL: {type(exc).__name__}: {exc}"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    fixdir = "test/bench/fixtures"
    if "--fixtures" in sys.argv:
        fixdir = sys.argv[sys.argv.index("--fixtures") + 1]
        args = [a for a in args if a != fixdir]
    only = set(args) or None

    missing = [v for _, v in ENDPOINTS.values() if not os.environ.get(v)]
    if missing:
        sys.exit("missing in environment: " + ", ".join(missing))

    print(f"{'api':11s} {'model':38s} {'kind':6s} {'http':4s} response")
    for ep, model, kinds in MATRIX:
        for kind in kinds:
            if only and kind not in only:
                continue
            code, note = probe(fixdir, ep, model, kind)
            print(f"{ep:11s} {model:38s} {kind:6s} {str(code):4s} {note}")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
