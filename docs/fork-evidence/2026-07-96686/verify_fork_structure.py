#!/usr/bin/env python3
"""Verify the parent relationship of the published raw DeFCoN headers."""

import json
from pathlib import Path


PACKAGE_DIR = Path(__file__).resolve().parent
HEADERS_FILE = PACKAGE_DIR / "rpc" / "block-headers.json"


def previous_hash(raw_header_hex: str) -> str:
    raw_header = bytes.fromhex(raw_header_hex)
    if len(raw_header) != 80:
        raise ValueError(f"expected an 80-byte header, got {len(raw_header)} bytes")
    return raw_header[4:36][::-1].hex()


def main() -> None:
    document = json.loads(HEADERS_FILE.read_text(encoding="utf-8"))
    headers = document["headers"]

    for header in headers.values():
        if len(bytes.fromhex(header["rawHeaderHex"])) != 80:
            raise SystemExit(f"{header['hash']}: raw header is not 80 bytes")

    common_hash = headers["commonAncestor96685"]["hash"]
    for name in ("projectDesignatedCanonical96686", "competingHistorical96686"):
        parent = previous_hash(headers[name]["rawHeaderHex"])
        if parent != common_hash:
            raise SystemExit(f"{name}: expected parent {common_hash}, got {parent}")

    if headers["projectDesignatedCanonical96686"]["hash"] == headers["competingHistorical96686"]["hash"]:
        raise SystemExit("height-96686 headers unexpectedly have the same hash")

    print(
        "Verified 4 raw block headers; both height-96686 blocks share parent 96685."
    )


if __name__ == "__main__":
    main()
