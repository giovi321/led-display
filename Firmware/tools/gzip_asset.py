#!/usr/bin/env python3
"""Gzip one web asset for embedding. Deterministic output (mtime=0) so
rebuilds don't churn the binary. Usage: gzip_asset.py <src> <dst.gz>"""
import gzip
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gzip_asset.py <src> <dst.gz>", file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()
    with open(dst, "wb") as f:
        with gzip.GzipFile(fileobj=f, mode="wb", compresslevel=9, mtime=0) as gz:
            gz.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
