// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
// Device only: the host test builds of this library have no update headers.
#ifndef HOST_TEST
#include "UpdateSigning.h"
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <string.h>

namespace UpdateSigning {
namespace {
struct PublicKey { const char* id; const char* pem; };

// Official release keys (public halves; the private halves are held offline by
// the owner). cf-release-1 signs every official release. cf-backup-1 never
// signs routinely: it exists so that, if the release key is ever lost or
// leaked, one update signed with it can replace or remove the release key.
static const char kReleasePem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEkNa5FE6/L/y22JjvU9TMYwb1LCDz\n"
    "oYzKCUeRsRqI05l0bBJZGh0y0HPq1ISQfddgsqPqcaV4Re4IaA1rsgVOtA==\n"
    "-----END PUBLIC KEY-----\n";
static const char kBackupPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEKPUEcwHD6t/BuCCqMG98VVbcFw3s\n"
    "qiBJrBerD0LZvAsKh2OF/KKSZtXe0JGed6Mea7BhNiDaKd+lFkIHB6MGYA==\n"
    "-----END PUBLIC KEY-----\n";
static const PublicKey kProductionKeys[] = {
    {"cf-release-1", kReleasePem},
    {"cf-backup-1", kBackupPem},
};

#ifdef CF_TEST_CLI
static const char kTestPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEg3gk8q7QDoYn9oBpjp0lJnKgw+tF\n"
    "QEyh09+FYyYFk2TKIsuP1UfGo5UcON9amIaaNDZPyNWfFlpjVWuSCIlmew==\n"
    "-----END PUBLIC KEY-----\n";
static const PublicKey kTestKey = {"test-only-1", kTestPem};
#endif

const PublicKey* find(const char* id) {
    if (!id || !id[0]) return nullptr;
    // Defense in depth: even if a test entry is accidentally copied into the
    // production table, release builds refuse this fixture identifier.
#ifndef CF_TEST_CLI
    if (strcmp(id, "test-only-1") == 0) return nullptr;
#endif
    for (const auto& key : kProductionKeys)
        if (key.id && strcmp(id, key.id) == 0) return &key;
#ifdef CF_TEST_CLI
    if (strcmp(id, kTestKey.id) == 0) return &kTestKey;
#endif
    return nullptr;
}
}  // namespace

bool knownKey(const char* id) { return find(id) != nullptr; }

OtaUpdate::VerifyResult verify(const OtaUpdate::Manifest& m, const uint8_t digest[32]) {
    if (!m.sig[0]) return OtaUpdate::VerifyResult::Unsigned;
    const PublicKey* key = find(m.keyId);
    if (!key) return OtaUpdate::VerifyResult::UnknownKey;
    unsigned char der[78];
    size_t derLen = 0;
    if (mbedtls_base64_decode(der, sizeof(der), &derLen,
                              reinterpret_cast<const unsigned char*>(m.sig), strlen(m.sig)) != 0)
        return OtaUpdate::VerifyResult::Bad;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const int parsed = mbedtls_pk_parse_public_key(&pk,
        reinterpret_cast<const unsigned char*>(key->pem), strlen(key->pem) + 1);
    const int checked = parsed == 0 && mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA) &&
        mbedtls_pk_get_bitlen(&pk) == 256
        ? mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, der, derLen) : -1;
    mbedtls_pk_free(&pk);
    return checked == 0 ? OtaUpdate::VerifyResult::Ok : OtaUpdate::VerifyResult::Bad;
}

#ifdef CF_TEST_CLI
bool verifyTestFixture() {
    OtaUpdate::Manifest m;
    strcpy(m.keyId, "test-only-1");
    strcpy(m.sig, "MEUCIQCHz8IEMOCjIStzZFpynuIjQvTz/HHL8olztrjWbppqKwIgPqA1QWeCpnfALWBulcoTtyg76yhQjj0ShuHib1xV0/4=");
    uint8_t digest[32] = {0xf6,0x65,0xcb,0x67,0x2c,0x96,0xfb,0xec,0x79,0x91,0xe1,0x00,0x9c,0xc8,0xc4,0xf0,
                          0x11,0x3e,0x1c,0x93,0x72,0xc3,0x40,0x9b,0x82,0x83,0x39,0x3a,0x64,0xd3,0xe9,0x7c};
    const bool good = verify(m, digest) == OtaUpdate::VerifyResult::Ok;
    digest[0] ^= 1;
    return good && verify(m, digest) == OtaUpdate::VerifyResult::Bad;
}
#endif
}  // namespace UpdateSigning
#endif
