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
  from opening or the last Up/Down, closes with `kNoChoice`, and never
  selects an option, even if Enter is being held at that moment.
* **Long rows marquee.** The focused row uses `ScrollLabel`; the title does
  too, so a long title scrolls instead of being cut off.

`open()` returns false if a prompt is already open or the option count is
below 1. Strings are copied, so the caller's arrays need not outlive the call.

The selection, window and timeout math lives in `ModalPromptModel.h`
(header-only, no display or button dependencies) and is covered by the
native `test_core_menuprompt` suite.

Test builds (`local_test`) can open a sample prompt over serial with
`prompt <n> [timeout_ms]`; see `lib/SerialCli/README.md`.

## ScrollLabel

One line of text that scrolls when it is wider than its box. It is the Music
Player's now-playing title marquee, moved here unchanged:

* Text that fits (including an exact fit) never moves. `draw()` centers it in
  the box or left-aligns it, as the caller asks.
* Longer text steps 6 px left whenever more than 300 ms has passed since the
  previous step. Once it has moved 30 px past the point where its end is
  visible, it jumps to a 20 px lead-in and scrolls again.
* `reset()` returns the text to its start position (call it when the text
  changes). It does not restart the 300 ms step clock.

```cpp
ScrollLabel title;                   // one per scrolling line, kept across frames
title.draw(4, 19, 120, text, true);  // x, y, box width, text, center when short
```

`draw()` uses the current font and color and leaves the text alignment
changed; set it again afterwards if later drawing depends on it. The step
math (`tick()` / `offset()`) is covered by `test_core_menuprompt`.

Both primitives are also compiled into the emulator build (`wasm/CMakeLists.txt`)
from these same sources.
