# Other drivers and thanks

## How this one compares

Checked on 2026-11-27 against the projects under [Prior art](#prior-art) and about
20 other public ones that drive these boards, most of them single-purpose
displays built on Elecrow's demos, GxEPD2 or ESPBoards' component.

- **Greys.** No other driver for either board offers greys. This one has four
  or seven, tuned on each panel in perceptual lightness.
- **Balance.** None of the others records or cancels the drive its partial
  refreshes leave, or checks its waveforms' balance when it compiles.
- **Glides and live mode.** None changes greys without flashing or animates.
  The fastest found elsewhere refreshes a whole bare 5.79" panel in about
  150 ms, on its own adapter rather than this board.
- **ESPHome's own drivers** have no model for either board, as of 2026.11.

What others have that this one does not: a design built around LVGL
(ESPBoards, samperk1), the 4.2"
V1.0 with an SSD1683 (ESPBoards), the panels' built-in fast modes (samperk1),
and a page editor in Home Assistant
([epaper-panel](https://github.com/bek00letz/epaper-panel)).

**LVGL.** ESPHome's own LVGL works with this driver in short tests (see the
[guide](guide.md#lvgl)). For a driver built around it,
[ESPBoards' component](https://github.com/ESPBoards/esphome-lvgl-crowpanel-epaper-5.79-4.2),
or [samperk1's](https://github.com/samperk1/esphome-crowpanel-579) on the 5.79",
looks good, though I have not tested either.

## What this driver took from them

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

