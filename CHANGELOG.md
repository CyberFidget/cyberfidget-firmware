# Changelog

All notable changes to Cyber Fidget firmware are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

**How this file is used.** The release workflow lifts the `[Unreleased]`
section verbatim into the top of the GitHub release notes, then appends the
full commit list below it. So this section is the short, human-written
"what changed for you" summary, and the commit list is the detail.

**Who writes it.** Whoever lands a change adds a line here in the same pull
request. Write for someone holding the device, not for someone reading the
code -- name what the change does, not how it is built. The release manager
tidies the section when cutting the release; the workflow stamps the heading
with the version and date automatically.

## [Unreleased]

### Added

- Battery protection: the device now powers down safely before the battery
  drops to a level that would damage it.
- An always-on battery diary that records run time and charge behavior, plus
  an opt-in soak mode for long unattended tests.
- Live captions on the phone companion now show latency badges, a "falling
  behind" indicator, and a timing summary at the end of a session.
- A Timers app, and a new mode in the Particle sim.
- Ragdoll Fidgie, a physics demo app.
- 3D wireframe models and posable jointed characters are now available to app
  authors.
- The phone companion now ships ready to copy with every release, so you no
  longer have to build it yourself.
- Files can be listed, inspected, and pulled off the device over USB.
- The menu wraps around: pressing Up on the first item jumps to the last, and
  Down on the last jumps back to the first.

### Changed

- The Web Portal and the phone companion now read as one app instead of two
  separate sites, on the new design language, with one shared navigation
  across both device surfaces.
- Live captions default to a faster speech model, and model settings are split
  out so transcription and live captions can be set independently.
- Portal pages are served compressed, reclaiming about 64 KB of app space, and
  about 28 KB of memory was recovered for the portal.
- Custom apps run on their own on-demand task and can now use the graphics
  runtime, including shapes, progress bars, and wrapped text.
- Dino Run and Spaceship now draw from the shared asset pipeline.
- Entering the Web Portal is now a session: it confirms before exiting and
  restarts cleanly on the way out.

### Fixed

- The emulator now draws lines pixel-for-pixel the same as the device.
- Memory card access is owned in one place and parked before sleep.
- Bluetooth: the device restarts into the portal when Bluetooth is active, and
  falls back cleanly when the combined-mode release fails.
- The portal now reports a Wi-Fi bring-up failure instead of silently
  swallowing it.
- Device status no longer errors when no memory card is inserted.
- The remove-download button is hidden when there is nothing downloaded.
- Live captions no longer duplicate partial text or lose their place when the
  backlog fills up.
- The menu highlight no longer drifts out of step with the item that opens.
  Navigating quickly could leave the highlight on the wrong row, or open an app
  other than the one that looked selected.

### Removed

- Dead header generation left over in the app module build.

[Unreleased]: https://github.com/CyberFidget/cyberfidget-firmware/compare/v1.3.2...HEAD
