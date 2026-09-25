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

The worker reads the saved WiFi networks (`wificfg`, see "Saved networks and
join order" below) and `pair.tok/acct/at`. The site is the
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
   Autoapply off posts "App changes waiting" and leaves the batch, unless the
   session was started with `applyWaiting` (the update prompt's "Get them
   now", `runSession(reason, true)` / `CheckinScheduler::checkNow(true)`),
   which applies it this once. That flag is not kept across the restart a
   check takes after Bluetooth use.
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

## Saved networks and join order

Up to 3 networks are saved (`WifiList.h` for the rules and stored keys,
`SavedWifi.h` for the device side; host tests in `test/test_sync_wifilist`).
The portal's WiFi page adds them (a fourth is refused until one is
forgotten), and both the portal and Settings > Saved WiFi offer "Use this
first" and "Forget". The older single-network keys `wificfg.ssid/pass` are
migrated into the list on first read and then kept as a copy of the first
network, so an older image after a return to the previous version still
finds one; a network an older image saved there becomes first.

A power cut in the middle of a save never leaves a mixed list: the list is
kept twice (two copies, each with a checksum written last), a save writes
the copy that is not current and then switches to it with one write, and a
marker (`lsync`) is set for the whole save. While the marker is set the
older keys are not trusted - the list is rebuilt into them at the next read
- so a half-written older pair never overrides the list. A save never
writes over the only whole copy: if the current copy is damaged, the read
uses the other one and the next save writes over the damaged one. Host tests cut a
save after every single write (add, password change, Use this first,
Forget, a remembered join, the migration) and check the next read gives the
whole earlier or the whole new list.

The portal's saved-network requests are read strictly (`WifiRequest.h`):
each request collects its own body, which is wiped as soon as it is read;
the whole body must be one JSON object with only the route's fields, and a
connect must carry `pass` ("" for an open network, as the portal page
sends it).

Every session that needs the station (check-ins of every reason, dev mode's
rejoin, linking, the update session) joins through `SavedWifi::join`:

1. The first network (the last one that worked), at its remembered channel
   and access point, so the connect does not sweep the band. Scheduled
   sessions, a remembered place and more than one saved network all end
   this attempt as soon as the network is reported absent.
2. If that fails: one scan, then a join of the strongest saved network it
   saw. If the radio will not start the scan, a plain join of the first
   network (its own connect looks on every channel) stands in for it. A
   single saved network with no remembered place skips this step - its
   connect already looked everywhere (the scan-less early bail above).
3. A join that worked moves that network first and remembers where it was
   (written only when something changed). A remembered place that led
   nowhere is dropped, so the next session starts with a plain join.

The boot window gives each attempt its 4 s budget; the fallback path may run
past it and its result reaches the status bar like any late boot result.
Every join prints one line (no names, no passwords):

    [wifi] join saved=<n> first=<remembered|plain> first_result=<ok|absent|timeout|stopped> first_ms=.. scan=<0|1|2> scan_starts=.. scan_ms=.. fallback_ms=.. result=.. slot=<i|-1> total_ms=..

(`scan=2`: the scan did not start and the plain join stood in.)

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

## Dev mode listening

`runSession(Reason::Dev)` (started by `lib/UpdatePrompt/AwakeMode` in a dev
mode power cycle) joins the saved network once and stays joined: it runs the
same check-in cycle as any session (check-in, offer, blobs, apply, follow-up)
over and over until it is cancelled. Between check-ins it waits
`devPollWaitMs()` (CloudProtocol, host-tested): the site's `next_poll_ms`
held to 2-30 s, doubled from 2 s up to 60 s after consecutive failures (an
error reply's own `next_poll_ms` when longer), any `Retry-After` up to 5 min,
plus 0-10 % jitter. When the answer shows the site has not taken the
sent mode yet - a 200 naming another mode, or a normal-mode pace (an hour or
more; a 204 has no body) - the next check-in comes after 11 s at most (the
site takes a mode change at most once per 10 s). A stored backoff is waited out inside the loop instead of
failing the start. A dropped connection is rejoined (5 s for the station's
own reconnect, then a fresh join).

- Deliveries apply whatever "Apply app changes automatically" says.
  `devSnapshot()` counts them as soon as the apply commits (before the
  follow-up report), so the running app relaunches at once.
- Every check-in carries `mode` from the stored setting (`AwakePolicy::wireMode`).
- Flash wear: the check-in time is stored only when the site sends its clock
  or every 10 min, and `next_ms` only when it changes. The firmware manifest
  is read at most once an hour, never ahead of an app waiting to be delivered.
- `busy()` stays true for the whole worker (radio apps cancel it, and idle
  sleep is held off, as for any session). `storeBusy()` is true only inside a
  check-in, so serial transfers work between check-ins; a check-in is skipped
  while a serial transfer is open.
- `lib/UpdatePrompt/AwakeMode` cancels the worker while any app outside its
  allow-list is in front (the portal, Music Player, Link, delivered apps,
  Voice Notes ...; see `lib/UpdatePolicy/README.md`) and starts it again
  after.
- A check-in claims the store (`storeBusy()`) before it looks for an open
  serial transfer, and looks again after a rejoin, so a transfer and a
  delivery never overlap.
- The worker ends when the site says the link is gone (401), and
  `devPollNow()` ends a wait early (Check for updates in dev mode shows the
  next check-in's result).
- No "Checking for updates..." status: dev mode has its own marker.

Test builds print one `[dev] poll=...` line per check-in (see
`lib/SerialCli/README.md`); release builds print only `[dev] delivered`,
`[dev] poll failing` and `[dev] poll ok again`.

## Firmware offers

The check-in always offers firmware. The session stores `firmware.url` in
`upd.fw_url` and then - in every session, the scheduled Boot, Daily
(including the headless daily wake, which is what the post-boot popup's
cached result comes from) and Awake ones too - reads the update manifest when
the remaining budget covers one more call (4 s) plus the usual reserve. It is
one call, never a wait, so a scheduled session still never holds WiFi on to
wait. It then runs the install gates (`UpdateSession::refreshOffer`, rules in
`lib/OtaUpdate`): a release that passes is stored in `upd.avail` for the
prompt, a refused one is removed from it, a failed read leaves it. Nothing is
downloaded in a check-in. A 204 check-in carries no offer, so it leaves
`avail` as it was.

Installing is the update session (`UpdateSession.cpp`, described in
`lib/OtaUpdate/README.md`): its own boot after "Install now", station only,
never Bluetooth. It uses `fetchPublic()` here (one GET without the device
credential, the trusted root list, PSRAM TLS via `useExternalTlsMemory()`,
plain HTTP only in test builds, no redirects) and `siteBase()`.

## Tests

The bench case is `test/bench/cases/wifi-pull.json`. It uses the real site
endpoints on a local test-mode server and the test-only `cloud` verbs.
The native tests in `test/test_sync_cloud` cover the planner and the wire
format with fixture bodies (escaped slashes and unicode in `doc`, null
`batch_id`, 204/429 answers, firmware offers, batch mismatch, duplicate hash
prefixes, the server's accepted result strings, backoff retention, and when
"App changes applied" is shown). The host build compiles cJSON 1.7.18 (the
framework's release) from `test/vendor/cJSON`.
