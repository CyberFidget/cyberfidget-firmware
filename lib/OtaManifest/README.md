# OtaManifest

This Arduino-free library checks the `hw.min_rev` and `hw.max_rev` strings in
an update manifest against `BoardInfo::Info`. Revisions have the form `M.m`
with decimal digits in each component. Bounds are inclusive and compared as
numbers, major first. Missing, invalid, overflowing, or inverted bounds return
`Malformed`; callers must refuse that result as well as `Incompatible`.

`BoardInfo` supplies the revision, including its reported 1.2 default for
unprogrammed, read-error, and unknown-layout boards. This library never reads
hardware. The release manifest parser and update client can use this gate
when they are added.
