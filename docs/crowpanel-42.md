# CrowPanel 4.2" panel notes

What the 4.2" V1.2 board turned out to be, from probing it, the UC8276 datasheet
(UC8276c_A0.6), Elecrow's V1.2 demo code and GxEPD2's `GxEPD2_420_SE0420NQ04`.

- **Which board.** V1.0 boards drive the panel with an SSD1683; V1.2 boards (a
  green circular sticker on the back) with a UC8276C. `crowpanel_42` is for V1.2.
  BUSY tells them apart: the UC8276C holds it low while it works and high when
  idle, the opposite of the SSD1683.
- **Pins** are the 5.79" board's: SPI clock GPIO12, data GPIO11, CS 45, DC 46,
  reset 47, BUSY 48, panel power GPIO7. Buttons: HOME GPIO2, EXIT GPIO1, PRV
  GPIO6, NEXT GPIO4, OK GPIO5.
- **No waveforms in OTP.** Reading the OTP back (0xA2, after a dummy byte) gives
  0xFF throughout, and a refresh with the LUT from OTP runs for 3 ms and moves
  nothing. Every refresh runs a waveform the driver loads, as Elecrow's own code
  does.
- **The panel's temperature.** The controller senses it as the supplies come on (TS_AUTO),
  and the driver reads it then with 0x40, through a second device on the SPI bus in 3-wire
  half-duplex mode: the board wires the panel's data line to the bus's MOSI pin alone. It read
  11 °C, which the flat's own sensors agreed with to 0.4 °C.
- **Frame rate by temperature.** The waveforms were tuned at 11 °C and 50 Hz. The frame rate
  follows the 5.79" panel's maker's curve (its full refresh ran 4.5 s at 10 °C, 3.28 s at 16.5,
  2.27 s at 26 and 1.66 s at 35), interpolated and scaled to 50 Hz at 11 °C: 55 Hz at 13 °C, 65
  at 16, 75 at 20, 95 at 26, 130 from 35. The controller's 5 Hz steps keep it smooth; bands
  would put this panel's tuning point on an edge, where one degree moved every waveform by
  40%. The seven-grey half steps scale with it, and past 200 Hz run at 200 Hz with their
  frames scaled to match. This panel's blank OTP cannot confirm the curve.
- **The data line reads back.** With GPIO11 switched to an input after a read
  command, the status flags read 0x13 idle and 0x15 with the supplies on, and
  0x40 returns the panel's own temperature in whole degrees (11 °C on the first
  night).
- **The waveform format is the SSD1683's.** Per table, groups of seven bytes: a
  repeat count, four phases (the level in the top two bits: 0 V, VSH, VSL, VDHR;
  frames in the rest, up to 63) and two state repeats. A repeat count of 1 plays
  once and 2 twice (50 frames at 50 Hz took 1.04 s and 2.03 s). VSH drives a pixel
  black and VSL white. The registers are 0x20 VCOM (eight groups), 0x21 WW (six),
  0x22 black to white (eight), 0x23 white to black (six), 0x24 BB (six); in black
  and white mode only six groups run. Note the order: the SSD1683's tables run WW,
  BW, WB, BB, so its BW is the UC8276's 0x23 and its WB is 0x22.
- **Picking a table.** In black and white mode (PSR 0x3F) with DDX 01, each pixel's
  NEW bit (DTM2, 0x13) and OLD bit (DTM1, 0x10), 1 white, pick WW, black to white,
  white to black or BB, as the SSD1683's 0x24 and 0x26 do.
- **No copy after a refresh.** The controller never copies NEW to OLD: a partial
  that sends only NEW leaves whatever OLD held. The driver writes the glass's
  reading to both after every update.
- **The RAM survives a hardware reset and deep sleep (0x07 0xA5).** A rectangle
  written to OLD came back after either. The driver still keeps the controller in
  standby through the ESP's deep sleep (about 26 µA against 0.5 µA by the
  datasheet), since how long the RAM lasts in deep sleep is not documented.
- **Voltages.** Elecrow's power setting, 0x01 03 10 3F 3F 03: VGH and VGL ±20 V,
  VSH +15 V and VSL -15 V, the SSD1683's levels.
- **VCOM belongs at -1.4 V, not Elecrow's -0.6 V.** VCOM_DC (0x82, -0.1 V a step from
  -0.1 V) is where a frame driving 0 V leaves a pixel as it is. Over a grey frame, 173 frames
  driving nothing lightened black and dark grey by 0.03 and 0.05 at -0.6 V and by 0.06 and
  0.11 at -0.1 V (GxEPD2's); 519 such frames moved them by +0.02 and +0.01 at -0.9 V, 0.00 to
  +0.01 at -1.1 and -1.3 V, and 0.00 and -0.01 at -1.5 V. Every waveform rests at 0 V for some
  frames, and every pixel outside a partial window rests for a whole refresh, so off its null
  VCOM drifts the greys and leaves a DC bias on waveforms that balance by frame count. At
  -1.4 V they balance in fact. All the greys here were tuned there.
- **SPI at 10 MHz.** The datasheet's write cycle is 100 ns at the least, so 10 MHz, though
  its feature list says 20; the 5.79" board's SSD1683s take 20. Reads, 350 ns: the
  temperature is read at 1 MHz.
- **Supplies.** Power on (0x04) takes 80 ms with Elecrow's booster soft start
  (0x06 96 96 29) and 310 ms with the default; power off (0x02) 40 ms.
- **Frame rate.** 0x30 sets it in 5 Hz steps, from 5 Hz at 0x00 to 200 Hz;
  Elecrow uses 0x09, 50 Hz.
- **Orientation.** RAM x runs along the 400 pixel side and y along the 300; with the
  board turned so Elecrow's factory menu reads normally, the origin is top left and
  nothing is mirrored.
- **Border** left floating (CDI 0xD7), as Elecrow leaves it, so no refresh drives it.
- **Tables run on their own timing.** The controller runs each table to its own
  length, the longest setting the refresh, and fills a shorter one's end with 0 V.
  So the driver checks that VCOM's table lasts as long as every pixel's, rather
  than that every table shares VCOM's timing as the 5.79" driver does.
- **Greys.** The grey waveforms keep Waveshare's 4.2" V2 VCOM, white and black tables
  and retune its dark and light ones. Where a grey lands hangs on a table's last few frames:
  9 frames of black against 3 of white in each of five repeats leaves dark grey at 0.22, and
  one more frame of black at the very end 0.12. So the tables end on a short pair of their own,
  which tunes them finer than a frame in each repeat: dark grey at a third, 1 black and 2
  white; at a sixth, 1 and 1; light grey at two thirds, 7 of black against 4 of white in each
  repeat then 1 more white. Four greys are even thirds, as ESPHome's anti-aliased fonts blend
  (Waveshare's own sat at 0.26 and 0.75). Seven greys add half steps at 150 Hz: 3, 5 and 4
  frames for levels 2, 3 and 5.

- **Grey partials, through the partial window.** 0x91 and 0x90 confine a refresh to a window,
  8 pixels at a time across and a row at a time down, and the pixels outside rest at 0 V. With
  the gates scanning the whole panel (PT_SCAN 1) and VCOM at its null, three windowed grey
  refreshes left four grey bands outside their windows within 0.01; scanning only the window's
  rows (PT_SCAN 0) let the bands outside drift by up to 0.5. In partial mode the planes' writes
  wrap within the window, so the driver writes whole planes first and leaves partial mode
  (0x92) after each pass. With `grey_partials`, the driver keeps the last frame drawn and
  refreshes, in greys, the window around every pixel whose level changed: a striped box drew
  0.92, 0.71, 0.35 and 0.01 and, removed again, left its area at the background's 0.38 and 0.69.

## Measured

Times are whole updates, drawing included, with logging at DEBUG: VERBOSE logging over the
115200 baud UART adds about 0.15 s to each. Greys are the first refresh after black and white
frames; repeated, they stay within 0.04. At 11 to 12 °C (the panel's sensor and the flat's own agreed), 50 Hz, against flat white and black
frames, each bar the panel's full height: where a bar sits moves its reading by up to 0.05,
since the camera's response is not linear and the lamp lights the panel unevenly.

| Refresh | Time | Result |
|---|---|---|
| Black and white (Elecrow's 20, 10, 20 frames) | 1.15 to 1.3 s | Clean black and white |
| Four greys | 3.7 s | 0.00, 0.33, 0.70, 0.95 |
| Seven greys | 3.95 s | 0.00, 0.18, 0.34, 0.49, 0.69, 0.84, 1.00 |
| Fast partial, 12 frames one way | 0.44 s | 0.00 black, 0.96 white; greys elsewhere stay |
| Grey partial (`grey_partials`), four greys | 3.75 s | The window only; outside within 0.01 |
| Nothing changed | 0.09 s | Nothing driven |

Also checked on the panel:

- **The cold partial needs its 25 frames each way.** Erasing blacks a grey refresh drew, 15,
  20 and 25 frames left 0.96, 0.98 and 1.00; drawing black from white, 0.12, 0.08 and 0.02.
  A balanced partial pushes the far way first, so it needs more than the fast partial's 12.
- **Deep sleep.** Two fast partials left 1,320 pixels a push out; they settled before the
  sleep, the picture held, and the first update after the wake was a cold partial against the
  controller's RAM (0.98 s), then fast ones again.
- **The watchdog.** With BUSY stuck, an update gives up after 20 s, resets the controller and
  sets the warning; the next update tries again.

## Still open

- Check the frame-rate curve in a warm room: it borrows the 5.79" panel's maker's.
