<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
# Public test signing fixture

`TEST-ONLY-public.pem` is the public half of a throwaway test key, compiled
only by `CF_TEST_CLI` builds as `test-only-1`; a release build always refuses
that id. Its private half is kept out of the repository (a private key in a
public repository trips secret scanning even when it is worthless). If you
do not have it, make a new test pair and replace the compiled test key:

```sh
openssl ecparam -name prime256v1 -genkey -noout -out TEST-ONLY-private.pem
openssl ec -in TEST-ONLY-private.pem -pubout -out TEST-ONLY-public.pem
```

Then re-sign `TEST-ONLY-image.bin` (below) and update the test public key and
digest in `lib/CloudSync/UpdateSigning.cpp`. Never use a test key as
`FIRMWARE_SIGNING_KEY` or in the production key table.

`TEST-ONLY-image.bin` is a short synthetic byte string for `upd verify-test`.
It is **not** an ESP32 app image and cannot be installed. The paired `.sig`
is base64 DER; `.sig.der` is the raw signature. A sample manifest shows the
field shape, not an installable release.

For a real local test site, build a test firmware image and sign its **exact**
`firmware.bin` bytes with the test private key:

```sh
openssl dgst -sha256 -sign TEST-ONLY-private.pem \
  -out firmware.bin.sig.der firmware.bin
openssl base64 -A -in firmware.bin.sig.der -out firmware.bin.sig
printf %s test-only-1 > firmware.bin.sig.keyid
sha256sum firmware.bin
```

Serve the same `firmware.bin` from the manifest's URL. Set `size` and
`sha256` from that file, paste the `.sig` text into `sig`, and set `key_id`
to `test-only-1`. Use `local_test`, the LAN `upd.base` override, and the
normal update offer/Install now flow. A release build always rejects this id.
