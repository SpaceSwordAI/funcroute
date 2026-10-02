# funcroute

[![build](https://github.com/SpaceSwordAI/funcroute/actions/workflows/build.yml/badge.svg)](https://github.com/SpaceSwordAI/funcroute/actions/workflows/build.yml)

A small C router that gives a text-only model eyes, ears and a filing cabinet.

Point your client at funcroute instead of at a model vendor, and the cheap
text-only model you already use starts handling screenshots, voice memos and
PDFs. Your client keeps sending one URL and one model name. It never finds out
that a different model answered.

```
your client ──▶ funcroute ──┬──▶ deepseek-v4-flash           (text)
  one URL,                  ├──▶ deepseek/deepseek-v4.1-flash (images, PDFs)
  one model name            └──▶ qwen/qwen3.8-omni-flash      (audio)
```

This is a personal tool that turned out to be reliable enough to publish. It is
not a model, not a fine-tune, and not a merger. It picks an upstream per request
by looking at what is attached to the request, rewrites the `model` field,
forwards it, and translates the answer back. Roughly 4,000 lines of C, five
shared libraries, no vendored code.

**What it isn't:** an inference engine, a proxy for your whole stack, a
load balancer, or a way to make an expensive model cheap. It does not merge
weights, change prompts, or reformat your messages beyond the model field and
the dialect translation. See [Prior art](#prior-art-and-why-this-exists-anyway)
for the grown-up tools that do more.

Contents: [Measured numbers](#measured-numbers) ·
[Prior art](#prior-art-and-why-this-exists-anyway) ·
[Build and run](#build-and-run) · [Configuration](#configuration) ·
[Reproducing these numbers](#reproducing-these-numbers) ·
[Sharp edges](#sharp-edges)

## Why this exists

Multimodal is a configuration tax. Everyone wants it, nobody wants to pay for
it, and every workaround has a taste:

| What I tried | What happened |
| --- | --- |
| Point the client at a frontier omni model | Works perfectly. The bill has a comma in it. |
| Add a second "vision" provider to the client | The client sends one model name, not a routing table. Partway through a conversation it switches back to the text model and 400s on the image still sitting in the history. |
| Use the vision model for everything | Now I am paying vision prices to reformat JSON, and I lost my cheap prefix cache. |
| Use the vendor's own multimodal endpoint | The attachment is `image_url` here, base64 `images[]` there, `input_audio` somewhere else and `file_data` somewhere else again. Every API has its own dialect for "here is a picture". |
| Hardcode the routing into the application | The application is now a router, and I am now the person who maintains a router. |

Underneath all of that is a boring fact: models are specialists. Text models
can't see, vision models can't hear, and the audio model is not going to read
your PDF. "Multimodal" is really "three models and a switchboard", and most
people end up writing the switchboard badly, once per project. This is the
switchboard, written once, in C, with no dependencies you don't already have.

## What it does with each request

1. Walks the message content parts and sorts the attachments by kind:
   `image_url`, `input_audio`, `file`. The Ollama `images[]` array counts as an
   image part, because that is all it is.
2. Picks a provider for that one request. No attachment means the cheap text
   model. Attachments mean the specialist for the most restrictive kind in the
   request, ordered audio > file > image, on the grounds that whatever gets
   picked has to be able to read everything in the body. The omni model reads
   images and PDFs too, so it can take an image and a voice memo at once.
3. Rewrites the `model` field, forwards the request, and translates the reply
   back if the client is speaking Ollama instead of OpenAI.
4. Trims the conversation to fit the configured budget, without ever dropping a
   message that carries an attachment.

Everything else passes through untouched. Tool messages are dropped atomically
when trimming, so an assistant `tool_calls` message never survives without its
run of `tool` results.

## Measured numbers

I generated a PNG with a red background, recorded a 440 Hz WAV, and put a secret
code in a PDF, then pointed each candidate model at all three. Results, not
marketing:

| Model | text | image | audio | file |
| --- | --- | --- | --- | --- |
| `deepseek-v4-flash` (default route) | yes | no | no | no |
| `deepseek/deepseek-v4.1-flash` (DeepSeek VL) | yes | yes | no | yes |
| `qwen/qwen3.8-omni-flash` | yes | yes | yes | yes |

Through the router, on one machine, in one run, one request per row:

| Request | Routed to | Body in | Body out | Wall clock |
| --- | --- | --- | --- | --- |
| text only | deepseek | 107 B | 589 B | 1014 ms |
| 4 KB image | deepseek-vl | 434 B | 2482 B | 1237 ms |
| 600 B PDF | deepseek-vl | 1087 B | 1581 B | 2028 ms |
| 1 s WAV | qwen-omni | 42 953 B | 1879 B | 2471 ms |
| image + WAV | qwen-omni | 43 306 B | 1718 B | 2992 ms |
| image, streamed | deepseek-vl | 451 B | 63 902 B | 1909 ms |
| image via `/api/chat` | deepseek-vl | 326 B | 625 B | 703 ms |
| image via `/api/generate` | deepseek-vl | 293 B | 664 B | 738 ms |

The useful part of that table is the routing column, not the latency. Every
audio-bearing row went to a different vendor than every text-only row, and the
client asked for the same model name throughout.

Two findings shaped the shipped config, both from that test run.

**DeepSeek VL has no ears.** No DeepSeek endpoint takes `input_audio` at all.
OpenRouter answers `404 no endpoints found that support input audio` and the
first-party API rejects the part type outright. Audio needed its own route to an
omni model, which is why routing is per kind rather than one "media provider"
field.

**DeepSeek VL thinks before it sees.** It is a reasoning model, and with
`reasoning: high`, or with the field left unset, it can spend a small client
`max_tokens` entirely on its reasoning trace and hand back empty content. In the
test that was 0 to 2 successes out of 3 depending on the run. At `reasoning: low`
it answered correctly 3 out of 3. Both attachment providers ship at `low`.

## Routing, as shipped

| Request contains | Goes to | Model |
| --- | --- | --- |
| nothing special | DeepSeek | `deepseek-v4-flash` |
| an `image_url` part | OpenRouter | `deepseek/deepseek-v4.1-flash` |
| a `file` part | OpenRouter | `deepseek/deepseek-v4.1-flash` |
| an `input_audio` part | OpenRouter | `qwen/qwen3.8-omni-flash` |

Images and PDFs go through OpenRouter's DeepSeek VL rather than the first-party
DeepSeek API because `api.deepseek.com` wants a pre-uploaded `file_id` before it
will look at a file, and rejects inline base64. Keeping both kinds on the same
upstream also means a mixed request stays with one provider.

Attachments travel inline in the request body as base64 data URLs. funcroute
never opens a file and accepts no multipart uploads. That is on purpose: the
things it routes are already coming off a stream somewhere, so the client has
the bytes before it has a path. If you want a PDF to reach the router by name,
that is your client's problem, and it is four lines of base64 in any language.

## Prior art, and why this exists anyway

There are grown-up projects in this space and you should look at them first.
LiteLLM and Portkey both do provider abstraction with far more, Bifrost and the
Kong, Envoy and Cloudflare gateways are built for teams, and OpenRouter is
already an aggregator with its own routing.

The difference that matters here is *when* the decision happens. Those tools
generally pick an upstream from the model name you asked for, or fall back when
one errors or is rate limited. funcroute ignores the model name on the way in
(it rewrites it) and picks from the content parts in the request body. That is
the whole reason it can sit behind a client that has one model name hardcoded
and no idea what a vision model is.

Things that follow from that being the only goal:

- It is C with five `pkg-config` dependencies, so the binary is one file with no
  runtime. It starts in milliseconds and you can read all of it in a sitting.
- It is not a service, a dashboard, or a config DSL. There is one JSON config.
- It does not do prompt rewriting, semantic routing, caching, budgets, retries,
  or load balancing across keys. If you want those, use something else, and
  consider running it *behind* one of them.
- It speaks both the OpenAI and Ollama dialects, because the two clients I
  actually use do not agree on which one is correct.

## Build and run

```sh
make                      # libcurl, jansson, libmicrohttpd, openssl, sqlite3
cp .env.example .env      # then put DEEPSEEK_API_KEY / OPENROUTER_API_KEY in it
chmod 600 .env
./run.sh
```

`run.sh` sources `.env` and execs the binary. You should see this:

```
funcroute 0.5.0: listening on 127.0.0.1:11434 (endpoint /v1/chat/completions)
  text  -> provider "deepseek"    model "deepseek-v4-flash"
  image -> provider "deepseek-vl" model "deepseek/deepseek-v4.1-flash"
  file  -> provider "deepseek-vl" model "deepseek/deepseek-v4.1-flash"
  audio -> provider "qwen-omni"   model "qwen/qwen3.8-omni-flash"
  attachment part types: image "image_url", audio "input_audio", file "file"
  ollama /api/tags exports model "bhag"
```

Four routes, one port. Point anything OpenAI-shaped or Ollama-shaped at
`http://127.0.0.1:11434` and ask it about a picture.

```sh
# text, handled by DeepSeek
curl -s localhost:11434/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"bhag","messages":[{"role":"user","content":"hello"}]}'

# image, handled by DeepSeek VL
curl -s localhost:11434/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"bhag","messages":[{"role":"user","content":[
        {"type":"text","text":"what is this?"},
        {"type":"image_url","image_url":{"url":"data:image/png;base64,<...>"}}]}]}'

# audio, handled by the omni model
curl -s localhost:11434/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"bhag","messages":[{"role":"user","content":[
        {"type":"text","text":"transcribe this"},
        {"type":"input_audio","input_audio":{"data":"<base64 wav>","format":"wav"}}]}]}'

# a PDF, handled by DeepSeek VL
curl -s localhost:11434/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"bhag","messages":[{"role":"user","content":[
        {"type":"text","text":"summarise this"},
        {"type":"file","file":{"filename":"report.pdf",
         "file_data":"data:application/pdf;base64,<...>"}}]}]}'
```

Same URL, same model name, four different upstream models.

### Dependencies

All external, all via `pkg-config`, nothing hand-rolled:

| Library | What for |
| --- | --- |
| libmicrohttpd | the HTTP server |
| libcurl | talking to the upstreams |
| jansson | reading and rewriting JSON |
| SQLite | the optional request log |
| OpenSSL (EVP) | base64 |
| pthreads | the SSE producer thread |

Builds clean with `-std=c2x -Wall -Wextra -Wpedantic`, no warnings.

## Releases, packaging and CI

Tagged builds are attached to the releases page, one tarball per platform:

| Tarball | Built on | Notes |
| --- | --- | --- |
| `funcroute-<version>-linux-x86_64.tar.gz` | ubuntu-24.04 | links your distro's libcurl, jansson, libmicrohttpd, openssl, sqlite3 |
| `funcroute-<version>-linux-aarch64.tar.gz` | ubuntu-24.04-arm | same |
| `funcroute-<version>-darwin-arm64.tar.gz` | macos-15 | self-contained, the Homebrew dylibs ship in `lib/` |
| `funcroute-<version>-darwin-x86_64.tar.gz` | macos-15-intel | same |

Each tarball unpacks into a single directory holding `funcroute`,
`funcroute-client`, `run.sh`, `config.json`, `.env.example`, the README and the
licence, so `./run.sh` works straight out of the unpack. `SHA256SUMS` covers all
of them.

All four targets are built on native runners, so nothing is cross-compiled or
emulated and no emulation shows up in your timings. The macOS tarballs are the
only ones doing real work: `scripts/package.sh` walks `otool -L`, copies every
non-system dylib into `lib/`, rewrites each reference to `@executable_path/lib/`,
and then fails the build if anything still points at the Homebrew prefix. The
Linux binaries are plain dynamic executables, which is why they are a quarter of
a megabyte instead of 40 MB, and why your distro needs the libraries:

```sh
apt-get install libcurl4 libjansson4 libmicrohttpd12 libssl3 libsqlite3-0  # Debian/Ubuntu
dnf install libcurl jansson libmicrohttpd openssl sqlite                   # Fedora
pacman -S curl jansson libmicrohttpd openssl sqlite                        # Arch
```

Or the old way, from source:

```sh
make VERSION=0.5.0
sudo make install              # /usr/local/bin; override with PREFIX=/usr
```

The version is compiled into both binaries. It shows up in the startup banner
and in `GET /api/version`, so you can tell a release build from a `git describe`
build without guessing. A binary compiled by hand, without `VERSION`, reports
`dev`.

CI runs on every push and pull request: build, `make test`, package, on all four
platforms. That test is offline (mock upstreams, no keys, no network) and asserts
the routed provider and model recorded for each attachment kind, so a routing
regression fails the build instead of a release.

Cutting a release is one command, which builds, tests, tags, pushes, and then
watches the workflow publish the result:

```sh
scripts/release.sh 0.6.0            # or: patch / minor / major
scripts/release.sh patch --dry-run  # show the plan, change nothing
```

Underneath, that is just `git tag v0.6.0 && git push origin v0.6.0`, so pushing a
tag by hand works the same. It refuses to tag a dirty tree or a tag that already
exists, and re-running the workflow on an existing tag replaces the assets.

JSON, read from `config.json` unless you pass a path as the first argument or
set `FUNCROUTE_CONFIG`.

```jsonc
{
  "server":     { "host": "127.0.0.1", "port": 11434, "max_connections": 128 },
  "database":   { "path": "funcroute.db" },   // "" disables persistence

  "providers": {
    "deepseek": {
      "type": "openai",                        // reserved for future use
      "base_url": "https://api.deepseek.com",  // the endpoint path is appended
      "api_key_env": "DEEPSEEK_API_KEY",       // or "api_key": "<literal>"
      "model": "deepseek-v4-flash",
      "reasoning": "high",                     // becomes reasoning_effort
      "timeout_secs": 120
    },
    "deepseek-vl": {
      "type": "openai",
      "base_url": "https://openrouter.ai/api",
      "api_key_env": "OPENROUTER_API_KEY",
      "model": "deepseek/deepseek-v4.1-flash", // eyes: images and PDFs
      "reasoning": "low",                      // low is what keeps content non-empty
      "timeout_secs": 180
    },
    "qwen-omni": {
      "type": "openai",
      "base_url": "https://openrouter.ai/api",
      "api_key_env": "OPENROUTER_API_KEY",
      "model": "qwen/qwen3.8-omni-flash",      // the only route with ears
      "reasoning": "low",
      "timeout_secs": 180
    }
  },

  "routing": {
    "default_provider": "deepseek",      // text-only requests
    "image_provider": "deepseek-vl",     // image_url parts
    "file_provider": "deepseek-vl",      // file parts
    "audio_provider": "qwen-omni",       // input_audio parts
    "media_provider": "deepseek-vl",     // catch-all; empty means image_provider

    "image_content_type": "image_url",   // which part types count as what
    "audio_content_type": "input_audio",
    "file_content_type": "file",

    "endpoint": "/v1/chat/completions",
    "ollama_model": "bhag",              // the name /api/tags hands out
    "max_request_bytes": 131072,         // context budget, 0 turns it off
    "max_messages": 128,
    "advertised_context_length": 1000000
  }
}
```

The attachment routes fall back to each other, specific beats general. Leave
`media_provider` out and it inherits `image_provider`. Leave `file_provider` or
`audio_provider` out and they inherit `media_provider`. Old configs that only
knew about `image_provider` and `media_provider` still start; they just don't get
per-kind routing. If a route names a provider that doesn't exist, that is a
startup error and not a surprise at three in the morning.

### The context budget, or how your history survives a screenshot

Clients resend the entire conversation on every turn, and screenshots and PDFs
are megabytes. Without a cap, one pasted image pushes the whole history out the
window.

- `max_request_bytes` and `max_messages` bound what gets forwarded. The router
  drops the oldest messages until both fit.
- The system prompt stays first and byte-for-byte identical, so DeepSeek's
  automatic prefix caching keeps hitting the part of the prefix that survived.
- The bytes inside an attachment are not counted, and a message carrying an
  attachment is never dropped. Otherwise the base64 for one screenshot would
  look enormous and take the screenshot with it.
- A tool exchange goes together. If an assistant `tool_calls` message is
  dropped, its run of `tool` results goes with it. An orphaned `tool` message
  with no predecessor is dropped regardless.
- All of this applies to `/v1/chat/completions`, `/api/chat` and `/api/generate`.

### Advertised context versus real context

`advertised_context_length` is the number clients show in their context meter,
served through `/v1/models`, `/api/tags` and `/api/show`. It is not a promise
about what actually gets forwarded, since `max_request_bytes` decides that. Set
it high enough that your client doesn't start panicking and trimming before the
router's own budget has had a chance to do anything.

## Running it

```sh
./run.sh                 # source .env, then exec ./funcroute
./run.sh config.json     # explicit config
ENV_FILE=prod.env ./run.sh
LOGDB=off ./run.sh       # or: LOGDB=/var/tmp/funcroute.db
```

`.env` is plain `KEY=VALUE` and is sourced with `set -a`, so you don't need
`export` in it. A missing `.env` is a warning, not a failure. If you would rather
run the binary directly, export `DEEPSEEK_API_KEY` and `OPENROUTER_API_KEY`
yourself and skip the script.

### The request log is optional

SQLite persistence is opt in per run. Command line beats environment beats
`config.json`.

```sh
./funcroute                       # log to database.path
./funcroute --log other.db        # --db is an alias
./funcroute --no-log              # no database opened or created at all
FUNCROUTE_LOG=other.db ./funcroute
FUNCROUTE_NO_LOG=1 ./funcroute
```

Setting `"database": {"path": ""}` turns it off too. Disabled means nothing is
opened and nothing is created, and you still get the one line summaries on
stdout, which are the useful part:

```
[2026-10-02T02:56:29Z] openai /v1/chat/completions model=bhag
  -> deepseek-vl/deepseek/deepseek-v4.1-flash (image) req=434B resp=2482B status=200 dur=1237ms
```

Those `(image)`, `(audio)` and `(file)` markers are there so you can convince
yourself the picture really did go somewhere other than where the text went.

The `requests` table holds `id, ts, protocol, endpoint, requested_model,
routed_provider, routed_model, has_image, has_audio, has_file, stream,
request_size, response_size, status, duration_ms, trimmed_bytes, error,
request_body, response_body`. An older database gets migrated in place with
`ALTER TABLE ADD COLUMN`, so you don't lose it. Request and response bodies are
stored in full by default, which is worth knowing before you log audio.

## What happens when a key is missing

Keys are checked per route at startup, because in practice a key *is* a
capability and not a config field. With only a DeepSeek key you get text, and the
attachment routes announce that they are closed instead of failing somewhere
upstream:

```
  text  -> provider "deepseek" model "deepseek-v4-flash"
  image -> disabled: provider "deepseek-vl" has no API key (set OPENROUTER_API_KEY)
  file  -> disabled: provider "deepseek-vl" has no API key (set OPENROUTER_API_KEY)
  audio -> disabled: provider "qwen-omni" has no API key (set OPENROUTER_API_KEY)
```

A request for a closed route gets `503` naming the variable to set, and the
request log records `routing provider has no API key`, so you find out from the
log and not only from a client error. Requests the open route can serve are
unaffected.

The text route is the exception. Without it there is nothing to fall back to, so
the process refuses to start rather than serving an endpoint that answers 502 to
everything:

```
funcroute: config error: provider "deepseek" (the text route) has no API key;
set DEEPSEEK_API_KEY. Text is the one route this router cannot do without, so it
will not start.
```

## The Ollama API

funcroute speaks Ollama as well, so Ollama-native clients work without changes.

| Route | Method | Notes |
| --- | --- | --- |
| `/api/chat` | POST | translates between OpenAI parts and Ollama `images[]` |
| `/api/generate` | POST | single prompt |
| `/api/tags` | GET | exports `ollama_model`, `bhag` by default |
| `/api/show` | POST | model card plus the advertised context |
| `/api/version`, `/api/ps` | GET | stubs, so probing clients calm down |
| `/api/embeddings` | POST | answers `501 not supported`, honestly |

Streaming is converted in both directions on a producer thread, upstream SSE on
one side and Ollama NDJSON on the other. Reasoning traces are re-surfaced rather
than dropped: DeepSeek's `reasoning_content` and OpenRouter's `reasoning` both
come out as Ollama `message.reasoning`, or a top level `reasoning` for
`/api/generate`. Thinking models keep thinking through the router.

The caveat is that Ollama's API can only express images. Audio and file parts
have nowhere to go, so `funcroute-client` refuses them in `--ollama` mode instead
of quietly throwing your attachment away.

## The two clients in the repo

`funcroute-client` is a dependency-free CLI:

| Flag | Meaning |
| --- | --- |
| `-c/--config`, `-b/--base-url`, `-e/--endpoint`, `-m/--model` | where to talk |
| `-p/--prompt`, `-f/--file` | what to send |
| `-i/--image URL\|PATH` | attach an image, local files become data URLs |
| `-a/--audio PATH`, `-A/--attach PATH` | attach audio, or a file such as a PDF |
| `-s/--stream` | stream tokens as SSE |
| `-o/--ollama` | use `/api/chat` instead |
| `-t/--timeout`, `-h/--help` | the usual |

```sh
./funcroute-client -i screenshot.png -p "what broke?"
./funcroute-client -a memo.wav -p "summarise this"
./funcroute-client -A report.pdf -p "what is the account number?"
./funcroute-client -i chart.png -a note.wav -p "describe both"   # lands on omni
```

`frontend.html` is the one I actually use for poking at it: open it in a browser,
point it at the router, drop in images, audio or PDFs. It classifies each file by
kind, builds the right content part, and shows each one as a chip you can remove
before sending.

## Reproducing these numbers

Everything in the tables above is a script in this repo, so you can check it
rather than trust it. The fixtures are generated byte by byte (no downloads) and
each one has a checkable answer in it: a red square, a 440 Hz tone, and one
secret code.

```sh
python3 test/bench/gen_fixtures.py test/bench/fixtures   # PNG, WAV, PDF + payloads

# which models accept which attachment kind (the capability table)
DEEPSEEK_API_KEY=... OPENROUTER_API_KEY=... \
  python3 test/bench/probe_capabilities.py

# reasoning effort vs empty content (the reasoning: low finding)
OPENROUTER_API_KEY=... python3 test/bench/probe_reasoning.py

# end to end through the router: every row of the measured table
python3 test/bench/e2e_live.py

# offline routing test: mock upstreams, no keys. This is the CI gate.
make test
```

`e2e_live.py` starts the router on a scratch database, sends one request per
attachment kind, streams one of them, exercises both Ollama routes, prints the
logged routing decision for each request, and exits non-zero if anything failed.
It needs real keys and costs a few cents. `probe_capabilities.py` prints the raw
status code and the model's own reply per cell, so a `404` there is evidence,
not my summary of one.

For routing decisions without spending anything, there are mock upstreams:

```sh
python3 test/mock_upstream.py 9101 deepseek &
python3 test/mock_upstream.py 9102 openrouter &
./funcroute test/config.test.json --no-log
```

The mocks echo which provider handled the request and emit provider-native
reasoning fields. `test/config.test.json` mirrors the production routing on port
11434, per-kind routes included.

## Repo layout

```
src/server.c        HTTP server, routing decision, context trimming   (~980 lines)
src/ollama.c        Ollama dialect, NDJSON streaming, format conversion (~1330 lines)
src/client.c        the CLI client                                     (~840 lines)
src/config.c/.h     JSON config, provider resolution, fallbacks        (~360 lines)
src/provider.c/.h   one upstream request, reasoning field handling     (~175 lines)
src/logdb.c/.h      optional SQLite request log                        (~285 lines)
test/routing_test.py    offline routing assertions against mock upstreams
test/mock_upstream.py   mock OpenAI upstream for routing tests
test/bench/*.py         the scripts behind the measured numbers
scripts/package.sh      builds a release tarball for one platform
.github/workflows/      the 4-platform build matrix and the tag-driven release
```

About 4,000 lines of C in total, of which roughly 3,200 is the router and 840 is
the client.

## HTTP surface

| Route | Method | Notes |
| --- | --- | --- |
| `/v1/chat/completions` | POST | OpenAI chat, streaming or not |
| `/v1/models` | GET | OpenAI-style model list |
| `/api/chat`, `/api/generate` | POST | Ollama dialect |
| `/api/tags`, `/api/show`, `/api/version`, `/api/ps`, `/api/embeddings` | varies | the Ollama probing surface |

## Sharp edges

The honest list, in the order you would hit them:

- **Audio has exactly one route.** DeepSeek has no audio input at all, so if
  `qwen-omni` is down, audio is down. Images and PDFs carry on without it.
- **Attachment routes must stay on `reasoning: low`.** A reasoning model can eat
  a small `max_tokens` and return nothing, which looks exactly like a broken
  router. Measured above.
- **Routing is per request, not per conversation.** A conversation that starts
  as text and then pastes a screenshot will be answered by two different models.
  Each request is stateless, so the vision model sees the whole history it needs,
  but do not expect a stable "the model" across a session.
- **Bodies are logged in full by default.** A 43 KB audio request becomes a 43 KB
  database row. Disable the log or trim it if that bothers you.
- **Two upstreams are paid third parties.** The keys go in `.env`, which is
  `chmod 600` and gitignored. Your attachments leave your machine, obviously.
- **Prefix caching only survives where the prefix survives.** Trimming keeps the
  system prompt byte-identical for exactly that reason, so don't reorder messages
  upstream of the router and expect the cache to follow along.
- **The test suite is thin.** `make test` runs `test/routing_test.py`, which
  starts mock upstreams and asserts the routed provider and model recorded for
  every attachment kind. That runs in CI on all four platforms with no keys. The
  live probes in `test/bench` are not run in CI, because they need real accounts
  and cost money; run them yourself if you doubt a number above.
- **This is capability stitching, not a model merger.** The composite only looks
  like one big multimodal model because the router swaps upstreams per request.
  Please don't cite it in a paper.
- **The economics are modest.** Cheapskate the text onto the cheap model and let
  only attachments reach the expensive one. It doesn't make the expensive model
  cheap, it makes it rare.

## License

MIT. See [LICENSE](LICENSE). Use it, fork it, ship it.
