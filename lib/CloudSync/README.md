# CloudSync

`runSession(reason)` starts one STA-only worker on a plain FreeRTOS task. The
main loop calls `poll()`, which returns true when a check-in session has
just finished (`lastResult()` then holds it); `consumeResult()` hands the
same result to one other reader (the test CLI). The same entry point serves
the boot-window, daily, awake, manual and development-mode schedulers; when
each may start is decided in `lib/UpdatePolicy`. A Bluetooth-tainted entry writes the `bootcfg.bootcloud`
one-shot and restarts; `AppManager` consumes it at boot. A session never
starts while the Music Player or the portal is the active app.

## Radio apps

`AppManager::switchToApp` treats the Music Player and the portal as radio
apps. Before either starts it shows "Finishing check..." if a session is
running, cancels it and waits for WiFi to be off. When the session does not
stop in time, or when the Music Player is launched after a session has used
WiFi in this power cycle, the device restarts with a one-shot
(`bootcfg.bootmusic` or the portal's `bootcfg.bootapp`, plus `skipanim`) that
relaunches the app in a clean power cycle. The session only switches WiFi off
if it switched it on.

## Credentials and endpoints

The worker reads `wificfg.ssid/pass` and `pair.tok/acct/at`. The site is the
compiled `https://cyberfidget.com`; test builds (`CF_TEST_CLI`) read an
`upd.base` override and permit an HTTP LAN server. Release builds use HTTPS,
and the server's chain must end in the short trusted root list
(`lib/TrustedRoots`), not the SDK's full certificate bundle, which is no
longer linked. Each request builds the list's PEM text in PSRAM and frees it
after the client is cleaned up. The device credential is sent only in the
Authorization header, is never logged, and its header buffer is wiped after
use. mbedTLS's allocator is set to PSRAM-first before the first handshake.

## Session

`CloudProtocol` (host-compiled, tested in `test/test_sync_cloud`) parses the
check-in and loadout answers, builds the check-in body, and picks waits.
`CloudPlanner` decides each step; the worker acts on every step it returns.

1. Check-in: device id, firmware, ABI, board revision, LittleFS totals,
   `lapply_cap=batch1`, manifest CRC, installed entries, and the last applied
   batch record when it belongs to the current pair identity.
2. A 200 with `batch_id` and `upd.autoapply` on (default) fetches the offer.
   Autoapply off posts "App changes waiting" and leaves the batch.
3. The offer's `doc` bytes (after JSON un-escaping) must hash to `doc_crc` and
   carry the check-in's batch id and a base. Every blob row must be a
   same-origin URL with a SHA-256 and size, hash prefixes must be unique, and
   each blob must be named by a confined document path ending
   `-<sha8>.wasm`.
4. If the applied record already holds this batch and document CRC, nothing
   is downloaded and the answer is `already-applied`.
5. A target that already exists with the offered size and SHA-256 is not
   downloaded. Otherwise one GET checks SHA-256 and computes the CRC
   FerrySession needs at open, then one GET per destination streams into
   FerrySession chunks and is hashed again before `fwcommit`.
6. `lapply` gets the unchanged document with the server's CRC, holding the
   `LoadoutStore` lock that the menu reorder also takes.
7. A follow-up check-in carries `applied_batch` + `result`.

Permanent defects are answered as `rejected:<reason>` so they never wedge the
queue: `offer-crc`, `offer-batch`, `blob-offer`, `blob-prefix`, `blob-path`,
`blob-target`, `blob-sha` (the SHA failed on two reads), `blob-missing` (a
document path nothing provides), plus `rejected:apply` / `rejected:record` and
`stale-revision` from `lapply`. Transport failures stay retryable and are not
answered.

## Scheduled sessions (Boot, Daily, Awake)

A scheduled session never holds WiFi on to wait: a follow-up report that
would have to wait out the server's floor, and any Retry-After, is deferred
to the next session (`report-deferred` / `rate-limited`), exactly as a
manual session does when its budget runs out. Its budget is 60 s instead of
150 s. Manual, Dev and Recovery sessions keep the waiting behaviour below.

Joining: the boot window allows 4 s (the start-up animation is 5 s;
measured join + first TLS open on home WiFi was 3.2-3.8 s, so a slow join
simply gives the cached result); Daily and Awake keep 10 s. Every scheduled
session bails as soon as the station reports the saved network absent
(`WL_NO_SSID_AVAIL`, error `no-network`). That is the connect's own scan,
not an extra scan before connecting: it costs nothing when the network is
present, where a separate scan-first pass would add a full channel sweep
(~1-2 s) to every successful join inside the 4 s window. Measured on
HIL-A (2026-09, LAN test site over plain HTTP, strong home network): with
the saved network absent the boot-window session ended `no-network` at
total 2.6 s (about 0.7 s of that is set-up before the join starts, as in
every session), with WiFi off; with it present, joins took 1.1-1.6 s.
Those joins do not include a TLS handshake (the production-site join +
TLS open was 3.2-3.8 s in the earlier TLS measurement).

Every check-in session prints one read-only line when it ends, before it
counts as finished (so it precedes anything that waited for it, such as a
Music Player start):

    [checkin] reason=<boot|daily|awake|manual|dev|recovery> join_ms=.. total_ms=.. result=<ok|none|error> err=.. heap_min=.. largest_min=.. wifi=<off|on>

`heap_min` / `largest_min` are the internal-heap low-water marks of the
free total and the largest free block during the session; `wifi` is the
radio mode after the worker's shutdown. The credential is never printed.

## Waits and deadline

The follow-up waits for the server's floor: 61 s normally, 3 s when the
check-in's `mode` or `next_poll_ms` says the device is in a 2 s dev mode. One
429 per session is retried after its `Retry-After`. A wait runs only when the
remaining 150 s budget also covers a request after it; otherwise the answer is
deferred (`report-deferred`). An applied batch is then reported from its
record on the next session's first check-in; a rejection is found again when
the batch is re-offered. `next_poll_ms`, `Retry-After`, and the last check-in
time are retained in `upd` for schedulers. Any reply carrying `Retry-After`
(a 429, or a 503 the site sends with 60 or 3600) stores a backoff; only a
200/204 clears it, and other replies leave it in place.

The worker is not registered with the task watchdog: a TLS open can block
longer than its period. The per-call timeout, the session deadline and
cancellation bound every wait instead. The session runs in a helper so all
its heap objects are destroyed before the task deletes itself.

"App changes applied" is posted only when this session's apply changed the
menu, not for an `already-applied` answer.

A running session holds off the device's 60 s idle sleep (`AppManager::loop`
refreshes the interaction time while `busy()`), so the idle period restarts
when the session ends. Without this, a session that waits out the 61 s
follow-up floor was put into deep sleep mid-session.

WiFi is turned fully off at every worker exit. Only a radio that failed to
switch off restarts the device (error delivered after boot); a passed
deadline with the radio off is an ordinary error.

## Firmware offers

The check-in always offers firmware. The session stores `firmware.url` in
`upd.fw_url` for the update lane and posts nothing; comparing against the
firmware manifest and asking the user belong to that lane. This library never
installs firmware.

## Tests

The bench case is `test/bench/cases/wifi-pull.json`. It uses the real site
endpoints on a local test-mode server and the test-only `cloud` verbs.
The native tests in `test/test_sync_cloud` cover the planner and the wire
format with fixture bodies (escaped slashes and unicode in `doc`, null
`batch_id`, 204/429 answers, firmware offers, batch mismatch, duplicate hash
prefixes, the server's accepted result strings, backoff retention, and when
"App changes applied" is shown). The host build compiles cJSON 1.7.18 (the
framework's release) from `test/vendor/cJSON`.
