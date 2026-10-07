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

- Several sounds can now play at once. Booper plays one note per held button,
  tuned so any buttons held together make a chord.
- Booper's bottom-right button switches between chord sets: Major, Minor,
  Power, Sus4 and Pentatonic.
- Settings > Sound > Speaker EQ lets you choose how the speaker sounds:
  Balanced, Loud or Off. Your choice is kept through sleep and power-off.
- Apps you make in the App Builder can now play several notes at once and read
  how loud the microphone hears.
- Apps you make can now tell how long a button was held and whether a button
  is down right now.
- Apps you make can use more of standard C++: `<functional>`, `<vector>` and
  `<algorithm>`.
- Settings > Updates has an "About this Fidget" page showing the firmware
  version, build, type, build date and board revision. The Status screen shows
  the firmware version too.
- Check for updates now shows each step as it happens (joining Wi-Fi, checking
  in, looking for updates, getting apps), with a timer and a plain reason if
  something fails.

### Changed

- All sounds now come from a new audio engine. Tones are cleaner, low notes
  are easier to hear on the small speaker, and sounds start and stop without
  clicks.
- The speaker is tuned for how the Cyber Fidget's case shapes its sound, so
  everyday sounds come through louder and clearer.
- An app that needs newer firmware now says so on screen and returns to the
  menu, instead of stopping partway through when it reaches something your
  Fidget can't do yet.
- In the App Builder's emulator, sounds now play exactly as they do on the
  device.

### Fixed

- Short beeps now last as long as asked. They used to run two to three times
  longer.
- Saving the battery history no longer interrupts a sound that is playing.
- The App Builder's emulator now draws exactly like the device, and adding a
  number to text works as it does on the device.

### Removed

## [1.4.1] - 2026-10-03

### Added

- Drawings and animations sent to the device can now play from the Screensavers
  menu.
- Wi-Fi networks can be found, saved, and tested over USB, without opening the
  Web Portal.
- The device can now report its built-in menu over USB, even when no custom
  menu has been saved.

### Changed

- The phone companion now tells incomplete memory card copies, device
  connection problems, and speech download failures apart, with advice for each.

### Fixed

- Menu changes now keep nested categories and leave apps that were not part of
  the change in place.
- Reordering the menu on the device now saves the new order for the next start.
- The battery bar screen now clears the lights when it opens and closes.
- Phone companion buttons now respond while waiting for device status. A late
  reply no longer resets the page you are using or clears work in progress.
- Creating a daily note without a saved provider key now opens the correct
  transcription settings.
- Notes checkboxes now work with the keyboard and screen readers, and Select
  All shows when only some notes are selected.
- Speech downloads now catch badly cut-off files before saving them, and
  transcription can be retried after a failed setup instead of staying stuck.
- Live captions now warn when speech repeatedly returns no text, with advice
  to replace a damaged speech pack.
- Stopping live captions now cancels a start that is still waiting. Text from
  a stopped run is no longer shown, sent to the device, or saved in a later run.

### Removed

## [1.4.0] - 2026-09-30

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

[Unreleased]: https://github.com/CyberFidget/cyberfidget-firmware/compare/v1.4.1...HEAD
[1.4.1]: https://github.com/CyberFidget/cyberfidget-firmware/compare/v1.4.0...v1.4.1
[1.4.0]: https://github.com/CyberFidget/cyberfidget-firmware/compare/v1.3.3-rc2...v1.4.0
