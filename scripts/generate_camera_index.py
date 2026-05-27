#!/usr/bin/env python3
import gzip
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HTML = ROOT / "web" / "index.html"
OUT = ROOT / "src" / "camera_index.h"


def c_array(data):
    lines = []
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i : i + 16])
        lines.append(f" {chunk},")
    return "\n".join(lines)


def main():
    html = HTML.read_bytes()
    gz = gzip.compress(html, compresslevel=9, mtime=0)
    arr = c_array(gz)
    text = f"""#pragma once
#include <stdint.h>

// Generated from web/index.html by scripts/generate_camera_index.py.
#define index_ov2640_html_gz_len {len(gz)}
#define index_ov3660_html_gz_len {len(gz)}
#define index_ov5640_html_gz_len {len(gz)}

const uint8_t index_ov2640_html_gz[] = {{
{arr}
}};

const uint8_t index_ov3660_html_gz[] = {{
{arr}
}};

const uint8_t index_ov5640_html_gz[] = {{
{arr}
}};
"""
    OUT.write_text(text)
    print(f"Wrote {OUT} ({len(html)} bytes html, {len(gz)} bytes gzip)")


if __name__ == "__main__":
    main()
