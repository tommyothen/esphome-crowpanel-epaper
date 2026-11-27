# How it works

## The 5.79"

`crowpanel_579` drives the 5.79" panel, which uses two SSD1683 controllers in
cascade behind one SPI chip select. It builds on `EPaperBase` from ESPHome's
`epaper_spi`, so updates do not block: BUSY is polled from `loop()` and data
goes out in 10 ms slices. The frame buffer is 108 KB (4 bits per pixel) in
PSRAM, while the panel's own RAM holds the previous frame.

| Mode | Time (13 to 20 °C) | Behaviour |
|---|---|---|
| Full black and white (`grey: false`) | about 3.7 s | Panel OTP waveform, temperature compensated; resets registers first so it stays clean after loaded partials |
| Full four greys (`grey: true`) | about 4.2 s | Charge balanced; the panel's OTP has no grey mode. Measured 0.02, 0.26, 0.62 and 1.00 in L* |
| Full seven greys (`grey_levels: 7`) | 4.4 to 4.5 s | Three passes (half steps at 125 Hz), charge balanced. Measured 0.01, 0.18, 0.37, 0.54, 0.69, 0.83 and 1.00 in L* |
| Partial, first after boot or wake | about 1.3 s | Black and white, 25 frames each way, charge balanced, changed rows only; greys elsewhere survive (the panel's OTP partial fades them) |
| Partial, subsequent | 0.4 to 0.5 s | 15-frame one-way push with per-pixel balance tracking, changed rows only |
| Partial, nothing changed | about 50 ms | Drawing only; the panel is not driven |
| Glide (`grey_partials: glide`) | about 0.5 s for a box, 1.3 s whole panel | No flash: each changed pixel goes straight to its new grey, within about 0.04 on average (0.12 at worst); the next full refresh settles them |
| Live (`grey_partials: live`) | 14 to 16 ticks a second for a clock, a ball and a ticker | 8-frame ticks nudging pixels towards the latest frame; ticks on until everything arrives |

The 5.79"'s glides and live ticks run at 195 Hz, its controllers' fastest rate,
tuned on this panel, whose ink moves at about two thirds of the 4.2"'s pace;
ticks are 8 frames rather than 4, and scans and ticks run VCOM at its measured
null (`0x28`) so held whites do not darken. The SSD1683 has no partial window,
so glides settle at the next full refresh rather than in a window.

Panel wiring details, waveform notes, measurements and how the greys were
measured are in [the 5.79" notes](crowpanel-579.md).

## The 4.2"

`crowpanel_42` drives the 4.2" V1.2 board (the version is printed on the
back, under the SKU), whose UC8276C has no waveforms in its OTP: every
refresh runs one the driver loads. It shares `crowpanel_epaper` and the same wiring and options as
`crowpanel_579`, with SPI at 10 MHz (the controller's spec limit). V1.0 boards
use an SSD1683 and are not supported yet.

- **The panel's own temperature.** The driver reads it through a 3-wire
  half-duplex SPI read each time the supplies come on, so it needs no
  `temperature_sensor`, and steps its frame rate smoothly in 5 Hz steps from
  50 Hz at 11 °C; `id(epd).panel_temperature()` returns it in °C. The 5.79"'s
  controllers could be read the same way, but that read has not been built
  for them yet, so the 5.79" takes a `temperature_sensor` instead.
- **VCOM at its null.** Measured on the panel, at -1.4 V a frame driving nothing
  leaves greys as they are. Elecrow's -0.6 V lightened them on every such frame
  and left a DC bias on waveforms balanced by frame count.

| Mode | Time (50 Hz, 11 °C) | Behaviour |
|---|---|---|
| Full black and white (`grey: false`) | 1.15 to 1.3 s | Balanced, Elecrow's 20/10/20 waveform |
| Full four greys (`grey: true`) | about 3.7 s | Measured 0.03, 0.33, 0.74 and 0.98 in L* |
| Full seven greys (`grey_levels: 7`) | about 3.95 s | Measured 0.01, 0.16, 0.34, 0.46, 0.67, 0.84 and 0.99 in L* |
| Fast greys (`fast_greys: true`) | about 2.0 s four or seven | Two shakes and a push at 100 Hz: about twice as quick, erasing the picture before as well (within 0.09) |
| Partial, first after boot or wake | about 0.98 s | Black and white, 25 frames each way, charge balanced |
| Partial, subsequent | about 0.44 s | 12-frame one-way push with per-pixel balance tracking |
| Partial, nothing changed | about 0.09 s | Drawing only; the panel is not driven |
| Grey partial (`grey_partials: true`) | about 3.75 s slow, 2.3 s fast (7 greys) | Greys in the changed window only; outside holds within 0.01 |
| Glide (`grey_partials: glide`) | 0.32 to 0.38 s for a box, 0.93 s whole panel | No flash: each changed pixel goes straight to its new grey, within about 0.04 on average (0.11 at worst) |
| Live (`grey_partials: live`) | 23 ticks a second for a clock and a bouncing ball | 4-frame ticks at 200 Hz nudging pixels towards the latest frame; ticks on until everything arrives |

```yaml
display:
  - platform: crowpanel_42
    id: epd
    grey: true
    lambda: |-
      it.print(20, 20, id(font), Color::BLACK, "Hello");
```

The 4.2" has two things the 5.79" does not: the first because its controller
can do something the SSD1683 cannot, the second because it has not been done on
the 5.79" yet.

- **Grey partials (`grey_partials: true`) and `request_grey_partial()`.** The
  UC8276C can confine a refresh to a partial window, which the 5.79"'s SSD1683s
  cannot: every refresh there drives every pixel. So `grey_partials: true`
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
  target. They were tuned frame by frame on this panel's ink; the 5.79"'s moves
  at about two thirds of the pace, so it needs its own tuning, not yet done.

Notes and measurements are in [the 4.2" notes](crowpanel-42.md).

## Glides and live ticks

Both boards share `grey_partials: glide` and `grey_partials: live` through
`crowpanel_epaper`, needing `grey: true` and PSRAM.

- **Glide (`grey_partials: glide`).** Walks each changed pixel straight from its
  old grey to its new one in binary-weighted scans, with no flash: a box in
  0.32 to 0.38 s on the 4.2" (0.93 s whole panel) and about 0.5 s on the 5.79"
  (1.3 s whole panel). Greys land near enough (about 0.04 out on average on
  both) and settle exactly at the next grey refresh (full,
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
