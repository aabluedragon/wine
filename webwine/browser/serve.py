#!/usr/bin/env python3
"""Static server with the cross-origin isolation headers SharedArrayBuffer needs.
The input ring is a SAB (the worker is blocked in the interpreter and cannot
receive postMessage), so COOP/COEP are required - a plain http.server will boot
the game but leave input disabled."""
import http.server, socketserver, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8799

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

print(f"serving {PORT} with COOP/COEP (SharedArrayBuffer enabled)")
S(("", PORT), H).serve_forever()
