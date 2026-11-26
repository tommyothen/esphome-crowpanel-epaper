# crowpanel-epaper

ESPHome external display components for ELECROW's CrowPanel ESP32-S3 e-paper
HMI boards. Tested with ESPHome 2026.10.

## Boards

| Board | Resolution | Component | Status |
|---|---|---|---|
| CrowPanel 5.79" | 792x272 | `crowpanel_579` | Ready |
| CrowPanel 4.2" V1.2 (UC8276C) | 400x300 | `crowpanel_42` | Ready |

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
| Full four greys (`grey: true`) | about 3.9 s | 4 greys, charge balanced; the panel OTP has no grey mode (1.8 s faster than Waveshare's unmodified 4.2" V2 waveform, with the same measured greys) |
| Full seven greys (`grey_levels: 7`) | about 4.4 s | 7 evenly spaced greys in three passes, charge balanced |
| Partial, first after boot or wake | about 1.3 s | Black and white, charge balanced, changed rows only; greys elsewhere survive (the panel's OTP partial fades them) |
| Partial, subsequent | 0.4 to 0.5 s | One-way push with per-pixel balance tracking, changed rows only |
| Partial, nothing changed | about 50 ms | Drawing only; the panel is not driven |

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
| Fast greys (`fast_greys`) | 1.2 s four, 1.25 s seven | One shake instead of many: about three times quicker, a little more of the previous picture left |

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

And **fast greys**: with `fast_greys: true`, grey refreshes shake each pixel once
instead of many times, so four greys take 1.2 s and seven 1.25 s, and grey
partials likewise. The slow ones erase the previous picture a little better
(within 0.03 against 0.06).

A grey partial takes as long as a grey refresh, and works across deep sleep too:
the driver keeps a record of each row's greys in RTC memory, so after a wake it
redraws the band of rows that changed. Without it, partials are the fast black
and white ones.

Notes and measurements are in [docs/crowpanel-42.md](docs/crowpanel-42.md).

## Minimal configuration

Pin `external_components` to a full commit SHA or a tag; GitHub will not fetch a short SHA. The board's USB-C port is a CH340
on UART0, which resets the board into the bootloader automatically, so flashing
needs no BOOT button.

```yaml
external_components:
  - source: github://tommyothen/crowpanel-epaper@211bc65a480454ce0d4efb2f92e3fb401e09e7b7  # a full commit SHA or a tag
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

All pins default to the 5.79" board's fixed wiring.

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
| `lambda`, `pages` | none | Standard ESPHome drawing options; at most one may be set |

## Drawing

Draw with `Color::BLACK` on `Color::WHITE` paper.

- With four greys, colours map by luma: below 64 is black, below 128 dark grey,
  below 192 light grey, and the rest white. Fonts with `bpp: 2` give
  anti-aliased text on a grey refresh. The four greys keep Waveshare's spacing,
  where dark grey sits close to black and makes anti-aliased text look bold.
- With `grey_levels: 7`, luma maps to seven even steps, and `bpp: 4` fonts use
  them. Seven greys space the four base levels evenly first, so dark grey
  stands clear of black, then add the half steps.
- Partial refreshes are black and white and drive only pixels whose black or
  white reading changes (white from level 4 up). Greys elsewhere stay as the
  last grey refresh left them; a changed pixel shows as black or white until
  the next full refresh.

## Front controls

The dial and the two buttons are plain GPIOs, active low with the board's own
pull-ups, so they work with ESPHome's `gpio` binary sensor and can wake the
board from deep sleep with `esp32_ext1_wakeup` in `ANY_LOW` mode.

| Control | GPIO |
|---|---|
| Dial up | GPIO6 |
| Dial down | GPIO4 |
| Dial press | GPIO5 |
| MENU | GPIO2 |
| EXIT | GPIO1 |

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
out of balance, puts the controllers into deep sleep mode 1 (which retains
RAM), and holds `power_pin` and `reset_pin` high through sleep. On wake, the
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
  each table's net drive and check that every table in a waveform runs to the
  same timing, so a change that breaks balance does not compile.
- **Fast partials and cancelling.** Once the driver knows what the glass shows
  (after the first update since a boot or wake), a partial pushes each changing
  pixel one way only: 15 frames, where a balanced partial drives 25 each way. A bit per pixel tracks
  whether that pixel is a push out of balance; each fast change flips the bit,
  so no pixel is ever more than one push out however long the device runs.
  Before the next full refresh, a 15-frame push back cancels them, hidden by
  the refresh's flash. Before a deep sleep, which would lose that bit map, a
  settling pass cancels them instead: only those pixels blink (40 frames over
  and 25 back) and land where they were. A device that wakes, draws once and
  sleeps never makes a fast partial.
- **Discharge and power wind-down.** Every loaded waveform uses end option
  `0x22` to discharge the pixels after the last phase. The ±15 V supplies stay
  on for 5 s after an update so a follow-up skips switching them on (0.1 s),
  capped at 60 s at a stretch however often updates arrive. The controllers
  enter deep sleep 10 s after the supplies switch off.
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
- **Periodic full refreshes.** `full_update_every` caps a run of partials,
  across deep sleep too, to reset deep blacks and clear what partials leave
  behind.

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
- **No internal temperature readback.** The board wires the SPI data line to
  one GPIO as an output, so reading the controllers' internal sensor would
  require bit-banging. Provide a `temperature_sensor` if the board runs outside
  13 to 20 °C.
- **RAM for fast partials.** Tracking unsettled pixels allocates an extra 54 KB
  alongside the 108 KB frame buffer. If that allocation fails, fast partials
  are disabled and partials run balanced.
- **Sleeping mid-update.** Entering deep sleep while `epaper_spi.is_updating`
  is true leaves any unsettled fast-partial pixels uncancelled and forces a
  full refresh on the next wake.
- **CrowPanel 4.2".** V1.0 boards (SSD1683) are not supported, and its frame rate by
  temperature borrows the 5.79" panel's maker's curve until checked in a warm room.

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

MIT. See [LICENSE](LICENSE).
