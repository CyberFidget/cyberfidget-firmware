# MenuManager

MenuManager draws and drives the main app menu (categories, wrap-around
highlight, cross-slide transitions, long-press reorder). The library also
holds two small UI primitives other screens share: `ModalPrompt` and
`ScrollLabel`.

## ModalPrompt

A full-screen question with a title and one or more options, answered with
Up / Down / Enter:

```text
+------------------------------+
|        Update 1.4.0 ready    |  <- inverted title bar
|##Install now#################|  <- focused row (filled)
|  Remind me later             |
|  Skip this version           |
+------------------------------+
```

```cpp
#include "ModalPrompt.h"

static void onAnswer(int result) {
    if (result == ModalPrompt::kNoChoice) { /* timed out */ }
    else                                  { /* result is the zero-based option */ }
}

const char* const opts[] = { "Install now", "Remind me later", "Skip this version" };
ModalPrompt::instance().open("Update 1.4.0 ready", opts, 3, onAnswer);        // no timeout
ModalPrompt::instance().open("Update 1.4.0 ready", opts, 3, onAnswer, 30000); // 30 s idle timeout
```

Behavior:

* **Result only.** The prompt reports the chosen index, or `kNoChoice` when
  its timeout expires. What an answer means, and anything that gets stored
  because of it, belongs to the caller. The prompt has no built-in strings.
* **Menu only.** A prompt can only open while the menu (or the boot screen
  that hands over to it) is the active app; `open()` returns false anywhere
  else. The prompt pauses the app underneath, and only the menu is written to
  be paused that way. An app that needs a prompt later must extend this
  deliberately (`appTakesPrompts()` in `AppManager.cpp`) and check that it
  survives being paused.
* **Takes over the screen and buttons while open.** `open()` saves the six
  button callbacks the current app had and installs its own; they are handed
  back before the done callback runs. `AppManager` draws the prompt instead
  of the active app's frame, so the app underneath is paused, not ended.
* **Navigation wraps**, like the main menu: Up on the first option goes to the
  last, Down on the last goes to the first.
* **Input is edge-triggered**, like the main menu: Up/Down move on press;
  Enter chooses on release, and only if the press also happened while the
  prompt was open (a button already held when it opened cannot pick an
  option). Left, Right and Back do nothing.
* **Long lists scroll.** Four option rows fit under the title; with more
  options the window follows the selection one row at a time and a scrollbar
  appears on the right.
* **Timeout is optional** and off unless `open()` is given one. It counts
  from opening or the last Up/Down/Enter press, never fires while Enter is
  held, closes with `kNoChoice`, and never selects an option.
* **Nothing leaks into the app.** When the prompt closes, any button that
  went down inside it (or is still down) has its Held events and its next
  Release dropped, so finishing a press cannot act on the menu underneath.
* **Always answers exactly once.** An app switch, idle sleep, or the empty
  battery shutdown first closes an open prompt with `kNoChoice` (done
  callback runs, button callbacks go back to the app they came from). An open
  prompt does not keep the device awake: opening it restarts the idle clock
  once, and when that runs out the prompt closes and sleep proceeds as usual.
  The serial `reboot` restarts directly and does not run the callback.
* **Long rows marquee.** The focused row uses `ScrollLabel`; the title does
  too, so a long title scrolls instead of being cut off.

`open()` returns false if a prompt is already open, the option count is
below 1, or the active app does not take prompts. Strings are copied, so the caller's arrays need not outlive the call.

The selection, window and timeout math lives in `ModalPromptModel.h`
(header-only, no display or button dependencies) and is covered by the
native `test_core_menuprompt` suite.

Test builds (`local_test`) can open a sample prompt over serial with
`prompt <n> [timeout_ms]`; see `lib/SerialCli/README.md`.

## Status bar and Status screen

Every menu screen draws a 12 px status strip across the top (`StatusView`);
apps and screensavers keep the full 128x64. The menu list sits below it:
13 px rows (was 16), text at the row top (was +2), still 4 visible rows, so
navigation, wrap, scrolling and the cross-slide are unchanged. The strip:

* **Left:** a WiFi glyph and the age of the last check-in: `--` never, `<1h`,
  `5h`, `3d`. Fewer arcs as the check-in ages. It never means "connected
  now"; WiFi is normally off. The only live state is Dev mode listening,
  drawn inverted with no age.
* **Middle:** the most important pending line (marquee via `ScrollLabel`
  when long).
* **Right:** battery %.

The **Status** item (a root menu leaf) shows a dot while something needs
attention. It opens a screen listing the last check-in (age, cached or
fresh), battery detail and every pending notification (`*` = needs
attention, `(late)` / `(cached)` flags). Up/Down move, Back returns;
leaving it clears the dot.

The data comes from `lib/StatusService` (header-only, no display or
networking, covered by `test_core_status`). Any subsystem posts:

```cpp
#include "StatusService.h"
StatusService::instance().post(StatusKind::Checking, nullptr,
                               StatusService::defaultPriority(StatusKind::Checking),
                               false /* sticky */, millis());
```

`StatusView::popup(kind, text, flags, onAccept)` asks about a major event
with a `ModalPrompt` (accept / "Later"). Accept clears the entry and calls
`onAccept`; Later, a teardown, or a popup that cannot open (not on the menu)
routes the event to the bar and badge. What accepting means belongs to the
caller. The check-in clock is `StatusView::nowSec()`: pass it to
`setCheckIn()`.

The emulator has no menu host, so the bar is device-only there.

## ScrollLabel

One line of text that scrolls when it is wider than its box. It is the Music
Player's now-playing title marquee, moved here unchanged:

* Text that fits (including an exact fit) never moves. `draw()` centers it in
  the box or left-aligns it, as the caller asks.
* Longer text steps 6 px left whenever more than 300 ms has passed since the
  previous step. Once it has moved 30 px past the point where its end is
  visible, it jumps to a 20 px lead-in and scrolls again.
* `reset()` returns the text to its start position (call it when the text
  changes). It does not restart the 300 ms step clock; this is the Music
  Player's behavior.
* `restart(nowMs)` also restarts the step clock, so newly shown long text
  holds still for one full step before moving. The prompt uses it for a newly
  focused row.

```cpp
ScrollLabel title;                   // one per scrolling line, kept across frames
title.draw(4, 19, 120, text, true);  // x, y, box width, text, center when short
```

`draw()` uses the current font and color and leaves the text alignment
changed; set it again afterwards if later drawing depends on it. The step
math (`tick()` / `offset()`) is covered by `test_core_menuprompt`.

Both primitives are also compiled into the emulator build (`wasm/CMakeLists.txt`)
from these same sources.
