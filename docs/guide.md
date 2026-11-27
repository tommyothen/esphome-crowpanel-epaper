# Guide

## Options

All pins default to the boards' fixed wiring, which the two share.

| Option | Default | Notes |
|---|---|---|
| `grey` | `false` | Full refreshes in four greys (about 4.2 s on 5.79", 3.7 s on 4.2") instead of black and white |
| `grey_levels` | `4` | `4` or `7`. `7` runs three passes with a half step between each of the four greys (4.4 to 4.5 s on 5.79", 3.95 s on 4.2"); needs `grey: true` |
| `grey_partials` | `false` | `glide`: no-flash glides to the new grey. `live`: continuous ticks towards the latest frame, for things that move. On `crowpanel_42`, also `true` for windowed grey partials (`crowpanel_579` rejects `true`: its controllers have no partial window). Needs `grey: true` and PSRAM |
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
4.2"'s own calls. Why only the 4.2":

- **`fast_greys`** is a waveform tuned frame by frame on the 4.2"'s ink. The
  5.79"'s ink moves at about two thirds of that pace, so it needs its own
  tuning, which has not been done.
- **`request_grey_partial()`** refreshes a window of the panel in greys. The
  5.79"'s SSD1683s have no partial window: every refresh drives every pixel.
- **`panel_temperature()`** reads the controller's own sensor back over the SPI
  data line. The 5.79"'s controllers could answer the same way, but that read
  has not been built for them yet.

`crowpanel_579_turbo` and `crowpanel_42_turbo` take their board's options and
require `grey: true`.

## Drawing

Draw with `Color::BLACK` on `Color::WHITE` paper.

- With four greys, colours map by luma: below 64 is black, below 128 dark grey,
  below 192 light grey, and the rest white. Fonts with `bpp: 2` give
  anti-aliased text on a grey refresh. On the 5.79" the four greys keep
  Waveshare's spacing, where dark grey sits darker than an even third and makes
  anti-aliased text look bold; on the 4.2" they are near even thirds.
- With `grey_levels: 7`, luma maps to seven even steps, and `bpp: 4` fonts use
  them. Seven greys space the four base levels evenly first, so dark grey
  stands clear of black, then add the half steps.
- Black and white partial refreshes drive only pixels whose black or white
  reading changes (white from level 4 up). Greys elsewhere stay as the last
  grey refresh left them; a changed pixel shows as black or white until the
  next full refresh, or on the 4.2" a grey partial.

## Buttons

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

## Examples

Every example needs no network: flash it over USB with
`esphome run examples/<name>.yaml`, then use the board's own buttons, or let
live mode animate on its own.

- [demo-579.yaml](../examples/demo-579.yaml): the dial counts with fast
  partials, the press swaps four and seven greys, MENU runs a grey refresh, EXIT
  sleeps for 10 s; each refresh's time shows on the panel after it lands.
- [demo-42.yaml](../examples/demo-42.yaml): prev and next count; OK cycles
  between glides, fast partials that settle into greys 3 s after the last press,
  grey partials every time, and fast partials alone; Home swaps four and seven
  greys, Exit sleeps for 10 s; uses fast greys throughout and shows the panel's
  temperature.
- [live-579.yaml](../examples/live-579.yaml) and
  [live-42.yaml](../examples/live-42.yaml): live mode with nothing asking
  for updates after the first: a clock, a bouncing ball and, on the 5.79", a
  ticker that slides each item in and holds it, with the live tick rate shown
  once a second.
- [test-card-579.yaml](../examples/test-card-579.yaml) and
  [test-card-42.yaml](../examples/test-card-42.yaml): bring-up test cards,
  with logs over USB. A border on the outermost pixels with the corners named,
  greys, anti-aliased text in black and dark grey, a counter and a moving box,
  with a grey refresh every sixth update and partials between. A missing edge
  or mirrored text means the rotation (or on the 5.79", the halves) is wrong;
  the 5.79"'s greys should match on both sides of the seam. The 4.2"'s shows
  seven greys, its own temperature and grey partials.

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
- **Greys measured on a camera.** Greys, glides and ticks are tuned in
  perceptual lightness (CIE L*) from photos at four exposures, with the
  camera's response fitted, so the numbers here are what an eye sees as even
  steps, within about 0.04. The 4.2"'s four-grey light grey sits 0.07 light,
  and the 5.79"'s four greys 0.07 and 0.05 dark: one frame, the finest step
  there, moves them further. See "Measuring greys" in
  [the 5.79" notes](crowpanel-579.md#measuring-greys).
- **CrowPanel 4.2".** V1.0 boards (SSD1683) are not supported, and its frame
  rate by temperature borrows the 5.79" panel's maker's curve until checked in
  a warm room.
- **Glides and live ticks** land near enough, not exact, until the next grey
  refresh settles them (on the 5.79", the next full refresh). Text scrolling
  continuously smears on either panel because the ink needs 0.3 to 0.5 s to
  settle; slide items in and hold them instead.
