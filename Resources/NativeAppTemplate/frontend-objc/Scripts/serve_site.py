#!/usr/bin/env python3

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class OuterframeSiteHandler(SimpleHTTPRequestHandler):
    def guess_type(self, path: str) -> str:
        candidate = Path(path)
        if candidate.suffix == ".outer":
            return "application/vnd.outerframe"
        if candidate.name in {"macos-arm", "macos-x86"}:
            return "application/octet-stream"
        if candidate.name == "index.html" and candidate.parent.name.endswith(".bundle"):
            return "text/plain; charset=utf-8"
        return super().guess_type(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="build/site", help="Directory to serve.")
    parser.add_argument("--port", type=int, default=8025, help="Port to listen on.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    handler = partial(OuterframeSiteHandler, directory=str(root))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"Serving {root} at http://127.0.0.1:{args.port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
