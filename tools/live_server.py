"""Serve the live view (tools/live.html) and take button presses from it.

    python tools/live_server.py [--port 8765] [--keys out/game/keys.txt]

Then run the game with `--live out/game/live.bmp --input out/game/keys.txt`
and open http://localhost:8765/tools/live.html?src=/out/game/live.bmp.

GET serves files from the repo folder. POST /keys takes a body of
"KEYS [X Y]" (held keys in hex, libctru's KEY_* bits; X Y: a touch on the
bottom screen) and writes it to the keys file, which the runner reads every
frame. Only listens on 127.0.0.1.
"""
import argparse
import functools
import os
import re
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BODY = re.compile(rb'[0-9a-fA-F]{1,8}( [0-9]{1,3} [0-9]{1,3})?')


class Handler(SimpleHTTPRequestHandler):
    keys_path = None

    def do_POST(self):
        if self.path != '/keys':
            self.send_error(404)
            return
        body = self.rfile.read(min(int(self.headers.get('Content-Length', 0)), 64)).strip()
        if not BODY.fullmatch(body):
            self.send_error(400)
            return
        tmp = self.keys_path + '.tmp'
        with open(tmp, 'wb') as f:     # written whole, then renamed: never read half-written
            f.write(body + b'\n')
        os.replace(tmp, self.keys_path)
        self.send_response(204)
        self.end_headers()

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def log_message(self, *args):
        pass                            # quiet: the page polls the picture several times a second


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--port', type=int, default=8765)
    ap.add_argument('--keys', default=os.path.join(ROOT, 'out', 'game', 'keys.txt'))
    args = ap.parse_args()
    Handler.keys_path = os.path.abspath(args.keys)
    server = ThreadingHTTPServer(('127.0.0.1', args.port),
                                 functools.partial(Handler, directory=ROOT))
    print(f'live view: http://localhost:{args.port}/tools/live.html?src=/out/game/live.bmp')
    print(f'buttons go to {Handler.keys_path}')
    server.serve_forever()


if __name__ == '__main__':
    main()
