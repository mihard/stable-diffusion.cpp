#!/usr/bin/env python3
"""Generate a C++ header that embeds openapi.yaml as a string literal.

Usage: gen_openapi_yaml_h.py <input.yaml> <output.h>
"""

import sys
import pathlib

# C++ raw string delimiters may be at most 16 characters.
DELIM = "OPENAPI_SPEC"


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.yaml> <output.h>")
        return 1

    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])

    content = src.read_text()

    # Guard against the raw string delimiter appearing in the YAML content
    terminator = f"){DELIM}\""
    if terminator in content:
        print(f"ERROR: YAML content contains the raw string terminator {terminator!r}")
        print("Change DELIM in this script to a unique identifier.")
        return 1

    dst.parent.mkdir(parents=True, exist_ok=True)
    # The content is placed immediately after the opening ( with no extra newline,
    # so the YAML starts at column 0 as expected.
    dst.write_text(
        f'// Auto-generated from {src.name} -- do not edit directly.\n'
        f'static const char OPENAPI_YAML[] = R"{DELIM}({content}){DELIM}";\n'
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
