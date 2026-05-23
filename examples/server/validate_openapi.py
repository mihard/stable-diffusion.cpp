#!/usr/bin/env python3
"""Build-time validation: ensures openapi.yaml paths match route registrations in C++ source."""

import re
import sys
import pathlib

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML is required. Install with: pip install pyyaml")
    sys.exit(1)

ROOT = pathlib.Path(__file__).parent

# Routes that exist in the server but are intentionally excluded from the OpenAPI spec
# (infrastructure endpoints, not part of the public API surface)
EXCLUDED_PATHS = frozenset(["/", "/docs", "/openapi.yaml"])

# Route files that register API endpoints (excludes routes_index.cpp and routes_docs.cpp)
ROUTE_FILES = [
    "routes_openai.cpp",
    "routes_sdapi.cpp",
    "routes_sdcpp.cpp",
]

# Mapping from httplib regex path strings to OpenAPI path templates.
# Only needed for parameterised routes; plain string routes are used as-is.
REGEX_TO_TEMPLATE = {
    r"/sdcpp/v1/jobs/([A-Za-z0-9_\-]+)": "/sdcpp/v1/jobs/{id}",
    r"/sdcpp/v1/jobs/([A-Za-z0-9_\-]+)/cancel": "/sdcpp/v1/jobs/{id}/cancel",
}


def load_spec_routes(spec_path: pathlib.Path) -> set[tuple[str, str]]:
    """Return set of (METHOD, /path) from openapi.yaml paths section."""
    spec = yaml.safe_load(spec_path.read_text())
    routes = set()
    http_methods = {"get", "post", "put", "delete", "patch", "head", "options"}
    for path, item in spec.get("paths", {}).items():
        for method in item:
            if method.lower() in http_methods:
                routes.add((method.upper(), path))
    return routes


def load_code_routes(route_files: list[pathlib.Path]) -> set[tuple[str, str]]:
    """Return set of (METHOD, /path) extracted from route registration calls."""
    # Matches: svr.Get("/path", ...) or svr.Post(R"(/regex/path)", ...)
    plain_re = re.compile(
        r'svr\.(Get|Post|Put|Delete|Patch)\s*\(\s*"(/[^"]*)"'
    )
    # Raw string variant: svr.Get(R"(/path)", ...)  or  svr.Get(R"DELIM(/path)DELIM", ...)
    # Use [^"]* for content because raw string paths never contain double-quotes,
    # but may contain ) characters (e.g. capture groups like ([A-Za-z0-9_\-]+)).
    raw_re = re.compile(
        r'svr\.(Get|Post|Put|Delete|Patch)\s*\(\s*R"[A-Za-z_]*\((/[^"]*)\)"'
    )

    routes = set()
    for fpath in route_files:
        content = fpath.read_text()
        for m in plain_re.finditer(content):
            method, path = m.group(1).upper(), m.group(2)
            if path not in EXCLUDED_PATHS:
                routes.add((method, path))
        for m in raw_re.finditer(content):
            method, raw_path = m.group(1).upper(), m.group(2)
            if raw_path in EXCLUDED_PATHS:
                continue
            # Translate regex paths to OpenAPI templates
            path = REGEX_TO_TEMPLATE.get(raw_path, raw_path)
            routes.add((method, path))
    return routes


def main() -> int:
    spec_path = ROOT / "openapi.yaml"
    if not spec_path.exists():
        print(f"ERROR: {spec_path} not found")
        return 1

    route_files = [ROOT / f for f in ROUTE_FILES]
    missing = [f for f in route_files if not f.exists()]
    if missing:
        print(f"ERROR: route file(s) not found: {missing}")
        return 1

    spec_routes = load_spec_routes(spec_path)
    code_routes = load_code_routes(route_files)

    in_spec_not_code = spec_routes - code_routes
    in_code_not_spec = code_routes - spec_routes

    if not in_spec_not_code and not in_code_not_spec:
        print(f"OK: openapi.yaml is in sync ({len(spec_routes)} routes validated)")
        return 0

    print("ERROR: openapi.yaml is out of sync with route registrations!\n")
    if in_spec_not_code:
        print("  In openapi.yaml but NOT registered in C++ source:")
        for method, path in sorted(in_spec_not_code):
            print(f"    {method:6s} {path}")
    if in_code_not_spec:
        print("  Registered in C++ source but NOT in openapi.yaml:")
        for method, path in sorted(in_code_not_spec):
            print(f"    {method:6s} {path}")
    print("\n  Update openapi.yaml to match the registered routes.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
