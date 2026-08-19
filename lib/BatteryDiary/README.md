# BatteryDiary

BatteryDiary keeps a bounded battery and usage history in LittleFS and carries
hourly deep-sleep check-ins through RTC slow memory without mounting flash on
the normal timer-wake path.

## Storage

The files live at `/apps/.diary/batdiary.bin` and
`/apps/.diary/batstats.bin`. This location is intentional: the existing sync
confinement permits `flist /apps/.diary` and `fread` below `/apps/`, so a
browser can retrieve both files without a diary-specific binary transport or
a wider filesystem boundary.

`batdiary.bin` contains little-endian 16-byte records:

```text
u32 seq, u32 uptime_or_count, s16 vcell_mv, u8 soc_half_pct,
s8 crate_qtr_pct_hr, u8 event, u8 boot_count_lo, u16 crc16
```

The CRC-16/CCITT covers the first 14 bytes. SOC units are 0.5 percent and
charge-rate units are 0.25 percent/hour. The event values are BOOT=1,
TIMER_CHECKIN=2, AWAKE_SAMPLE=3, SLEEP_ENTER=4,
SHUTDOWN_SLEEPSIDE=5, SHUTDOWN_RUNTIME=6, and FLUSH_MARKER=7. BOOT uses
`uptime_or_count` for the wake cause: power-on=0, ext0=1, ext1=2, timer=3,
touch=4, ULP=5, other=6. TIMER_CHECKIN uses the cumulative check-in count;
awake events use `millis()`. FLUSH_MARKER uses the launched app index so soak
runs can be segmented.

The ring caps at 3072 records (48 KiB). When the next append would exceed the
cap, it rewrites the file without the oldest 1024 records. Stats are a
CRC-protected schema-1 structure containing the nine documented `uint32_t`
counters and extrema followed by a `uint16_t` CRC.

## Sampling and RTC behavior

Awake gauge ticks arrive every 200 ms. On-time accumulates from those ticks;
the first AWAKE_SAMPLE is emitted after about 30 seconds and later samples
every five minutes. Hourly timer wakes append only the already-read voltage
and the check-in count to a 384-record RTC ring (6144 bytes of records plus
28 bytes of metadata). SOC and charge rate are zero on this fast path so it
does not add more gauge transactions. A timer wake mounts LittleFS only when
the RTC ring reaches 24 records (roughly daily at the hourly cadence) or when
a sleep-side shutdown verdict must be made durable. The daily cadence exists
because RTC memory does not survive an EN-line reset, and both plugging in
USB and a host opening the serial port can pulse EN through the auto-reset
circuit - so the un-flushed window is capped at about a day of check-ins. The
timer-path mount never formats on failure; a failed mount leaves records in
the RTC ring for the next attempt, and overflow drops are counted honestly in
the stats block.

Normal boots flush retained RTC records after `LoadoutStore` mounts LittleFS.
Runtime shutdown records are appended and flushed before display teardown.
SLEEP_ENTER stays in RTC memory and rides the next normal flush.

## Charge-cycle approximation

A cycle is counted after charge rate remains positive for at least ten minutes
of five-minute awake samples and then remains negative for at least ten
minutes. A zero sample or a sign change before qualification resets that
side's duration, so jitter around zero does not count. If the first awake
sample after boot is positive, the detector treats the positive side as
qualified; this approximates charging that crossed a reboot. Detector phase
is not written into the fixed stats schema, so an unrelated reboot during an
unqualified side can lose that partial side.

## CLI

`diary` prints stats, up to the last eight records, and a `diary.done` line.
`diary clear` truncates the records and resets all stats except lifetime boot
count and accumulated on-time.
