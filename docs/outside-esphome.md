# Using the drivers outside ESPHome

These drivers are ESPHome external components, so on their own they only work
inside ESPHome. If you are building in another framework, you are very welcome
to take whatever parts help you. The project is under the MIT licence, and a
lot of the panel work lives in plain C++ that you can adapt. If you do port it,
[open an issue](https://github.com/tommyothen/esphome-crowpanel-epaper/issues)
with a link and I will add it to the docs, so people on that framework can find
it.

## Why ESPHome

I run Home Assistant at home and already have other boards in ESPHome, so I
wanted these displays to plug in the same way as everything else. ESPHome also
handled SPI, the buttons, deep sleep, fonts and updates over the air, which
left the time for the panels themselves.

You do not need Home Assistant to use them, though. ESPHome runs on its own,
and none of the examples needs Home Assistant or a network. ESPHome's own LVGL
works with them too, in the short tests I have run ([LVGL](guide.md#lvgl)). For
a driver built around LVGL,
[ESPBoards' component](https://github.com/ESPBoards/esphome-lvgl-crowpanel-epaper-5.79-4.2)
looks good for the same boards, though I have not tested it.

## What carries over

Most of the panel logic and data has little of ESPHome in it:

- **Waveforms and balance checks.** Each board's `.cpp` file under `components/`
  holds the waveforms and the compile-time `static_assert` checks that keep
  them balanced.
- **Tunings.** Frame counts, glide tables and tick curves live in those same
  files and in the panel notes for the [5.79"](crowpanel-579.md) and
  [4.2"](crowpanel-42.md).
- **Controller sequences.** The board files also cover start-up, RAM windows,
  the 5.79"'s two controllers in cascade and the 4.2"'s partial window.
- **The shared core.** `components/crowpanel_epaper/` tracks which pixels a fast
  partial left out of balance and cancels that, and holds the glides' doses and
  drive record as well as the live mode's tick model.

## What you would replace

To run the drivers somewhere else, these are the ESPHome parts you would swap
out:

- **The display base.** The drivers build on ESPHome's `epaper_spi`, where
  `EPaperBase` runs each update as a state machine stepped from ESPHome's loop.
  You can step it from a task of your own instead.
- **Drawing.** Pixels go into a buffer of 4-bit levels from 0 (black) to 6
  (white), so you can put LVGL's flush callback, Adafruit GFX or your own
  drawing code in front of it.
- **SPI and pins.** ESP-IDF's `spi_master` or Arduino's SPI can replace
  ESPHome's SPI and pin handling.
- **The smaller pieces.** Logging, PSRAM allocation and the hook that settles
  the panel before deep sleep will need equivalents in your setup.
- **YAML options.** Each board's `display.py` turns the YAML into calls to the
  C++ setters, so you can call those setters directly.

## Notes on porting

I have not ported the drivers myself, so you will likely find more
ESPHome-specific bits in the code than are listed here.

Everything here is under the MIT licence, so all it asks is that the licence
notice stays with anything you reuse. The four-grey waveform is adapted from
Waveshare's, so its notice in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) goes with that part.

If you adapt the drivers or build something with them, I would love to see
what you make, and a port can go in these docs (see the top of this page). A
link back is always appreciated, and if the project helped you, a
[star on GitHub](https://github.com/tommyothen/esphome-crowpanel-epaper) helps
other CrowPanel owners find it.
