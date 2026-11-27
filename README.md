# crowpanel-epaper

ESPHome external display components for ELECROW's CrowPanel ESP32-S3 e-paper
HMI boards. Tested with ESPHome 2026.10.

## Boards

| Board | Resolution | Component | Status |
|---|---|---|---|
| CrowPanel 5.79" | 792x272 | `crowpanel_579` | Ready |
| CrowPanel 4.2" V1.2 (UC8276C) | 400x300 | `crowpanel_42` | Ready |
| Either, without charge balance | | `crowpanel_579_turbo`, `crowpanel_42_turbo` | Experimental; see [Turbo](#turbo) |

Both boards build on `crowpanel_epaper`, which holds what they share: the 4-bit
frame buffer and its levels, the planes made from it, the record of what the
glass shows and of which pixels a fast partial left out of balance, the
update's passes, and glides and live ticks. Each board's component holds only
its controllers and waveforms. List `crowpanel_epaper` alongside the board's
component under `external_components`, or leave `components:` out to make every
component available.

## Choosing a driver

Start with the balanced platform for your board (`crowpanel_579` or
`crowpanel_42`). Pick the refresh mode by how often the picture changes and
whether it needs greys.

| Platform or mode | Use for | Time and look | Needs | Risks and limits |
|---|---|---|---|---|
| `crowpanel_579` / `crowpanel_42`, `grey: false` (default) | Plain black and white, or the quickest full refresh on the 4.2" | Full B&W: about 3.7 s (5.79" OTP) or 1.15 to 1.3 s (4.2"). Flashes. Fast partials 0.4 to 0.5 s | Standard pins; 54 KB / 30 KB RAM for fast partials | Black and white only; no anti-aliased text |
| `grey: true` (4 greys) | Dashboards, anti-aliased text (`bpp: 2`), wake-draw-sleep displays | Full refresh: about 4.2 s (5.79") or 3.7 s (4.2"). Flashes | `grey: true` | On the 5.79" dark grey sits darker than an even third; on the 4.2" greys are near even thirds |
| `grey_levels: 7` | Shading, pictures, `bpp: 4` text, or even grey steps on the 5.79" | Full refresh: 4.4 to 4.5 s (5.79") or 3.95 s (4.2"), three passes. Flashes | `grey: true` | Longer than four greys |
| `fast_greys: true` (4.2" only) | 4.2" dashboards or pictures that run grey refreshes often | Full 4 or 7 greys in about 2.0 s; 7-grey partial in 2.3 s. Flashes | `crowpanel_42`, `grey: true` | History matches slow greys (within 0.09); less field time than slow greys |
| Fast partials (`grey_partials: false`, default) | Clocks, counters, minute dashboards, button and dial input | 0.4 to 0.5 s (5.79") or 0.44 s (4.2"); cold partial after wake 1.3 s or 0.98 s. No flash | 54 KB / 30 KB RAM for balance tracking | Changed pixels show B&W until the next full refresh (or `request_grey_partial()` on 4.2"); full-refresh blacks clear only to grey; each push is out of balance until cancelled or settled |
| `grey_partials: true` (4.2" only) | Menus or dashboards where changed regions must keep exact greys without flashing the rest | About 3.75 s slow, or 2.3 s with `fast_greys: true` (7 greys). Flashes only the changed window; rest holds within 0.01 | `crowpanel_42`, `grey: true`, PSRAM (60 KB) | As slow as a full grey refresh; changes far apart redraw everything between them in one rectangle |
| Fast partials + `request_grey_partial()` (4.2" only) | Interactive screens that answer quickly on input, then clean up once still | 0.44 s per press, then one windowed grey refresh after a delay | `crowpanel_42`, `grey: true`, PSRAM | Single rectangle around everything changed; your YAML script triggers the settle |
| `grey_partials: glide` | Dashboards or UI updates with greys where flashing is unwelcome | Box in 0.32 to 0.38 s (4.2") or 0.5 s (5.79"); whole panel 0.93 s or 1.3 s. No flash | `grey: true`, PSRAM (about 330 KB on 4.2", 610 KB on 5.79") | Greys land near, not exact (about 0.04 out on 4.2", 0.055 on 5.79") until the next grey refresh; power loss or reflashing mid-flight loses the drive record |
| `grey_partials: live` | Short bursts of motion, sliding items in and holding them, live indicators | 23 to 27 ticks/s (4.2") or 14 to 16 ticks/s (5.79"). No flash; ink takes 0.3 to 0.5 s to settle | `grey: true`, PSRAM (about 330 KB on 4.2", 610 KB on 5.79") | Every tick is a real refresh (50,400 to 57,600/h on 5.79", 82,800 to 97,200/h on 4.2"); skips `full_update_every`; power loss or reflashing loses the drive record; never leave running unattended |
| `crowpanel_579_turbo`, `crowpanel_42_turbo` | Bench experiments only | Full 7 greys in 0.9 s (4.2") or 1.4 s (5.79"); B&W frame in 0.46 s or 0.67 to 0.87 s | `grey: true`, PSRAM, base board component listed | No charge balance: builds image retention on still pictures and may shorten panel life. Faint white ghost on 4.2"; darkest grey lands near black on 5.79". Never use on a display you rely on |

A few practical rules follow from how the panels behave:

- **A device that wakes, draws once and sleeps.** Use `crowpanel_579` or
  `crowpanel_42`. A single partial after a wake is always the balanced 25-frame
  cold partial (about 1.3 s on the 5.79", 0.98 s on the 4.2"), or on the 4.2"
  with `grey_partials: true` a grey partial over the changed rows from RTC
  hashes. It never makes a fast partial, so nothing needs settling before the
  next sleep. Add `grey: true` for anti-aliased text, and `fast_greys: true` on
  the 4.2" to halve grey refresh time.
- **A dashboard updating every minute.** Use `crowpanel_579` or `crowpanel_42`
  with `grey: true`. Leave `grey_partials: false` if only numbers or black and
  white widgets change, or use `grey_partials: glide` if grey areas change and
  you want no flash. Keep `full_update_every` set (default 30) so periodic full
  refreshes clear partial ghosting and settle glided greys exactly.
- **Something that moves (`grey_partials: live`).** Live mode runs on by itself
  while the picture changes or pixels are still arriving, and its ticks do not
  count towards `full_update_every`. Every tick is a real refresh of the panel:
  at the live demos' measured rates, 14 to 16 ticks a second on the 5.79" is
  50,400 to 57,600 refreshes an hour, and 23 ticks a second on the 4.2" is
  82,800 an hour (up to about 97,000 at 27 ticks a second). Use live mode for
  short spells while something moves, slide elements in and hold them still so
  the ink settles, and stop it when finished. Do not leave it running
  unattended.
- **Unpaid drive after a reflash or power cut.** Fast partials, glides and live
  ticks keep their balance records in RAM or PSRAM, and the balanced platforms
  settle them before a planned deep sleep or a grey refresh. Reflashing, a crash
  or cutting power while glides or live ticks are in flight wipes that record,
  so their unpaid drive stays on the glass until later refreshes wear it off
  (a faint ghost of the live demo's bouncing ball was seen under later grey
  refreshes on the 5.79" after interrupting it).
- **Turbo is for experiments only.** Switching charge balance off makes full
  refreshes several times faster at the same ±15 V voltages, at the cost of
  leaving drive on the panel every time. On a still picture, that builds image
  retention (100 turbo refreshes of a checkerboard on the 4.2" left a 0.12 ghost
  on white, still 0.02 under balanced refreshes afterwards and taking 13 minutes
  to fade to 0.004) and may shorten the panel's life. Even on one refresh, the
  4.2" leaves a faint ghost on white (0.05, fading to 0.03 after 12 s) and the
  5.79"'s darkest grey lands near black. Keep any display you rely on on a
  balanced platform.
- **Temperature.** Without a `temperature_sensor`, the 5.79"'s loaded waveforms
  assume 13 to 20 °C at 50 Hz; give it a sensor if the room runs much warmer or
  colder. The 4.2" reads its own panel temperature whenever its supplies come
  on.

## CrowPanel 5.79"

`crowpanel_579` drives the 5.79" panel, which uses two SSD1683 controllers in
cascade behind one SPI chip select. It builds on `EPaperBase` from ESPHome's
`epaper_spi`, so updates do not block: BUSY is polled from `loop()` and data
goes out in 10 ms slices. The frame buffer is 108 KB (4 bits per pixel) in
PSRAM, while the panel's own RAM holds the previous frame.

| Mode | Time (13 to 20 °C) | Behaviour |
|---|---|---|
| Full black and white (`grey: false`) | about 3.7 s | Panel OTP waveform, temperature compensated; resets registers first so it stays clean after loaded partials |
| Full four greys (`grey: true`) | about 4.2 s | 4 greys, charge balanced; the panel OTP has no grey mode (deeper white matches the OTP refresh; still quicker than Waveshare's unmodified 4.2" V2 waveform) |
| Full seven greys (`grey_levels: 7`) | 4.4 to 4.5 s | 7 greys in three passes (half steps at 125 Hz), charge balanced; see the notes for measured levels |
| Partial, first after boot or wake | about 1.3 s | Black and white, 25 frames each way, charge balanced, changed rows only; greys elsewhere survive (the panel's OTP partial fades them) |
| Partial, subsequent | 0.4 to 0.5 s | 15-frame one-way push with per-pixel balance tracking, changed rows only |
| Partial, nothing changed | about 50 ms | Drawing only; the panel is not driven |
| Glide (`grey_partials: glide`) | about 0.5 s for a box, 1.3 s whole panel | No flash: each changed pixel goes straight to its new grey, within about 0.055 on average; the next full refresh settles them |
| Live (`grey_partials: live`) | 14 to 16 ticks a second for a clock, a ball and a ticker | 8-frame ticks nudging pixels towards the latest frame; ticks on until everything arrives |

The 5.79"'s glides and live ticks run at 195 Hz, its controllers' fastest rate,
tuned on this panel, whose ink moves at about two thirds of the 4.2"'s pace;
ticks are 8 frames rather than 4, and scans and ticks run VCOM at its measured
null (`0x28`) so held whites do not darken. The SSD1683 has no partial window,
so glides settle at the next full refresh rather than in a window.

Panel wiring details, waveform notes and measurements (including the seven-grey
readings) are in [docs/crowpanel-579.md](docs/crowpanel-579.md).

## CrowPanel 4.2"

`crowpanel_42` drives the 4.2" V1.2 board (a green circular sticker on the
back), whose UC8276C has no waveforms in its OTP: every refresh runs one the
driver loads. It shares `crowpanel_epaper` and the same wiring and options as
`crowpanel_579`, with SPI at 10 MHz (the controller's spec limit). V1.0 boards
use an SSD1683 and are not supported yet.

- **The panel's own temperature.** The driver reads it through a 3-wire
  half-duplex SPI read each time the supplies come on, so it needs no
  `temperature_sensor`, and steps its frame rate smoothly in 5 Hz steps from
  50 Hz at 11 °C; `id(epd).panel_temperature()` returns it in °C.
- **VCOM at its null.** Measured on the panel, at -1.4 V a frame driving nothing
  leaves greys as they are. Elecrow's -0.6 V lightened them on every such frame
  and left a DC bias on waveforms balanced by frame count.

| Mode | Time (50 Hz, 11 °C) | Behaviour |
|---|---|---|
| Full black and white (`grey: false`) | 1.15 to 1.3 s | Balanced, Elecrow's 20/10/20 waveform |
| Full four greys (`grey: true`) | about 3.7 s | Even thirds: measured 0.00, 0.33, 0.70 and 0.95 |
| Full seven greys (`grey_levels: 7`) | about 3.95 s | Measured 0.00, 0.18, 0.34, 0.49, 0.69, 0.84 and 1.00 |
| Fast greys (`fast_greys: true`) | about 2.0 s four or seven | Two shakes and a push at 100 Hz: about twice as quick, erasing the picture before as well (within 0.09) |
| Partial, first after boot or wake | about 0.98 s | Black and white, 25 frames each way, charge balanced |
| Partial, subsequent | about 0.44 s | 12-frame one-way push with per-pixel balance tracking |
| Partial, nothing changed | about 0.09 s | Drawing only; the panel is not driven |
| Grey partial (`grey_partials: true`) | about 3.75 s slow, 2.3 s fast (7 greys) | Greys in the changed window only; outside holds within 0.01 |
| Glide (`grey_partials: glide`) | 0.32 to 0.38 s for a box, 0.93 s whole panel | No flash: each changed pixel goes straight to its new grey, within about 0.04 on average (0.13 at worst) |
| Live (`grey_partials: live`) | 23 ticks a second for a clock and a bouncing ball | 4-frame ticks at 200 Hz nudging pixels towards the latest frame; ticks on until everything arrives |

```yaml
display:
  - platform: crowpanel_42
    id: epd
    grey: true
    lambda: |-
      it.print(20, 20, id(font), Color::BLACK, "Hello");
```

The UC8276C adds two capabilities the 5.79" does not have:

- **Grey partials (`grey_partials: true`) and `request_grey_partial()`.** The
  controller can confine a refresh to a partial window, so `grey_partials: true`
  redraws only the rectangle around what changed in greys, flashing only it. It
  takes as long as a grey refresh and works across deep sleep: the driver keeps
  a hash of each row's levels in RTC memory (1.2 KB), so after a wake it
  refreshes the changed band of rows at full width. To get fast answers during
  input and clean greys afterwards, leave `grey_partials: false` and call
  `request_grey_partial()` once input stops; it redraws in greys the window
  around everything changed since the last grey refresh, including areas a fast
  partial changed and changed back, and tracks that window across deep sleep:

```yaml
script:
  - id: settle
    mode: restart  # each button press starts the wait again
    then:
      - delay: 3s
      - lambda: id(epd).request_grey_partial();
      - component.update: epd
```

- **Fast greys (`fast_greys: true`).** Shakes each pixel twice at 100 Hz and
  pushes once instead of Waveshare's many shakes, cutting four or seven greys
  from 3.7 or 3.95 s to about 2.0 s, and grey partials likewise. Drawn over
  every other grey, both fast and slow greys leave each grey within 0.09 of its
  target.

Notes and measurements are in [docs/crowpanel-42.md](docs/crowpanel-42.md).

## Glides and live ticks

Both boards share `grey_partials: glide` and `grey_partials: live` through
`crowpanel_epaper`, needing `grey: true` and PSRAM.

- **Glide (`grey_partials: glide`).** Walks each changed pixel straight from its
  old grey to its new one in binary-weighted scans, with no flash: a box in
  0.32 to 0.38 s on the 4.2" (0.93 s whole panel) and about 0.5 s on the 5.79"
  (1.3 s whole panel). Greys land near enough (about 0.04 out on average on the
  4.2", 0.055 on the 5.79") and settle exactly at the next grey refresh (full,
  or `request_grey_partial()` on the 4.2").
- **Live (`grey_partials: live`).** Each update is one short tick pushing pixels
  towards the latest frame (4 frames at 200 Hz on the 4.2", 8 frames at 195 Hz
  on the 5.79"). While the panel runs one tick, both cores draw the next frame
  and plan its tick, and the panel carries on by itself while anything changes
  or is still arriving. Draw by `millis()` and the panel animates with nothing
  asking for updates; an update asked for during a tick waits for it, so the
  latest frame wins. On the 4.2", a tick's refresh takes 31 ms plus sending its
  window at 10 MHz (`data_rate: 20MHz`, beyond the UC8276C's datasheet but clean
  in tests here, took a busy two-box animation from 15 to 16.5 ticks a second).

```yaml
display:
  - platform: crowpanel_42
    grey: true
    grey_partials: glide  # no flash; greys settle exactly at the next grey refresh
```

## Minimal configuration

Pin `external_components` to a full commit SHA or a tag; GitHub will not fetch a
short SHA. The board's USB-C port is a CH340 on UART0, which resets the board
into the bootloader automatically, so flashing needs no BOOT button. For the
4.2", use `crowpanel_42` for the component and the platform.

```yaml
external_components:
  - source: github://tommyothen/crowpanel-epaper@0a17723f4748257bb40ea4a55352615feaf6924f  # a full commit SHA or a tag
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
| `grey` | `false` | Full refreshes in four greys (about 4.2 s on 5.79", 3.7 s on 4.2") instead of black and white |
| `grey_levels` | `4` | `4` or `7`. `7` runs three passes with a half step between each of the four greys (4.4 to 4.5 s on 5.79", 3.95 s on 4.2"); needs `grey: true` |
| `grey_partials` | `false` | `glide`: no-flash glides to the new grey. `live`: continuous ticks towards the latest frame, for things that move. On `crowpanel_42`, also `true` for windowed grey partials (`crowpanel_579` rejects `true`). Needs `grey: true` and PSRAM |
| `temperature_sensor` | none | Sensor ID for panel temperature in °C. On 5.79", switches self-balanced waveforms across temperature bands (without one, 50 Hz, 13 to 20 °C). On 4.2", overrides the panel's own sensor |
| `full_update_every` | `30` | Updates per full refresh (`1` to `255`); `1` makes every update full. Live ticks do not increment this count |
| `update_interval` | `never` | Most devices decide for themselves when to draw |
| `rotation` | `0` | Display rotation in degrees |
| `cs_pin` | `GPIO45` | SPI chip select |
| `dc_pin` | `GPIO46` | Data/command pin |
| `reset_pin` | `GPIO47` | Panel reset pin |
| `busy_pin` | `GPIO48` | Panel BUSY input (inverted by default on `crowpanel_42`, where the UC8276C holds BUSY low while working) |
| `power_pin` | `GPIO7` | Switches the panel's supply/ground; must be high before anything answers |
| `data_rate` | `20MHz` | SPI clock rate (`10MHz` by default on `crowpanel_42`, the UC8276C's spec limit) |
| `lambda`, `pages` | none | Standard ESPHome drawing options; at most one may be set |

`crowpanel_42` adds one option and two calls:

| Option | Default | Notes |
|---|---|---|
| `fast_greys` | `false` | Grey refreshes in two shakes and a push at 100 Hz: about 2.0 s for four or seven greys, and grey partials likewise; needs `grey: true` |

`id(epd).request_grey_partial()` and `id(epd).panel_temperature()` are the
4.2"'s own calls.

`crowpanel_579_turbo` and `crowpanel_42_turbo` take their board's options and
require `grey: true`.

## Drawing

Draw with `Color::BLACK` on `Color::WHITE` paper.

- With four greys, colours map by luma: below 64 is black, below 128 dark grey,
  below 192 light grey, and the rest white. Fonts with `bpp: 2` give
  anti-aliased text on a grey refresh. On the 5.79" the four greys keep
  Waveshare's spacing, where dark grey sits darker than an even third and makes
  anti-aliased text look bold; on the 4.2" they are even thirds.
- With `grey_levels: 7`, luma maps to seven even steps, and `bpp: 4` fonts use
  them. Seven greys space the four base levels evenly first, so dark grey
  stands clear of black, then add the half steps.
- Black and white partial refreshes drive only pixels whose black or white
  reading changes (white from level 4 up). Greys elsewhere stay as the last
  grey refresh left them; a changed pixel shows as black or white until the
  next full refresh, or on the 4.2" a grey partial.

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
driver finishes any pending plane copy, settles any glides, live ticks or
fast-partial pixels left out of balance (on balanced platforms), puts the
5.79"'s controllers into deep sleep mode 1 (which retains RAM) or switches the
4.2"'s supplies off and leaves its controller in standby (its own deep sleep
loses the RAM within 30 minutes), and holds `power_pin` and `reset_pin` high
through sleep. On wake, the first update can be partial with no flash, and
`full_update_every`, the row hashes and (on the 4.2") the dirty window for
`request_grey_partial()` carry on across wakes.

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
glass, unsettled pixels stay uncancelled, and the driver makes the next update
a full one.

## Turbo

`crowpanel_579_turbo` and `crowpanel_42_turbo` are the same drivers with charge
balance switched off, at the same ±15 V voltages. They are for experiments:
every refresh leaves its drive on the panel, which builds image retention on a
still picture and may shorten the panel's life. Keep a display you rely on on
the balanced platform. Each takes its board's options, needs `grey: true` and
PSRAM, and needs its board's component listed too:

```yaml
external_components:
  - source: github://tommyothen/crowpanel-epaper@<a full commit SHA or a tag>
    components: [crowpanel_42_turbo, crowpanel_42, crowpanel_epaper]

display:
  - platform: crowpanel_42_turbo
    grey: true
```

A turbo full refresh is one push of every pixel to black or white, as far as it
has to go, then the greys dosed from white as a glide would. From the far end a
pixel gets the tuning's reset (64 frames on the 4.2", 96 on the 5.79"); a white
or grey that is already white gets half of that; a black already black gets
three-eighths. The planes pick which of those four each pixel gets, so the
reset costs one refresh of its longest push. Nothing is recorded, cancelled or
settled: partials push one way even from cold, glides never stop to settle, and
deep sleep needs no settling first. On the 4.2", a grey partial is a turbo
refresh of its window. Live ticks run no faster than balanced ones, which were
already at the refresh's limit.

Measured on the bench against the balanced platforms (both running seven greys,
with 0 the balanced refresh's black and 1 its white, each picture drawn over a
checkerboard):

| | 4.2" balanced | 4.2" turbo | 5.79" balanced | 5.79" turbo |
|---|---|---|---|---|
| Full refresh, seven greys | 3.9 s | 0.9 s | 4.5 s | 1.4 s |
| Full refresh, black and white frame | 3.9 s | 0.46 s | 4.4 to 4.6 s | 0.67 to 0.87 s |
| White | 1.00 | 1.02 to 1.07 | 1.00 | 0.99 to 1.01 |
| Ghost of the checkerboard on white | 0.00 | 0.05, 0.03 after 12 s | 0.00 | 0.01 |
| Ghost on black | 0.00 | 0.00 | 0.015 | 0.01 |
| Greys apart by what was there before | 0.03 | 0.03 | 0.07 | 0.04 |

What the bench runs show:

- **Image retention on a still picture.** A hundred turbo refreshes of the
  checkerboard in a row on the 4.2" (about 50 s) left a ghost of 0.12 on the
  white after it, and 0.02 under the balanced platform's refreshes afterwards,
  fading to 0.004 within 13 minutes.
- **White ghost on the 4.2".** A pixel arriving from black lands whiter than a
  white touched up in place, leaving a faint ghost (0.05, settling to 0.03 in
  12 s).
- **Darkest grey on the 5.79".** The darkest grey lands near black, because a
  freshly reset white takes black faster than the settled white glides were
  measured from.

The full measurements are in [docs/crowpanel-42.md](docs/crowpanel-42.md) and
[docs/crowpanel-579.md](docs/crowpanel-579.md).

## Looking after the panel

On the turbo platforms, only discharge and power wind-down, VCOM's null, the
watchdog and boot reset, temperature compensation and periodic full refreshes
apply. Nothing is balanced, cancelled or settled.

- **Charge-balanced waveforms.** The cold partial, four-grey, seven-grey and
  fast-grey waveforms give every pixel equal frames at +15 V and -15 V on every
  refresh, with VCOM held at its DC level. Compile-time `static_assert` checks
  verify each table's net drive and timing (all tables equal on the 5.79", VCOM
  at least as long as every pixel table on the 4.2"), so a change that breaks
  balance does not compile.
- **Fast partials and cancelling.** Once the driver knows what the glass shows
  (after the first update since a boot or wake), a partial pushes each changing
  pixel one way only: 15 frames on the 5.79", 12 on the 4.2", where a balanced
  cold partial drives 25 each way. A bit per pixel tracks whether that pixel is
  a push out of balance; each fast change flips the bit, so no pixel is ever
  more than one push out however long the device runs. Before the next full
  refresh (or a 4.2" grey partial covering it), a matching push back cancels
  them, hidden by the refresh's flash. Before a deep sleep, which would lose
  that bit map, a settling pass cancels them instead: only those pixels blink
  (40 frames over and 25 back on the 5.79", 37 and 25 on the 4.2") and land
  where they were. A device that wakes, draws once and sleeps never makes a
  fast partial.
- **Glides and live ticks.** Glides and ticks push pixels by varying amounts, so
  both boards record each pixel's net drive in PSRAM, a byte a pixel (frames at
  200 Hz on the 4.2", 195 Hz on the 5.79"). Pushing a pixel already at black or
  white further the same way pays back drive pointing the other way without
  changing what you see. Before any grey refresh covering a pixel (full, or a
  windowed grey partial on the 4.2"), scans cancel its recorded drive, hidden by
  the refresh. A glide that would take a pixel past 127 frames either way
  settles instead (in its window on the 4.2", by a full refresh on the 5.79");
  in live mode, a pixel at the limit holds until settled. Planned deep sleep
  settles glides and ticks first; cutting power or reflashing mid-flight loses
  the PSRAM record and leaves unpaid drive on the glass until later refreshes
  wear it off.
- **Discharge and power wind-down.** On the 5.79" every loaded waveform uses
  end option `0x22` to discharge the pixels after the last phase. The ±15 V
  supplies stay on for 5 s after an update so a follow-up skips switching them
  on (0.1 s on the 5.79", 0.08 s on the 4.2"), capped at 60 s at a stretch
  however often updates arrive. The 5.79"'s controllers enter deep sleep 10 s
  after the supplies switch off.
- **VCOM at its null.** On the 4.2", VCOM is set to -1.4 V, where a frame
  driving 0 V leaves a pixel as it is, so waveforms balance in fact rather than
  only by frame count and pixels outside a grey partial window keep their greys.
  On the 5.79", scans and ticks run VCOM at its null (`0x28`), measured over
  800 held frames so untouched white holds during long live spells.
- **Update watchdog and boot reset.** If an update does not finish in 20 s (a
  loose ribbon or a failed controller), the driver logs an error, sets a
  warning status, resets and sleeps the controllers so the supplies switch off
  and the device can still sleep, and starts again from a full refresh. On any
  boot that is not a wake from deep sleep, it resets and sleeps them too, in
  case a reboot or OTA update caught them with the supplies on.
- **Temperature compensation.** Particles move faster in the warm and slower in
  the cold. When `grey` is off on the 5.79", the panel's OTP full refresh
  compensates on its own. With `temperature_sensor` set on the 5.79", the
  self-balanced waveforms switch frame rate across the panel maker's bands:
  37.5 Hz below 12 °C, 50 Hz from 12 to under 21 °C, 75 Hz from 21 to under
  33 °C, and 100 Hz from 33 °C; the seven-grey half steps scale their frame
  counts to match. Without a sensor, the 5.79" runs at 50 Hz (13 to 20 °C). The
  4.2" reads its panel's own temperature as its supplies come on and adjusts in
  5 Hz steps along the same maker's curve, from 50 Hz at 11 °C where it was
  tuned. Fast partials, cancelling and settling always run at 50 Hz (and glides
  and cancelling scans at 200 Hz or 195 Hz) so pushes and cancellations match
  even hours later at another temperature.
- **Periodic full refreshes.** `full_update_every` caps a run of partials,
  across deep sleep too, to reset deep blacks and clear what partials and glides
  leave behind. Live ticks do not count towards it, so nothing interrupts an
  animation until you request a refresh.

## Examples

Four demos need no network: flash one over USB and drive it from the board's own
buttons (or let live mode animate on its own).

- [examples/demo-579.yaml](examples/demo-579.yaml): the dial counts with fast
  partials, the press swaps four and seven greys, MENU runs a grey refresh, EXIT
  sleeps for 10 s; each refresh's time shows on the panel after it lands.
- [examples/demo-42.yaml](examples/demo-42.yaml): prev and next count; OK cycles
  between glides, fast partials that settle into greys 3 s after the last press,
  grey partials every time, and fast partials alone; Home swaps four and seven
  greys, Exit sleeps for 10 s; uses fast greys throughout and shows the panel's
  temperature.
- [examples/live-579.yaml](examples/live-579.yaml) and
  [examples/live-42.yaml](examples/live-42.yaml): live mode with nothing asking
  for updates after the first: a clock, a bouncing ball and, on the 5.79", a
  ticker that slides each item in and holds it, with the live tick rate shown
  once a second.

## Test card

Both test cards need no network: flash and read logs over USB with
`esphome run test-card.yaml` (5.79") or `esphome run test-card-42.yaml` (4.2").

- [test-card.yaml](test-card.yaml) draws a border on the outermost pixels with
  the four corners named, four greys on each half, anti-aliased text in black
  and dark grey, a counter across the seam and a moving box, with a grey refresh
  every sixth update and partials between. A missing edge or mirrored text means
  the halves or rotation are wrong; the greys should match on both sides of the
  seam.
- [test-card-42.yaml](test-card-42.yaml) does the same for the 4.2" with seven
  greys, the panel's own temperature and grey partials.

## Limitations

- **ESPHome version.** Tested with ESPHome 2026.10. `epaper_spi`'s base class
  changes between releases, so older versions may not compile the component.
- **Black and white partials.** Any pixel that changes during a black and white
  partial refresh shows as black or white until the next full refresh, or on the
  4.2" a grey partial.
- **Clearing full-refresh blacks.** A large black area drawn by a full refresh
  fades only to grey when a fast partial clears it, while black drawn by a
  partial clears cleanly. Keep large black shapes still between full refreshes,
  or ask for a full refresh with `epaper_spi.full_update_next` when they move.
- **No internal temperature readback on the 5.79".** Provide a
  `temperature_sensor` if it runs outside 13 to 20 °C. The 4.2" reads its own
  through a 3-wire read on the same SPI pins; the 5.79" could too, but does not
  yet.
- **RAM and PSRAM.** Tracking unsettled fast-partial pixels allocates 54 KB
  alongside the 108 KB frame buffer on the 5.79", and 30 KB alongside 60 KB on
  the 4.2"; with `grey: true` and PSRAM, the driver also keeps a copy of the
  last grey frame (108 KB on the 5.79", 60 KB on the 4.2"). Glides, live ticks
  and turbo need PSRAM: about 330 KB on the 4.2" and 610 KB on the 5.79". If an
  allocation fails, fast partials fall back to balanced partials, and grey
  partials or glides to black and white.
- **Sleeping mid-update, or losing power mid-glide.** Entering deep sleep while
  `epaper_spi.is_updating` is true leaves unsettled fast-partial pixels
  uncancelled and forces a full refresh on the next wake. Reflashing or cutting
  power while glides or live ticks have unpaid drive loses the PSRAM record;
  that drive stays on the glass until later refreshes clear it.
- **Greys tuned on a camera.** Every grey, glide and tick was tuned on a laptop
  camera's readings at a fixed exposure. Measured as perceptual lightness
  instead, both panels' dark greys sit light: the 5.79"'s seven greys by up to
  0.16 of the black to white range, the 4.2"'s by 0.09. See "Measuring greys"
  in [docs/crowpanel-579.md](docs/crowpanel-579.md#measuring-greys).
- **CrowPanel 4.2".** V1.0 boards (SSD1683) are not supported, and its frame
  rate by temperature borrows the 5.79" panel's maker's curve until checked in
  a warm room.
- **Glides and live ticks** land near enough, not exact, until the next grey
  refresh settles them (on the 5.79", the next full refresh). Text scrolling
  continuously smears on either panel because the ink needs 0.3 to 0.5 s to
  settle; slide items in and hold them instead.

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
