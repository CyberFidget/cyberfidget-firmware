# trusted-roots

Maintains the device's short trusted root certificate list and checks that
the hosts the device must reach still chain to it.

The device verifies every HTTPS server against the roots in
`lib/TrustedRoots/roots/` instead of the SDK's full certificate bundle
(about 64 KB of flash). If a protected host moves to a certificate authority
that is not on the list, the device can no longer reach it, so the list is
checked against the live hosts by hand and on every release.

Recovery if that ever happens: owners reinstall firmware from the website
over USB, which does not depend on the device's list.

## Files

| Path | What it is |
|---|---|
| `lib/TrustedRoots/roots/*.der` | The root certificates, one DER file each. Single source of truth. |
| `lib/TrustedRoots/roots/roots.json` | Name, SHA-256, download source, where the fingerprint was checked, and which protected host each root serves; plus the protected host list. |
| `lib/TrustedRoots/TrustedRootsData.cpp` | Generated C++ table (committed). Do not edit by hand. |
| `generate.py` | Writes `TrustedRootsData.cpp` from `roots/`. |
| `check_chains.py` | Connects to each protected host trusting ONLY the list. |

Both scripts use the Python standard library only (Python 3.8+); there is
nothing to install.

## Check the protected hosts

```bash
python tools/trusted-roots/check_chains.py
```

```
Trusting 15 roots from lib/TrustedRoots/roots
OK   cyberfidget.com: leaf issued by YE1, TLSv1.2
OK   github.com: leaf issued by Sectigo Public Server Authentication CA DV E36, TLSv1.2
...
6 OK, 0 WARN
```

Each host gets a TLS 1.2 connection (the device speaks TLS 1.2 only) that
trusts only the listed roots: no system store and no fetching of missing
intermediates, which matches the device. `WARN` means the served chain no
longer ends in the list (or the host could not be reached). Exit status is 1
when any host warns.

- `check_chains.py example.com` checks other hosts instead (a host outside
  the list should WARN).
- `--without FILE` leaves a root out, to see which hosts depend on it, e.g.
  `--without sectigo-server-root-e46.der --without usertrust-ecc.der github.com`
  warns.

The release workflow (`.github/workflows/build-release.yml`) runs the check
as a non-blocking step: warnings are printed and annotated on the run, and
the release still publishes.

## Change the list

1. Download the root from its certificate authority (or Mozilla's CA list)
   and save it as DER in `lib/TrustedRoots/roots/`.
2. Check its SHA-256 (`openssl x509 -inform der -in FILE -noout -fingerprint -sha256`)
   against the value the CA or Mozilla's CA list publishes, and add an entry to
   `roots.json` recording the source URL, where you checked it and why it is
   on the list. To remove a root, delete its entry and file.
3. Regenerate and confirm:

   ```bash
   python tools/trusted-roots/generate.py
   python tools/trusted-roots/generate.py --check
   python tools/trusted-roots/check_chains.py
   pio test -e test_core
   ```

`generate.py` refuses to write anything if a file does not match its listed
SHA-256 or is not exactly one DER certificate. `--check` exits 1 when the
committed table is stale.
