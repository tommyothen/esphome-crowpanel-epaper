# Using the drivers outside ESPHome

The drivers are ESPHome external components and only work inside ESPHome. That
was a choice, and the MIT licence is there so you can take as much of them as
you need somewhere else.

## Why ESPHome

- **It does the plumbing.** SPI, pins, the buttons, deep sleep, fonts and
  drawing, updates over the air and YAML configuration were all there, which
  left the work for the panels themselves: their waveforms, charge balance,
  greys, glides and live mode.
- **It runs standalone.** Home Assistant is optional, and none of the examples
  needs it or a network.
- **It had nothing for these boards.** ESPHome's own drivers have no model for
  either one.

## What carries over

Plain logic and data, with little of ESPHome in it:

- **The waveforms,** and the compile-time checks that keep them balanced, in
  each board's `.cpp` under `components/`.
- **The tunings:** frame counts, glide tables and tick curves, each with how it
  was measured, in the same files and in the panel notes.
- **The controller sequences:** start-up, RAM windows, the 5.79"'s two
  controllers in cascade and the 4.2"'s partial window.
- **The algorithms** in the shared core, `components/crowpanel_epaper/`: which
  pixels a fast partial left out of balance and how that is cancelled, the
  glides' doses and their drive record, and the live mode's tick model.

## What you would replace

- **The display base.** The drivers build on ESPHome's `epaper_spi`, whose
  `EPaperBase` runs each update as a state machine stepped from ESPHome's loop.
  Step it from a task of your own instead.
- **Drawing.** Pixels land in a buffer of 4-bit levels, 0 (black) to 6
  (white). Put your own drawing in front of it: LVGL's flush callback, Adafruit
  GFX or your own.
- **SPI and pins:** ESP-IDF's `spi_master`, or Arduino's SPI.
- **The small things:** logging, PSRAM allocation and the hook that settles
  the panel before deep sleep.
- **The YAML options,** in each board's `display.py`. The setters on the C++
  classes do the same job.

## Before you start

- We have not ported them ourselves, so expect ESPHome assumptions in more
  places than this list.
- Keep the MIT licence's notice, and Waveshare's for the four-grey waveform
  ([THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).
- If you build something with them, a link back is appreciated, and so is a
  [star on GitHub](https://github.com/tommyothen/esphome-crowpanel-epaper).
