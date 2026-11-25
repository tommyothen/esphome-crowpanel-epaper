# crowpanel-epaper

ESPHome display components for ELECROW's CrowPanel ESP32-S3 e-paper HMI
boards. `crowpanel_579` drives the 5.79", a 792x272 panel run by two SSD1683
controllers in cascade. The 4.2" comes next.

- Partial refreshes in about 1.2 s that drive only the pixels that change, so
  greys elsewhere on the screen survive any number of them.
- Four greys in about 4.1 s: Waveshare's grey waveform with the particle
  shaking cut from five rounds to one, which saves 1.8 s and leaves the greys
  measuring the same. Or seven, in three passes and about 5.3 s.
- Every waveform the driver loads is balanced: each pixel spends as long
  driven one way as the other on every refresh, so nothing builds up in the
  panel however long it runs. The build checks this, so a change that breaks
  it does not compile.
- Deep sleep keeps the picture. The panel holds its frame and its RAM while
  the ESP32 sleeps, so the first update after a wake is partial, with no
  flash, and `full_update_every` keeps counting across wakes.
- Nothing blocks. It builds on `EPaperBase` from ESPHome's `epaper_spi`, so
  BUSY is polled from `loop()` and data goes out in 10 ms slices.
- One 108 KB frame buffer, 4 bits a pixel, in PSRAM. The panel's own RAM
  holds the previous frame, so there is no second copy.

Tested with ESPHome 2026.10. `epaper_spi`'s base class changes between
releases, so older versions may not build it.

## Use

```yaml
external_components:
  - source: github://tommyothen/crowpanel-epaper@main
    components: [crowpanel_579]

esp32:
  variant: esp32s3
  flash_size: 8MB
  framework:
    type: esp-idf

psram:
  mode: octal
  speed: 80MHz

logger:
  hardware_uart: UART0  # the USB-C port is a CH340 on UART0

spi:
  clk_pin: GPIO12
  mosi_pin: GPIO11

font:
  - file: "gfonts://Inter@600"
    id: font
    size: 28
    bpp: 2

display:
  - platform: crowpanel_579
    id: epd
    grey: true
    update_interval: 60s
    lambda: |-
      it.print(20, 20, id(font), Color::BLACK, "Hello");
```

| Option | Default | |
|---|---|---|
| `grey` | `false` | Full refreshes in four greys instead of the panel's own black and white one (about 3.7 s) |
| `grey_levels` | `4` | `7` adds a half step between each grey, in three passes; needs `grey` |
| `full_update_every` | `30` | Updates per full refresh; `1` makes every update full |
| `update_interval` | `never` | Most devices decide for themselves when to draw |
| `rotation` | `0` | |
| `cs_pin`, `dc_pin`, `reset_pin`, `busy_pin` | GPIO45, 46, 47, 48 | The board's wiring |
| `power_pin` | GPIO7 | Switches the panel's ground |
| `data_rate` | `20MHz` | |

`epaper_spi.full_update_next` and `epaper_spi.is_updating` work as they do for
ESPHome's own e-paper displays.

## Drawing

Draw with `Color::BLACK` on the white paper. Colours map to four levels by
luma: below 64 is black, below 128 dark grey, below 192 light grey, the rest
white. Fonts with `bpp: 2` give anti-aliased text on a grey refresh. With
`grey_levels: 7`, luma maps to seven even steps, and `bpp: 4` fonts use them.

The four greys keep Waveshare's spacing, where dark grey sits close to black,
which makes anti-aliased text look bold. Seven greys space the four evenly
first, so dark grey stands clear of black, then add the half steps.

Partial refreshes are black and white and drive only the pixels whose black
or white reading changes. Greys elsewhere stay exactly as the last grey
refresh left them; a pixel that changes shows as black or white until the
next full refresh. A large black area that a full refresh drew fades only to
grey when a partial clears it, while black that a partial drew clears
cleanly. Keep big black shapes still between full refreshes, or ask for a full
one when they move.

## Looking after the panel

- Both loaded waveforms, the partial and the grey one, give every pixel as
  many frames at +15 V as at -15 V on every refresh, with VCOM held at its DC
  level. A `static_assert` checks both tables when the component compiles.
- After each refresh the controllers discharge the pixels (end option 0x22)
  and go into deep sleep, so nothing stays driven between updates.
- The panel's own full refresh, used when `grey` is off, is its OTP waveform:
  temperature compensated, and the panel maker's to balance.
- The loaded waveforms are fixed rather than temperature compensated. They
  were tuned indoors; in the cold, expect slower, paler changes.
- A full refresh now and then still helps: it resets the deep blacks and
  clears what partials leave behind. `full_update_every` caps a run of
  partials, across deep sleep too.

## Deep sleep

Nothing to configure. Before the ESP32 sleeps, the driver sleeps the
controllers in the mode that keeps RAM and holds the power and reset pins high
through the sleep. On a wake it carries on where it left off.

```yaml
deep_sleep:
  id: sleeper
  sleep_duration: 30min

esphome:
  on_boot:
    priority: -100
    then:
      - component.update: epd
      - wait_until:
          condition:
            not:
              epaper_spi.is_updating: epd
      - deep_sleep.enter: sleeper
```

Wait for `is_updating` to clear before sleeping. If the ESP32 sleeps part way
through an update, the panel's RAM may no longer match the glass, so the
driver makes the next update a full one.

## Test card

`test-card.yaml` needs no network: `esphome run test-card.yaml` over USB. It
draws a frame with the corners named, four greys on each half, anti-aliased
text and a counter across the seam, with a grey refresh every sixth update and
partials between. A missing edge or mirrored text means the halves or the
rotation are wrong; the greys should match either side of the seam.

## The panel

From Elecrow's schematics and demos, GxEPD2's GDEY0579T93 driver, the other
community drivers listed below, and testing on the board.

- **Pins.** SPI clock GPIO12, data GPIO11, CS 45, DC 46, reset 47, BUSY 48.
  GPIO7 switches the panel's ground through Q9 and must be high before
  anything answers.
- **Two SSD1683s behind one CS.** The left controller takes the same commands
  with 0x80 set: 0x91, 0xC4, 0xC5, 0xCE, 0xCF, 0xA4, 0xA6. Commands without a
  RAM or address argument (0x12, 0x18, 0x22, 0x20, 0x3C, 0x10) go to both.
- **The split.** Each controller has 400 columns of RAM and shows 396. A
  99-byte row splits at byte 49, which both receive: the left controller
  shows its high nibble and the right one its low nibble. The right
  controller is mounted the other way round, so its X counts down from the
  seam (entry mode 0x02) while the left one counts up (0x03). The seam at
  395/396 neither repeats nor loses a column.
- **BUSY** is high while busy. The board pulls it low, so it reads idle while
  the controllers sleep; never wait for it to rise after a reset.
- **Init** is a software reset (0x12) and the internal temperature sensor
  (0x18 0x80).
- **Full refresh** is 0x22 0xF7, with the OTP waveform for the temperature.
  About 3.7 s.
- **The cascade bit.** Partial and grey refreshes need 0x21 0x00 0x10; GxEPD2
  calls the 0x10 the cascade bit. Without it, every changed pixel stops half
  way, at grey, whatever the waveform, so the advice in crowtides never to
  send 0x21 does not hold on this board.
- **Loaded waveforms.** 0x32 takes 227 bytes in the black and white mode's
  format: five tables of six groups, VCOM's and then WW, BW, WB and BB, named
  by the pixel's 0x24 bit then its 0x26 bit. 0x3F, 0x03, 0x04 and 0x2C take
  the end option and the voltages, then 0x22 0xCF runs it without the OTP.
  On this panel VSH1 (+15 V) drives a pixel black and VSL (-15 V) white.
- **The OTP partial waveform** (0x22 0xFF) runs about ten frames and also
  drives pixels that do not change: two of them took a light grey from 52 to
  65 on the camera, and eleven took it nearly to white. The driver loads its
  own instead: a pixel turning black is pushed towards white for 15 frames
  and then towards black for 15, one turning white the other way round, and
  the rest get nothing. It clears a moved box a little better than the OTP's
  and leaves greys untouched. Pushing one way only, for 20 frames, clears a
  little better still but balances only over many changes.
- **What a partial compares.** RAM 0x26 should hold what is on the glass and
  0x24 the new frame. A full refresh leaves 0x26 right. A partial refresh
  swaps the banks 0x24 and 0x26 address until the next reset, which is what
  RAM ping-pong (0x37, set from the OTP) does: afterwards, writing the new
  frame to 0x24 alone makes the next partial right and 0x26 alone does not.
  The driver writes both, as GxEPD2's `writeImageAgain` does, which works
  either way.
- **Grey.** The OTP has no grey waveform; claiming 80 to 127 °C with 0x1A in
  display mode 1 gives only black and white. Waveshare's four-grey waveform
  for their 4.2" V2, another SSD1683, works. Each pixel's high bit goes to
  0x26 and its low bit to 0x24, 11 white and 00 black. Its end option is 0x07,
  which leaves the pixels charged to the last phase (+15 V on black); the
  datasheet asks for 0x22, which discharges them, and the greys look the same.
- **Seven greys.** Each pixel's RAM bits pick one of four waveforms, so one
  refresh gives four greys at most. A seven-grey refresh runs three, each
  after its own reset: a half step towards black for the odd levels (4
  frames up from black, 1 from dark grey, 2 from light, since a grey answers
  a push by how dark it is), the four greys with dark and light driven less
  so they sit evenly apart, and the same half step towards white. The two
  half steps cancel, and the middle pass balances on its own. Measured
  against flat white and black frames, the seven read 0.01, 0.21, 0.47,
  0.58, 0.70, 0.85 and 0.99, and four partials over them changed none by more
  than 0.02. The camera sees a strong lamp gradient across the panel, so
  every reading here is corrected against flat frames.
- **Read back.** The controllers can report their temperature, RAM and display
  options, but only over the data line, which the board wires to one GPIO as
  an output. Reading would mean bit-banging it.
- **Deep sleep.** 0x10 0x01 keeps RAM. With GPIO7 and reset held high through
  an ESP32 deep sleep, the panel keeps its picture and both planes, and they
  survive the hardware reset and 0x12 that start the next update.
- **The board.** 8 MB of octal PSRAM in the S3's package. The USB-C port is a
  CH340 on UART0, which also resets the board into the bootloader, so the
  logger needs `hardware_uart: UART0` and flashing needs no BOOT button.

### The grey waveform's timing

Waveshare's waveform has four groups that do something, each run a set number
of times. Group 1 only shakes the particles: the same drive in every pixel's
table, balanced positive and negative. Group 2 is where the greys form. Busy
time on this panel, and the greys photographed against each other after a
grey frame and after eight partials:

| Group 1 | Group 2 | Busy | Greys |
|---|---|---|---|
| 5 | 5 | 5.47 s | Waveshare's |
| 3 | 5 | 4.57 s | the same |
| 2 | 5 | 4.12 s | the same |
| 1 | 5 | 3.68 s | the same, and the driver's choice |
| 1 | 3 | 3.00 s | dark and light grey a shade lighter |
| 1 | 2 | 2.65 s | dark and light grey a shade lighter |

Each frame is about 20 ms. None of them left a ghost of the frame before.

### Still open

- Dark grey in the seven-grey ramp reads 0.47, where an even ramp wants
  0.33: its table could take a little more drive.
- Thirteen greys: two more half-step passes, quarter steps, each band with its
  own length. One length for every band crosses the levels over.
- A faster grey: shortening group 2 and nudging its phases to win the greys
  back.

## Prior art

- Elecrow's demos: <https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792>
- GxEPD2's GDEY0579T93: <https://github.com/ZinggJM/GxEPD2/blob/master/src/gdey/GxEPD2_579_GDEY0579T93.cpp>
- ESPboards: <https://github.com/ESPBoards/esphome-lvgl-crowpanel-epaper-5.79-4.2>
- samperk1: <https://github.com/samperk1/esphome-crowpanel-579>
- crowtides, the best notes on the quirks: <https://github.com/rowanseymour/crowtides>
- Waveshare's 4.2" V2, the grey waveform: <https://github.com/waveshareteam/e-Paper>
- Solomon Systech's SSD1683 datasheet, in Elecrow's repo above: the waveform format and the
  voltage tables

## Licence

MIT.
