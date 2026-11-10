#!/usr/bin/env python3
"""fbrecv.py [PORT] [DIR]: receives the pictures that the phone's /redwood/bin/fbgrab sends (POST /NAME.png) and keeps them in DIR
(default logs/shots). Run it on the Mac before the run; the phone must reach the Mac's address on the same network."""
import http.server, os, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
DIR = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'logs', 'shots')
os.makedirs(DIR, exist_ok=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        name = os.path.basename(self.path) or 'shot.png'
        data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        with open(os.path.join(DIR, name), 'wb') as f:
            f.write(data)
        print('fbrecv: %s, %d bytes' % (name, len(data)), flush=True)
        self.send_response(200)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, *args):
        pass


http.server.HTTPServer(('0.0.0.0', PORT), Handler).serve_forever()
