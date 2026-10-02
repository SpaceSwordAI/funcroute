#!/usr/bin/env python3
"""Minimal mock OpenAI-compatible upstream for smoke-testing funcroute.

Usage: mock_upstream.py <port> <name>
Echoes back which mock handled the request and the model it received.
Emits reasoning traces in the provider's native field:
  deepseek   -> message/delta.reasoning_content
  openrouter -> message/delta.reasoning
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port = int(sys.argv[1])
name = sys.argv[2]
reasoning_key = "reasoning_content" if name == "deepseek" else "reasoning"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            req = json.loads(body)
        except Exception:
            req = {}
        model = req.get("model")
        stream = req.get("stream", False)
        effort = req.get("reasoning_effort")
        has_image = False
        for m in req.get("messages", []):
            content = m.get("content")
            if isinstance(content, list):
                for part in content:
                    if isinstance(part, dict) and part.get("type") == "image_url":
                        has_image = True
                        url = part.get("image_url", {}).get("url", "")
                        print(f"[mock:{name}] image url prefix: {url[:48]}",
                              flush=True)
        msgs = len(req.get("messages", []))
        print(f"[mock:{name}] model={model} stream={stream} image={has_image}"
              f" effort={effort} msgs={msgs} path={self.path}", flush=True)

        if not stream:
            payload = {
                "id": f"chatcmpl-mock-{name}",
                "object": "chat.completion",
                "model": model,
                "choices": [{"index": 0, "message": {
                    "role": "assistant",
                    "content": f"handled by {name}",
                    reasoning_key: f"{name} reasoning trace",
                }, "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 7, "completion_tokens": 3,
                          "total_tokens": 10},
                "mock_name": name,
                "mock_image": has_image,
            }
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        # stream=true -> SSE: reasoning delta first, then content deltas
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        deltas = [{reasoning_key: f"{name} reasoning trace"},
                  {"content": f"handled by {name}"},
                  {"content": " [streamed]"}]
        for d in deltas:
            chunk = {
                "id": f"chatcmpl-mock-{name}",
                "object": "chat.completion.chunk",
                "model": model,
                "choices": [{"index": 0, "delta": d, "finish_reason": None}],
            }
            self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
            self.wfile.flush()
            time.sleep(0.1)
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

    def log_message(self, fmt, *args):
        pass


ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
