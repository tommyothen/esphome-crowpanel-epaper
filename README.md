# crowpanel-epaper

ESPHome display components for ELECROW's CrowPanel ESP32-S3 e-paper HMI
boards. `crowpanel_579` drives the 5.79", a 792x272 panel run by two SSD1683
controllers in cascade. The 4.2" comes next.

- Partial refreshes that drive only the pixels that change, so greys
  elsewhere on the screen survive any number of them: about 0.9 s for the
  first after a boot or a wake, then about 0.5 s. They send only the rows
  that changed, and when nothing did, the panel is not driven at all.
- Four greys in about 3.9 s: Waveshare's grey waveform with the particle
  shaking cut from five rounds to one, which saves 1.8 s and leaves the greys
  measuring the same. Or seven, in three passes and about 4.7 s.
- Every pixel's drive stays balanced: as long one way as the other, so
  nothing builds up in the panel however long it runs. Most refreshes balance
  on their own; the fast partials are tracked a pixel at a time and cancelled.
  The build checks every waveform, so a change that breaks this does not
  compile.
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
| `temperature_sensor` | none | Any sensor giving the panel's temperature, or near enough: the waveforms run faster in the warm and slower in the cold |
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

- The partial, grey and seven-grey waveforms give every pixel as many frames
  at +15 V as at -15 V on every refresh, with VCOM held at its DC level. A
  `static_assert` checks each table when the component compiles.
- Fast partials are the exception, on purpose. Once the driver knows what
  the glass shows, which is from the first update after a boot or a wake,
  a partial pushes each changing pixel one way only: 15 frames instead of
  15 each way. The driver keeps a bit a pixel for "pushed, not yet
  cancelled"; each fast change flips it, so no pixel is ever more than one
  push out, however long the device runs. Before a grey refresh, a 15-frame
  push back cancels them, hidden by the grey's flash. Before a deep sleep,
  which would lose the record, a settling pass does it: only those pixels
  blink, 40 frames over and 25 back, and land where they were. A device
  that wakes, draws once and sleeps never makes a fast partial, so it runs
  exactly as before.
- The ±15 V supplies stay on for five seconds after an update, so the next
  skips switching them on (0.1 s), but for no more than a minute at a
  stretch however often updates come. The controllers sleep ten seconds
  after the supplies go off.
- An update the panel has not finished in 20 s has failed, a loose ribbon
  or a dead controller say: the driver resets the controllers, which
  switches the supplies off, and starts again from a full refresh, so the
  device can still sleep. On any boot that is not a wake from deep sleep, it
  resets them too, in case a reboot caught them with the supplies on.
- After each refresh the controllers discharge the pixels (end option 0x22)
  and go into deep sleep, so nothing stays driven between updates.
- The panel's own full refresh, used when `grey` is off, is its OTP waveform:
  temperature compensated, and the panel maker's to balance.
- Particles move faster in the warm and slower in the cold. Given a
  `temperature_sensor`, the driver runs the self-balanced waveforms at a
  frame rate for the temperature, which stretches or shrinks every frame
  alike and leaves their balance untouched, in the panel maker's own bands
  (below). Fast partials and their cancelling stay at 50 Hz, since a push
  and the pass that cancels it, perhaps hours later, must match exactly; the
  maker's own partial waveform does not change with temperature either.
  Without a sensor, everything runs as tuned, for 13 to 20 °C.
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
  little better still but balances only over many changes. Fewer frames are
  faster but leave more behind, measured against flat frames (1 is white):

  | Frames a half | Black a partial drew, cleared | Black a grey refresh drew, cleared | Black drawn |
  |---|---|---|---|
  | 8 | 0.75 | 0.40 | 0.13 |
  | 10 | 0.83 | 0.60 | 0.11 |
  | 12 | 0.86 | 0.72 | 0.09 |
  | 15 | 0.89 | 0.83 | 0.07 |

- **What a partial compares.** RAM 0x26 should hold what is on the glass and
  0x24 the new frame. A refresh with a loaded waveform swaps the banks 0x24
  and 0x26 address until the next reset, which is what RAM ping-pong (0x37,
  set from the OTP) does: after a partial, writing the new frame to 0x24
  alone makes the next partial right and 0x26 alone does not. The driver
  writes the frame's black and white reading to both after every such
  refresh, as GxEPD2's `writeImageAgain` does, so the next partial is right
  with or without a reset in between.
- **Where the time goes.** A partial refresh drives the panel for 0.85 s;
  the rest was resetting and initialising the controllers, sending the
  frame, writing it back to both planes, and waiting for ESPHome's next loop
  (about 16 ms) between each of a dozen steps. The driver runs the steps
  back to back within one loop call, and leaves the controllers awake for
  ten seconds after an update, so the next one skips their reset. A partial
  went from 1.17 s to 1.0 s.
- **One SPI write a row was slow.** Each write costs more than its 50
  bytes, so rows now go out 16 at a time.
- **Only what changed.** The driver keeps a hash of each row's black and
  white reading as the glass shows it, 1 KB in RTC memory so it survives
  deep sleep. A partial sends, and afterwards copies back, only the band of
  rows that changed: a clock tick sends a sixth of the frame and takes
  0.92 s. A redraw that changes nothing skips the refresh and takes only
  the drawing, about 50 ms.
- **Grey.** The OTP has no grey waveform; claiming 80 to 127 °C with 0x1A in
  display mode 1 gives only black and white. Waveshare's four-grey waveform
  for their 4.2" V2, another SSD1683, works. Each pixel's high bit goes to
  0x26 and its low bit to 0x24, 11 white and 00 black. Its end option is 0x07,
  which leaves the pixels charged to the last phase (+15 V on black); the
  datasheet asks for 0x22, which discharges them, and the greys look the same.
- **Seven greys.** Each pixel's RAM bits pick one of four waveforms, so one
  refresh gives four greys at most. A seven-grey refresh runs three: a half
  step towards black for the odd levels (4
  frames up from black, 1 from dark grey, 2 from light, since a grey answers
  a push by how dark it is), the four greys with dark and light driven less
  so they sit evenly apart, and the same half step towards white. The two
  half steps cancel, and the middle pass balances on its own. Measured
  against flat white and black frames, the seven read 0.01, 0.18, 0.45,
  0.58, 0.69, 0.86 and 1.00, and partials over them changed none by more
  than 0.02. The camera sees a strong lamp gradient across the panel, so
  every reading here is corrected against flat frames.
- **Keeping the supplies on.** 0x22 0xCC switches the supplies on and runs
  the loaded waveform, leaving them on; 0x0C runs it with them already on;
  0x03 switches them off. Each switch takes about 0.1 s. GxEPD2 leaves them
  on between partials too. Whether an idle controller with its supplies on
  puts a voltage across the pixels was the worry: over two minutes, a grey
  frame drifted no more with them on than off (light grey 0.45 to 0.44
  against 0.40 to 0.38, the ink settling either way).
- **Settling.** The pass that cancels a fast partial without a grey refresh
  to hide it must add one push's worth of drive and still leave each pixel
  where it was, so it pushes past the far colour and comes back. Measured
  against flat frames: 30 frames over and 15 back left whites at 0.96, 40
  and 25 at 1.02, 45 and 30 at 1.03.
- **VCOM.** Every loaded waveform sets VCOM, the offset that cancels the
  TFTs' kickback; if it is off for a panel, every frame of every refresh
  leaves a little drive on every pixel, always the same way. A test: three
  minutes of frames driving nothing over a grey frame, so only VCOM acts. At
  anything from 0x20 to Waveshare's 0x30 the greys drifted no more than the
  ink settles with no frames at all (light grey -0.02). The panel's own OTP
  value, loaded with 0x22 0xB9, lightened dark grey by 0.06 and every grey by
  about 0.1, so the driver keeps 0x30. The booster's soft start (0x0C) is left
  at its reset value, as Elecrow's firmware leaves it.
- **Temperature.** Faking the temperature with 0x1A and timing the OTP's
  own waveforms shows the maker's bands: the full refresh runs 4.50 s at
  10 °C and below, 3.28 s from 13 to 20 °C, 2.27 s from 22 to 30 °C and
  1.66 s from 35 °C, while the partial runs 0.43 s at every temperature. The
  four-grey refresh at 37.5, 50, 75 and 100 Hz runs 4.78, 3.53, 2.35 and
  1.77 s, the same progression, so the driver switches frame rate at 12, 21
  and 33 °C.
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
