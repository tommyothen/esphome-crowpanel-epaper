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
  VSH +15 V and VSL -15 V, the SSD1683's levels. VCOM_DC 0x05 is -0.6 V.
- **Supplies.** Power on (0x04) takes 80 ms with Elecrow's booster soft start
  (0x06 96 96 29) and 310 ms with the default; power off (0x02) 40 ms.
- **Frame rate.** 0x30 sets it in 5 Hz steps, from 5 Hz at 0x00 to 200 Hz;
  Elecrow uses 0x09, 50 Hz.
- **Orientation.** RAM x runs along the 400 pixel side and y along the 300; with the
  board landscape and the buttons on the right, the origin is top left and nothing
  is mirrored.
- **Border** left floating (CDI 0xD7), as Elecrow leaves it, so no refresh drives it.

## Measured

At 11 °C on the panel's sensor, 50 Hz, against flat white and black frames:

| Refresh | Time | Result |
|---|---|---|
| Black and white (Elecrow's 20, 10, 20 frames) | 1.3 to 1.4 s | Clean black and white |
| Four greys (the 5.79" driver's waveform) | 3.8 s | 0.00, 0.26, 0.75, 0.95 |
| Seven greys (the 5.79" driver's passes) | 4.4 s | 0.00, 0.50, 0.72, 0.82, 0.84, 0.91, 0.97: not tuned for this panel |
| Fast partial, 15 frames one way | 0.62 s | Solid black and white; greys elsewhere stay |

## Still open

- Tune the seven-grey passes on this panel; they need the camera.
- Read the panel's own temperature through a 3-wire read on the SPI bus, so no
  `temperature_sensor` is needed.
- Measure what the fast partial's 0.62 s is made of; the 5.79" manages 0.4 to 0.5 s.
