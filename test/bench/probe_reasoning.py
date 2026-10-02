#!/usr/bin/env python3
"""How reliable is a reasoning model at small max_tokens, per reasoning setting?

DeepSeek VL is a reasoning model. With `reasoning: high` (or the field omitted)
it can spend the client's entire max_tokens on its own trace and return empty
content. This measures that, three runs per setting, and is why both attachment
providers in config.json ship at `reasoning: low`.

    export OPENROUTER_API_KEY=...
    python3 test/bench/gen_fixtures.py test/bench/fixtures
    python3 test/bench/probe_reasoning.py [--reps 3] [--max-tokens 200]
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

URL = "https://openrouter.ai/api/v1/chat/completions"
MODELS = ["deepseek/deepseek-v4.1-flash", "deepseek/deepseek-v4-flash-vision-exp"]
SETTINGS = ["high", "low", None]


def run(fixdir, model, reasoning, max_tokens, reps):
    with open(os.path.join(fixdir, "image.json")) as fh:
        base = json.load(fh)
    out = []
    for _ in range(reps):
        body = json.loads(json.dumps(base))
        body["model"] = model
        body["max_tokens"] = max_tokens
        if reasoning is not None:
            body["reasoning"] = {"effort": reasoning}
        req = urllib.request.Request(
            URL, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + os.environ["OPENROUTER_API_KEY"]})
        started = time.time()
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                data = json.loads(resp.read())
            msg = data["choices"][0]["message"]
            content = (msg.get("content") or "").strip()
            trace = msg.get("reasoning") or ""
            out.append((len(content), len(trace), round(time.time() - started, 1),
                        "red" in content.lower(), content[:34]))
        except urllib.error.HTTPError as exc:
            out.append((0, 0, round(time.time() - started, 1), False, f"HTTP {exc.code}"))
    return sum(1 for o in out if o[3]), out


def main():
    argv = sys.argv[1:]
    fixdir = argv[argv.index("--fixtures") + 1] if "--fixtures" in argv else "test/bench/fixtures"
    reps = int(argv[argv.index("--reps") + 1]) if "--reps" in argv else 3
    max_tokens = int(argv[argv.index("--max-tokens") + 1]) if "--max-tokens" in argv else 200

    if not os.environ.get("OPENROUTER_API_KEY"):
        sys.exit("missing in environment: OPENROUTER_API_KEY")

    print(f"max_tokens={max_tokens} reps={reps}  (a 'hit' means the reply said 'red')")
    for model in MODELS:
        for setting in SETTINGS:
            ok, runs = run(fixdir, model, setting, max_tokens, reps)
            avg = lambda i: sum(r[i] for r in runs) // len(runs)  # noqa: E731
            print(f"{model:38s} reasoning={setting or 'unset':6s} {ok}/{len(runs)} hits  "
                  f"content={avg(0)}B reasoning={avg(1)}B latency={sum(r[2] for r in runs) / len(runs):.1f}s")
            for r in runs:
                print(f"      content={r[0]:4d}B reasoning={r[1]:5d}B {r[2]:5.1f}s :: {r[4]}")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
