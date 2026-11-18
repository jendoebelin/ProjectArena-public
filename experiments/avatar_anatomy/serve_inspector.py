"""Serve the frozen review inspector locally, including losslessly packed datasets.
Run: python experiments/avatar_anatomy/serve_inspector.py --port 8765
Then open http://127.0.0.1:8765. This does not install assets into the game.
"""
import argparse,http.server,functools
from pathlib import Path
ROOT=Path(__file__).resolve().parent/'inspector'
class Handler(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        path=Path(self.translate_path(self.path))
        packed=Path(str(path)+'.gz')
        if path.suffix=='.js' and not path.exists() and packed.is_file():
            handle=packed.open('rb')
            self.send_response(200)
            self.send_header('Content-Type','text/javascript; charset=utf-8')
            self.send_header('Content-Encoding','gzip')
            self.send_header('Content-Length',str(packed.stat().st_size))
            self.end_headers()
            return handle
        return super().send_head()
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',type=int,default=8765);a=p.parse_args()
    server=http.server.ThreadingHTTPServer(('127.0.0.1',a.port),functools.partial(Handler,directory=str(ROOT)))
    print(f'Frozen experimental review at http://127.0.0.1:{a.port}',flush=True)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()
