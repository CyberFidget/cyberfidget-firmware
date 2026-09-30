# TrustedRoots

The root certificates every device HTTPS connection trusts (the check-in and
pull client in `lib/CloudSync`, and the test build's `tlsprobe`). It replaces
the SDK's full certificate bundle, which is no longer linked into the image.

| Root | Why it is on the list |
|---|---|
| ISRG Root X1, X2, YE, YR | cyberfidget.com (Let's Encrypt), objects/release-assets.githubusercontent.com |
| USERTrust ECC, USERTrust RSA | github.com, api.github.com, codeload.github.com (cross-sign the Sectigo roots) |
| Sectigo Public Server Authentication Root E46, R46 | github.com, api.github.com, codeload.github.com |
| GTS Root R1, R3, R4 | backup: Google Trust Services, if the site moves hosts |
| Amazon Root CA 1-4 | backup: Amazon (AWS), if the site moves hosts |

The per-root source, fingerprint and reason live in `roots/roots.json` and in
the comment above each array in `TrustedRootsData.cpp`.

## Use

```cpp
char* roots = TrustedRoots::newPem();   // PSRAM-first; nullptr -> fail the request
if (!roots) return false;
config.cert_pem = roots;                // esp_http_client_config_t
...
esp_http_client_cleanup(client);
TrustedRoots::freePem(roots);           // only after the client is gone
```

The list is stored in flash as DER (about 13 KB) and turned into PEM text
(about 19 KB) in a PSRAM buffer for each connection, because the SDK's
HTTP client parses a CA list only from PEM. esp-tls parses it into the
handshake with mbedTLS's allocator, so it lands wherever that allocator
points (PSRAM once `CloudSync` has set it). Certificate verification stays
mandatory: without a CA list, esp-tls refuses the connection.

`writePem()` and `pemSize()` are plain C++ and covered by
`test/test_core_trustedroots` (`pio test -e test_core`), which also walks each
DER blob and re-hashes it against its recorded SHA-256.

## Changing the list

Edit `roots/` and regenerate with `tools/trusted-roots/generate.py`; see
`tools/trusted-roots/README.md`. `tools/trusted-roots/check_chains.py` reports
whether the protected hosts still chain to the list.
