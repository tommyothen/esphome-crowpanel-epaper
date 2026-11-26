# crowpanel-epaper

ESPHome external display components for ELECROW's CrowPanel ESP32-S3 e-paper
HMI boards. Tested with ESPHome 2026.10.

## Boards

| Board | Resolution | Component | Status |
|---|---|---|---|
| CrowPanel 5.79" | 792x272 | `crowpanel_579` | Ready |
| CrowPanel 4.2" V1.2 (UC8276C) | 400x300 | `crowpanel_42` | Ready |

Both build on `crowpanel_epaper`, which holds what they share: the buffer and its
levels, the planes made from it, the record of what the glass shows and of which
pixels a fast partial left out of balance, the update's passes, and glides and
live ticks (below), which work the same on both. Each board's
component holds only its controllers and their waveforms. List `crowpanel_epaper`
alongside the board's component under `external_components`, or leave
`components:` out to make every component available.

## CrowPanel 5.79"

`crowpanel_579` drives the 5.79" panel, which uses two SSD1683 controllers in
cascade behind one SPI chip select. It builds on `EPaperBase` from ESPHome's
`epaper_spi`, so updates do not block: BUSY is polled from `loop()` and data
goes out in 10 ms slices. The frame buffer is 108 KB (4 bits per pixel) in
PSRAM, while the panel's own RAM holds the previous frame.

Compared with the panel's built-in black and white OTP refreshes:

| Mode | Time (13 to 20 °C) | Behaviour |
|---|---|---|
| Full black and white (`grey: false`) | about 3.7 s | Panel OTP waveform, temperature compensated |
| Full four greys (`grey: true`) | about 4.2 s | 4 greys, charge balanced; the panel OTP has no grey mode (1.8 s faster than Waveshare's unmodified 4.2" V2 waveform, with the same measured greys) |
| Full seven greys (`grey_levels: 7`) | about 4.4 s | 7 evenly spaced greys in three passes, charge balanced |
| Partial, first after boot or wake | about 1.3 s | Black and white, charge balanced, changed rows only; greys elsewhere survive (the panel's OTP partial fades them) |
| Partial, subsequent | 0.4 to 0.5 s | One-way push with per-pixel balance tracking, changed rows only |
| Partial, nothing changed | about 50 ms | Drawing only; the panel is not driven |
| Glide (`grey_partials: glide`) | about 0.5 s for a box, 1.3 s whole panel | No flash: each changed pixel goes straight to its new grey, within about 0.055; the next full refresh settles them |
| Live (`grey_partials: live`) | 14 to 16 ticks a second for a clock, a ball and a ticker | Each tick nudges pixels towards the latest frame |

The 5.79"'s glides and live ticks run at 195 Hz, its controllers' fastest rate, tuned
on this panel, whose ink moves at about two thirds of the 4.2"'s pace; its ticks are
8 frames rather than 4. It has no partial window, so its glides settle at the next
full refresh.

Panel wiring details, waveform notes and measurements are in
[docs/crowpanel-579.md](docs/crowpanel-579.md).

## CrowPanel 4.2"

`crowpanel_42` drives the 4.2" V1.2 board (a green circular sticker on the
back), whose UC8276C has no waveforms in its OTP: every refresh runs one the
driver loads. It works as `crowpanel_579` does, with the same options and the
same fast partials, cancelling and settling, and the pins default to the same
wiring. V1.0 boards use an SSD1683 and are not supported yet.

- **The panel's own temperature.** The driver reads it each time the supplies come
  on, so it needs no `temperature_sensor`, and runs its waveforms faster in the
  warm; `id(epd).panel_temperature()` returns it in °C.
- **VCOM at its null.** Measured on the panel, at -1.4 V a frame driving nothing
  leaves the greys as they are. Elecrow's -0.6 V lightened them on every such
  frame, which also left a DC bias on waveforms that balance by frame count.

| Mode | Time (50 Hz) | Behaviour |
|---|---|---|
| Full black and white | 1.15 to 1.3 s | Balanced, Elecrow's waveform |
| Full four greys | about 3.7 s | Even thirds: measured 0.00, 0.33, 0.70 and 0.95 |
| Full seven greys | about 3.95 s | Measured 0.00, 0.18, 0.34, 0.49, 0.69, 0.84 and 1.00 |
| Partial, subsequent | about 0.44 s | One-way push with per-pixel balance tracking |
| Grey partial (`grey_partials`) | about 3.75 s | Greys in the changed window only; the rest untouched |
| Glide (`grey_partials: glide`) | 0.32 to 0.38 s for a box, 0.93 s whole panel | No flash: each changed pixel goes straight to its new grey, within about 0.04 |
| Live (`grey_partials: live`) | 23 ticks a second for a clock and a bouncing ball | Each tick nudges pixels towards the latest frame; ticks on until everything arrives |
| Fast greys (`fast_greys`) | 2.0 s four or seven | Two shakes and a push: about twice as quick, erasing the picture before as well |

```yaml
display:
  - platform: crowpanel_42
    id: epd
    grey: true
    lambda: |-
      it.print(20, 20, id(font), Color::BLACK, "Hello");
```

It also does what the 5.79" cannot: **grey partials**. The UC8276C can confine a
refresh to a window, so with `grey_partials: true` a partial update redraws, in
greys, just the window around what changed, and only that window flashes:

```yaml
display:
  - platform: crowpanel_42
    grey: true
    grey_partials: true  # partials in greys, flashing only what changed
```

A grey partial takes as long as a grey refresh, and works across deep sleep too:
the driver keeps a record of each row's greys in RTC memory, so after a wake it
redraws the band of rows that changed. Without it, partials are the fast black
and white ones.

A busy screen can have it both ways: fast black and white partials while things
change, then, once it is still, `request_grey_partial()` redraws in greys just the
window around everything changed since the last grey refresh, including areas a
black and white partial changed and changed back:

```yaml
script:
  - id: settle
    mode: restart  # each button press starts the wait again
    then:
      - delay: 3s
      - lambda: id(epd).request_grey_partial();
      - component.update: epd
```

Or **glide**: with `grey_partials: glide`, a partial update takes each changed
pixel straight from its grey to its new one, without flashing anything, in a
few short pushes inside the window: a box in 0.32 to 0.38 s, the whole panel
in 0.93 s. The greys land near enough, about 0.04 out on average and 0.13 at
worst; the next grey refresh, full or asked for with `request_grey_partial()`,
settles them exactly. Every push is recorded per pixel and cancelled before
that refresh, and before a deep sleep the driver settles glides itself.

```yaml
display:
  - platform: crowpanel_42
    grey: true
    grey_partials: glide  # no flash; greys settle exactly at the next grey refresh
```

**Live**: with `grey_partials: live`, each update is a short tick that pushes
every pixel a little towards the frame just drawn. While the panel runs one
tick, the driver draws the next frame and plans its tick, on both cores, and it
carries on by itself for as long as the picture changes or anything is still on
its way: draw by time (`millis()`) and the panel animates with nothing asking
for updates. An update asked for during a tick waits for it, so the latest
frame wins. The panel follows as fast as the ink allows, about 0.3 to 0.5 s for
a full black and white change, so fast motion trails. Pixels at black or white
keep being pushed the same way while their recorded drive points the other
way, which changes nothing you can see but pays it back. Live ticks do not
count towards `full_update_every`, so no full refresh interrupts an animation.
The same record and settling as glides apply. A tick's time is mostly the
refresh (31 ms), sending its window (10 MHz on the 4.2"; `data_rate: 20MHz`,
beyond the UC8276C's datasheet but clean in every test here, took a busy
animation from 15 to 16.5 ticks a second) and drawing the frame.

And **fast greys**: with `fast_greys: true`, grey refreshes shake each pixel twice
and push it once instead of shaking it many times, so four or seven greys take
2 s rather than 3.7 or 3.95, and grey partials likewise. Drawn over every other
grey, both leave each grey within 0.09 of where it should be.

Notes and measurements are in [docs/crowpanel-42.md](docs/crowpanel-42.md).

## Minimal configuration

Pin `external_components` to a full commit SHA or a tag; GitHub will not fetch a
short SHA. The board's USB-C port is a CH340 on UART0, which resets the board
into the bootloader automatically, so flashing needs no BOOT button. For the
4.2", use `crowpanel_42` for the component and the platform.

```yaml
external_components:
  - source: github://tommyothen/crowpanel-epaper@6d2fad834167507be36253891417bc9f2f72fe72  # a full commit SHA or a tag
    components: [crowpanel_579, crowpanel_epaper]  # crowpanel_epaper is the core both boards share

esp32:
  variant: esp32s3
  flash_size: 8MB
  framework:
    type: esp-idf

psram:
  mode: octal
  speed: 80MHz

logger:
  hardware_uart: UART0

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

`epaper_spi.full_update_next` and `epaper_spi.is_updating` work as they do for
ESPHome's built-in `epaper_spi` displays.

## Options

All pins default to the boards' fixed wiring, which the two share.

| Option | Default | Notes |
|---|---|---|
| `grey` | `false` | Full refreshes in four greys (about 3.9 s) instead of the panel's OTP black and white refresh (about 3.7 s) |
| `grey_levels` | `4` | `4` or `7`. `7` runs three passes with a half step between each of the four greys (about 4.4 s); requires `grey: true` |
| `temperature_sensor` | none | Sensor ID for panel temperature in °C; switches self-balanced waveforms across temperature bands. Without one, runs at 50 Hz (13 to 20 °C) |
| `full_update_every` | `30` | Updates per full refresh (`1` to `255`); `1` makes every update full |
| `update_interval` | `never` | Most devices decide for themselves when to draw |
| `rotation` | `0` | Display rotation in degrees |
| `cs_pin` | `GPIO45` | SPI chip select |
| `dc_pin` | `GPIO46` | Data/command pin |
| `reset_pin` | `GPIO47` | Panel reset pin |
| `busy_pin` | `GPIO48` | Panel BUSY input |
| `power_pin` | `GPIO7` | Switches the panel's ground through Q9; must be high before anything answers |
| `data_rate` | `20MHz` | SPI clock rate (10 MHz on the 4.2", the UC8276C's limit) |
| `grey_partials` | `false` | `glide`: partials take each changed pixel straight to its new grey without flashing. `live`: each update is a tick towards the latest frame, for things that move. Needs `grey: true` and PSRAM |
| `lambda`, `pages` | none | Standard ESPHome drawing options; at most one may be set |

`crowpanel_42` takes the same options, with these differences:

| Option | Default | Notes |
|---|---|---|
| `grey_partials` | `false` | Also `true`: partials redraw the window around what changed in greys, flashing only it |
| `fast_greys` | `false` | Grey refreshes in two shakes and a push: about 2 s rather than 3.7 or 3.95; needs `grey: true` |
| `temperature_sensor` | none | Used instead of the panel's own sensor, which the driver reads anyway |
| `busy_pin` | `GPIO48`, inverted | The UC8276C holds BUSY low while it works |

`id(epd).request_grey_partial()` and `id(epd).panel_temperature()` are the 4.2"'s
own calls.

## Drawing

Draw with `Color::BLACK` on `Color::WHITE` paper.

- With four greys, colours map by luma: below 64 is black, below 128 dark grey,
  below 192 light grey, and the rest white. Fonts with `bpp: 2` give
  anti-aliased text on a grey refresh. On the 5.79" the four greys keep
  Waveshare's spacing, where dark grey sits close to black and makes
  anti-aliased text look bold; on the 4.2" they are even thirds.
- With `grey_levels: 7`, luma maps to seven even steps, and `bpp: 4` fonts use
  them. Seven greys space the four base levels evenly first, so dark grey
  stands clear of black, then add the half steps.
- Partial refreshes are black and white and drive only pixels whose black or
  white reading changes (white from level 4 up). Greys elsewhere stay as the
  last grey refresh left them; a changed pixel shows as black or white until
  the next full refresh, or on the 4.2" a grey partial.

## Front controls

The controls are plain GPIOs, active low with the board's own pull-ups, so they
work with ESPHome's `gpio` binary sensor and can wake the board from deep sleep
with `esp32_ext1_wakeup` in `ANY_LOW` mode. The two boards share them.

| 5.79" | 4.2" | GPIO |
|---|---|---|
| Dial up | PRV | GPIO6 |
| Dial down | NEXT | GPIO4 |
| Dial press | OK | GPIO5 |
| MENU | HOME | GPIO2 |
| EXIT | EXIT | GPIO1 |

```yaml
binary_sensor:
  - platform: gpio
    id: dial_press
    pin: { number: GPIO5, inverted: true }
    filters: [delayed_on: 20ms]
    on_press:
      - epaper_spi.full_update_next: epd
      - component.update: epd
```

## Deep sleep

Nothing extra needs configuring on the display. Before the ESP32 sleeps, the
driver finishes any pending plane copy, settles any fast-partial pixels left
out of balance, puts the 5.79"'s controllers into deep sleep mode 1 (which
retains RAM) or switches the 4.2"'s supplies off and leaves its controller in
standby (its own deep sleep loses the RAM within 30 minutes), and holds
`power_pin` and `reset_pin` high through sleep. On wake, the
first update can be partial with no flash, and `full_update_every` and the row
hashes carry on across wakes.

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

Wait for `epaper_spi.is_updating` to clear before entering deep sleep. If the
ESP32 sleeps part way through an update, panel RAM may no longer match the
glass, so the driver makes the next update a full one.

## Looking after the panel

- **Charge-balanced waveforms.** The cold partial, four-grey and seven-grey
  waveforms give every pixel equal frames at +15 V and -15 V on every refresh,
  with VCOM held at its DC level. Compile-time `static_assert` checks verify
  each table's net drive, and that every table in a waveform runs to the same
  timing (on the 4.2", that VCOM's table lasts as long as the longest), so a
  change that breaks balance does not compile.
- **Fast partials and cancelling.** Once the driver knows what the glass shows
  (after the first update since a boot or wake), a partial pushes each changing
  pixel one way only: 15 frames on the 5.79", 12 on the 4.2", where a balanced
  partial drives 25 each way. A bit per pixel tracks
  whether that pixel is a push out of balance; each fast change flips the bit,
  so no pixel is ever more than one push out however long the device runs.
  Before the next full refresh, a 15-frame push back cancels them, hidden by
  the refresh's flash. Before a deep sleep, which would lose that bit map, a
  settling pass cancels them instead: only those pixels blink (40 frames over
  and 25 back) and land where they were. A device that wakes, draws once and
  sleeps never makes a fast partial.
- **Glides (4.2").** A glide pushes pixels by varying amounts, so it records
  each pixel's net drive, in frames at 200 Hz, a byte a pixel in PSRAM. Before
  any grey refresh that covers a pixel, full or partial, scans push it back by
  exactly that much, hidden by the refresh. A glide that would take a pixel
  past 127 frames either way settles its window in greys instead, and a deep
  sleep, which would lose the record, settles every glide first.
- **Discharge and power wind-down.** On the 5.79" every loaded waveform uses
  end option `0x22` to discharge the pixels after the last phase. The ±15 V
  supplies stay on for 5 s after an update so a follow-up skips switching them
  on (0.1 s on the 5.79", 0.08 s on the 4.2"), capped at 60 s at a stretch
  however often updates arrive. The 5.79"'s controllers enter deep sleep 10 s
  after the supplies switch off.
- **VCOM at its null (4.2").** VCOM is set where a frame driving 0 V leaves a
  pixel as it is, measured on the panel, so the waveforms balance in fact, not
  only by frame count, and pixels outside a grey partial's window keep their
  greys.
- **Update watchdog and boot reset.** If an update does not finish in 20 s (a
  loose ribbon or a failed controller), the driver logs an error, sets a
  warning status, resets and sleeps the controllers so the supplies switch off
  and the device can still sleep, and starts again from a full refresh. On any
  boot that is not a wake from deep sleep, it resets and sleeps them too, in
  case a reboot or OTA update caught them with the supplies on.
- **Temperature compensation.** Particles move faster in the warm and slower in
  the cold. When `grey` is off, the panel's OTP full refresh compensates on its
  own. With `temperature_sensor` set, the self-balanced waveforms switch frame
  rate across the panel maker's bands: 37.5 Hz below 12 °C, 50 Hz from 12 to
  under 21 °C, 75 Hz from 21 to under 33 °C, and 100 Hz from 33 °C. Scaling the
  frame rate changes every frame alike and keeps balance intact; the seven-grey
  half steps scale their frame counts to match. Fast partials, cancelling and
  settling always run at 50 Hz so pushes and cancellations match even hours
  later at another temperature; the maker's OTP partial does not change with
  temperature either. Without a sensor, waveforms run at 50 Hz (13 to 20 °C).
  Measured timings by band are in [docs/crowpanel-579.md](docs/crowpanel-579.md).
  The 4.2" reads its panel's own temperature as its supplies come on and sets
  its frame rate in 5 Hz steps along the same maker's curve, from 50 Hz at 11 °C
  where it was tuned.
- **Periodic full refreshes.** `full_update_every` caps a run of partials,
  across deep sleep too, to reset deep blacks and clear what partials leave
  behind.

## Examples

Two demos need no network: flash one over USB and drive it from the board's own
buttons. Each refresh's time shows on the panel a moment after it lands.

- [examples/demo-579.yaml](examples/demo-579.yaml): the dial counts with fast partials, the press
  swaps four and seven greys, MENU runs a grey refresh, EXIT sleeps for 10 s.
- [examples/demo-42.yaml](examples/demo-42.yaml): prev and next count; OK cycles how, between
  glides, fast partials that settle into greys 3 s after the last press, grey partials every
  time, and fast partials alone; Home swaps four and seven greys, Exit sleeps for 10 s; fast
  greys throughout, and the panel's own temperature in the corner.
- [examples/live-42.yaml](examples/live-42.yaml) and [examples/live-579.yaml](examples/live-579.yaml):
  live mode, with nothing asking for updates after the first: a clock, a bouncing ball and, on
  the 5.79", a scrolling ticker, with the ticks a second each panel manages.

## Test card

For the 4.2", [test-card-42.yaml](test-card-42.yaml) does the same with seven greys, the panel's
own temperature and grey partials.

[test-card.yaml](test-card.yaml) needs no network: flash and read logs over USB
with `esphome run test-card.yaml`. It draws a border on the outermost pixels
with the four corners named, four greys on each half, anti-aliased text in
black and dark grey, a counter across the seam and a moving box, with a grey
refresh every sixth update and partials between. A missing edge or mirrored
text means the halves or rotation are wrong; the greys should match on both
sides of the seam.

## Limitations

- **ESPHome version.** Tested with ESPHome 2026.10. `epaper_spi`'s base class
  changes between releases, so older versions may not compile the component.
- **Black and white partials.** Any pixel that changes during a partial refresh
  shows as black or white until the next full refresh.
- **Clearing full-refresh blacks.** A large black area drawn by a full refresh
  fades only to grey when a partial clears it, while black drawn by a partial
  clears cleanly. Keep large black shapes still between full refreshes, or ask
  for a full refresh with `epaper_spi.full_update_next` when they move.
- **No internal temperature readback on the 5.79".** Provide a
  `temperature_sensor` if it runs outside 13 to 20 °C. The 4.2" reads its own,
  through a 3-wire read on the same SPI pins; the 5.79" could too, but does not
  yet.
- **RAM for fast partials.** Tracking unsettled pixels allocates an extra 54 KB
  alongside the 108 KB frame buffer on the 5.79", and 30 KB alongside 60 KB on
  the 4.2", which keeps another 60 KB for the last frame drawn in greys. If an
  allocation fails, fast partials are disabled and partials run balanced, or
  grey partials black and white.
- **Sleeping mid-update.** Entering deep sleep while `epaper_spi.is_updating`
  is true leaves any unsettled fast-partial pixels uncancelled and forces a
  full refresh on the next wake.
- **CrowPanel 4.2".** V1.0 boards (SSD1683) are not supported, and its frame rate by
  temperature borrows the 5.79" panel's maker's curve until checked in a warm room.
- **Glides and live ticks** land near enough, not exactly, until the next grey refresh settles
  them; on the 5.79" that is the next full refresh. Text scrolling continuously smears on either
  panel: the ink needs 0.3 to 0.5 s to settle, so slide things in and hold them instead. They need PSRAM: about 330 KB on the 4.2" and 610 KB on the 5.79".

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

## Licence

MIT. See [LICENSE](LICENSE).
