# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC
"""Check that each protected host's live certificate chain ends in the
device's trusted root list (lib/TrustedRoots/roots/).

Each host is contacted with TLS that trusts ONLY those roots - no system
store, no fetching of missing intermediates - which is what the device does.
A host whose served chain does not verify is reported as WARN.

    python tools/trusted-roots/check_chains.py                 # protected hosts
    python tools/trusted-roots/check_chains.py example.com     # other hosts
    python tools/trusted-roots/check_chains.py --without isrg-root-x1.der

Exit status is 0 when every host is OK, 1 when any host warns. Under GitHub
Actions each warning is also printed as a ::warning:: annotation.
Standard library only.
"""

import argparse
import json
import os
import pathlib
import socket
import ssl
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
ROOTS_DIR = REPO / "lib" / "TrustedRoots" / "roots"


def load_manifest():
    with open(ROOTS_DIR / "roots.json", encoding="utf-8") as f:
        return json.load(f)


def trust_context(roots):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)  # verifies chain + hostname
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2    # the device speaks TLS 1.2 only
    for entry in roots:
        ctx.load_verify_locations(cadata=(ROOTS_DIR / entry["file"]).read_bytes())
    return ctx


def check(host, port, ctx, timeout):
    """(ok, detail). detail names the leaf issuer on success, else the error."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            with ctx.wrap_socket(sock, server_hostname=host) as tls:
                cert = tls.getpeercert()
                issuer = dict(item[0] for item in cert.get("issuer", ()))
                return True, f"leaf issued by {issuer.get('commonName', '?')}, {tls.version()}"
    except ssl.SSLCertVerificationError as e:
        return False, f"chain does not end in the list: {e.verify_message}"
    except (OSError, ssl.SSLError) as e:
        return False, f"could not connect: {e}"


def main():
    manifest = load_manifest()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("hosts", nargs="*",
                        help="hosts to check (default: the protected hosts in roots.json)")
    parser.add_argument("--without", action="append", default=[], metavar="FILE",
                        help="leave this root file out, to see which hosts depend on it")
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    files = {entry["file"] for entry in manifest["roots"]}
    unknown = [f for f in args.without if f not in files]
    if unknown:
        parser.error(f"not in roots.json: {', '.join(unknown)}")
    roots = [entry for entry in manifest["roots"] if entry["file"] not in args.without]
    hosts = args.hosts or manifest["protected_hosts"]
    ctx = trust_context(roots)
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"

    print(f"Trusting {len(roots)} roots from {ROOTS_DIR.relative_to(REPO)}"
          + (f" (without {', '.join(args.without)})" if args.without else ""))
    warnings = 0
    for host in hosts:
        ok, detail = check(host, args.port, ctx, args.timeout)
        print(f"{'OK  ' if ok else 'WARN'} {host}: {detail}")
        if not ok:
            warnings += 1
            if annotate:
                print(f"::warning title=Trusted root list::{host}: {detail}")
    print(f"{len(hosts) - warnings} OK, {warnings} WARN")
    return 1 if warnings else 0


if __name__ == "__main__":
    sys.exit(main())
