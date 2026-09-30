// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef TRUSTED_ROOTS_H
#define TRUSTED_ROOTS_H

#include <cstddef>
#include <cstdint>

// The root certificates every device HTTPS connection trusts, in place of
// the SDK's full certificate bundle. The table is generated from roots/
// (tools/trusted-roots/generate.py); see README.md for why each root is on
// the list and how to change it.
namespace TrustedRoots {

struct Root {
    const char* name;
    const uint8_t* der;
    size_t derLen;
    const char* sha256;  // lowercase hex SHA-256 of der, from roots/roots.json
};

extern const Root kRoots[];
extern const size_t kRootCount;

// Bytes writePem() needs for the whole list, terminating NUL included.
size_t pemSize();

// Writes every root as a PEM block (64-column base64) followed by a NUL.
// Returns the text length without the NUL, or 0 when cap < pemSize().
size_t writePem(char* out, size_t cap);

// Certificates actually parsed into an mbedTLS chain (mbedtls_x509_crt nodes
// linked by next; a node that holds no certificate has version 0). A
// template so the host test can walk a stand-in struct.
template <typename Crt>
size_t chainCount(const Crt* crt) {
    size_t n = 0;
    for (; crt; crt = crt->next) {
        if (crt->version != 0) ++n;
    }
    return n;
}

// True when the parse of the whole list returned 0 and yielded exactly
// kRootCount certificates. esp-tls accepts a positive (partial) parse result,
// which would silently trust fewer roots, so check before every connection.
bool parseComplete(int parseResult, size_t parsedCount);

#ifndef HOST_TEST
// Parses pem into a temporary chain (mbedTLS allocator) and frees it again.
// Returns parseComplete() of the result; never connect when this is false.
bool pemParsesCompletely(const char* pem);

// The whole list as PEM text for esp_http_client_config_t::cert_pem, in a
// PSRAM-first buffer. The buffer must outlive the HTTP client that uses it:
// free it with freePem() after esp_http_client_cleanup(). Returns nullptr
// when no buffer could be allocated; the caller must then fail the request.
char* newPem();
void freePem(char* pem);
#endif

}  // namespace TrustedRoots

#endif  // TRUSTED_ROOTS_H
