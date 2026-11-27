#!/usr/bin/env python3
"""Minimal HTTP lifecycle sentinel for an ephemeral CI Buildbarn cluster."""

import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class ClusterState:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._started = time.monotonic()
        self._done = False
        self._workers: dict[str, float] = {}

    def is_done(self) -> bool:
        with self._lock:
            return self._done

    def mark_done(self) -> None:
        with self._lock:
            self._done = True

    def record_worker(self, name: str) -> bool:
        now = time.monotonic()
        with self._lock:
            is_new = name not in self._workers
            self._workers[name] = now
            return is_new

    def snapshot(self) -> dict[str, object]:
        now = time.monotonic()
        with self._lock:
            return {
                'done': self._done,
                'uptime_s': int(now - self._started),
                'workers': sorted(self._workers),
            }


class SentinelServer(ThreadingHTTPServer):
    def __init__(self, server_address: tuple[str, int], state: ClusterState) -> None:
        self.state = state
        super().__init__(server_address, SentinelHandler)


class SentinelHandler(BaseHTTPRequestHandler):
    server: SentinelServer

    def _send(self, code: int, body: bytes, content_type: str = 'text/plain') -> None:
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == '/health':
            self._send(200, b'ok\n')
        elif parsed.path == '/done':
            if self.server.state.is_done():
                self._send(200, b'done\n')
            else:
                self._send(202, b'running\n')
        elif parsed.path == '/status':
            payload = json.dumps(self.server.state.snapshot()).encode('utf-8') + b'\n'
            self._send(200, payload, 'application/json')
        else:
            self._send(404, b'not found\n')

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == '/done':
            if self.client_address[0] not in {'127.0.0.1', '::1'}:
                self._send(403, b'loopback only\n')
                return
            self.server.state.mark_done()
            print('sentinel: coordinator marked build done', file=sys.stderr, flush=True)
            self._send(200, b'done\n')
            return
        if parsed.path == '/heartbeat':
            params = parse_qs(parsed.query)
            worker = params.get('worker', ['unknown'])[0][:64]
            if self.server.state.record_worker(worker):
                print(f'sentinel: worker connected: {worker}', file=sys.stderr, flush=True)
            self._send(200, b'ok\n')
            return
        self._send(404, b'not found\n')

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002, ARG002
        return


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8999)
    args = parser.parse_args()

    server = SentinelServer((args.bind, args.port), ClusterState())
    print(f'sentinel: listening on {args.bind}:{args.port}', file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
