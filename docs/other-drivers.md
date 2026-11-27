# Other drivers and thanks

This driver builds on a lot of other people's work. Elecrow, Waveshare, GxEPD2,
ESPBoards, samperk1 and crowtides all worked out things I would otherwise have
spent weeks on, and what came from each is below. Thank you to all of them.

## How this one compares

I looked at the projects under [Prior art](#prior-art) and about 20 other public
ones that drive these boards, on 2026-11-27. Most are displays built for one job
on top of Elecrow's demos, GxEPD2 or ESPBoards' component, and plenty of them do
that job well.

Where this one differs, as far as I could find:

- **Greys.** I could not find another driver for either board with greys. This
  one has four or seven, tuned on each panel in perceptual lightness.
- **Balance.** It records the drive its partial refreshes leave and cancels it,
  and checks its waveforms' balance when it compiles. I did not find that
  elsewhere.
- **Glides and live mode.** It changes greys without flashing, and can animate.
  The fastest refresh I found elsewhere is a whole bare 5.79" panel in about
  150 ms, on its own adapter rather than this board.
- **ESPHome.** ESPHome's own drivers do not cover either board yet (as of
  2026.11), which is part of why this one exists.

Others do things this one does not, and one of them may suit you better:

- **A design built around LVGL:**
  [ESPBoards' component](https://github.com/ESPBoards/esphome-lvgl-crowpanel-epaper-5.79-4.2),
  and [samperk1's](https://github.com/samperk1/esphome-crowpanel-579) for the
  5.79". Both look good, though I have not tested them. ESPHome's own LVGL also
  works with this driver, in short tests (see the [guide](guide.md#lvgl)).
- **The 4.2" V1.0,** with its SSD1683: ESPBoards' component.
- **The panels' built-in fast modes:** samperk1's.
- **A page editor in Home Assistant:**
  [epaper-panel](https://github.com/bek00letz/epaper-panel).

## What I learnt from them

- **Waveshare:** the four-grey waveform, from their 4.2" V2 code
  (`epd4in2_V2.py`), changed for these panels. Its permission notice is in
  [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
- **Elecrow:** the boards' pins, the 4.2"'s black and white refresh timing (20,
  10 and 20 frames) and its voltages, from their demos and schematics.
- **GxEPD2:** the 5.79"'s cascade bit (0x21's 0x10) and writing the picture to
  both RAM planes after a refresh. Ideas, not code.
- **crowtides:** notes on the 5.79"'s two controllers.

## Prior art

- Elecrow's demos: <https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792>
- GxEPD2's GDEY0579T93: <https://github.com/ZinggJM/GxEPD2/blob/master/src/gdey/GxEPD2_579_GDEY0579T93.cpp>
- ESPboards: <https://github.com/ESPBoards/esphome-lvgl-crowpanel-epaper-5.79-4.2>
- samperk1: <https://github.com/samperk1/esphome-crowpanel-579>
- crowtides, the best notes on the quirks: <https://github.com/rowanseymour/crowtides>
- Waveshare's 4.2" V2, the grey waveform: <https://github.com/waveshareteam/e-Paper>
- Solomon Systech's SSD1683 datasheet, in Elecrow's repo above: the waveform format and the
  voltage tables
- Elecrow's 4.2" demos, whose V1.2 code loads its own waveforms: <https://github.com/Elecrow-RD/CrowPanel-ESP32-4.2-E-paper-HMI-Display-with-400-300>
- GxEPD2's SE0420NQ04, a UC8276C panel: <https://github.com/ZinggJM/GxEPD2/blob/master/src/other/GxEPD2_420_SE0420NQ04.cpp>
- UltraChip's UC8276 datasheet (UC8276c_A0.6): the commands, the partial window and the timing

