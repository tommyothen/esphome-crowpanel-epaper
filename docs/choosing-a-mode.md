# Choosing a mode

Start with the balanced platform for your board, `crowpanel_579` or
`crowpanel_42`, and pick a mode by how often the picture changes and whether it
needs greys. Each mode below says what it is for, what it costs and what to
watch.

## Black and white

`grey: false`, the default.

- **For:** plain black and white, or the quickest full refresh on the 4.2",
  which runs Elecrow's one-second waveform where the 5.79" uses its panel's own,
  slower but adjusted for temperature by the panel itself.
- **Full refresh:** about 3.7 s on the 5.79" (the panel's own), 1.15 to 1.3 s on
  the 4.2". Flashes.
- **Needs:** nothing extra; fast partials take 54 KB (5.79") or 30 KB (4.2") of RAM.
- **Watch:** no greys, so no anti-aliased text.
- **Over Elecrow's own code:** on the 5.79" the full refresh is the panel's own,
  as in Elecrow's demos, but the partials are not. The panel's own partial
  drives every pixel, so greys fade a little with each one (eleven took a light
  grey nearly to white). This driver's partials push only the pixels that change,
  record each push and pay it back, send only the rows that changed, and skip
  the refresh when nothing has. On the 4.2" the full refresh is Elecrow's waveform,
  run with VCOM at the null measured on the panel (-1.4 V): Elecrow's -0.6 V
  lightened greys on every frame and left a DC bias.

## Four greys

`grey: true`

- **For:** dashboards, anti-aliased text (`bpp: 2` fonts), and displays that
  wake, draw and sleep.
- **Full refresh:** about 4.2 s (5.79") or 3.7 s (4.2"). Flashes.
- **Watch:** on the 5.79" dark grey sits darker than an even third; on the 4.2"
  the greys are near even thirds.

## Seven greys

`grey_levels: 7`, with `grey: true`.

- **For:** shading, pictures and `bpp: 4` text.
- **Full refresh:** 4.4 to 4.5 s (5.79") or 3.95 s (4.2"), in three passes.
  Flashes.
- **Watch:** a little slower than four greys.

## Fast greys (4.2" only)

`fast_greys: true`

- **For:** 4.2" dashboards or pictures that run grey refreshes often.
- **Full refresh:** four or seven greys in about 2 s; a seven-grey partial in
  2.3 s. Flashes.
- **Watch:** nothing much: it erases the picture before as well as the slow
  greys do, each grey within 0.09.
- **Why only the 4.2":** it is a waveform of its own, two shakes and one push in
  place of Waveshare's many shakes, tuned frame by frame on the 4.2"'s ink. The
  5.79"'s ink moves at about two thirds of that pace, so it needs its own
  tuning, which has not been done; its grey refresh already shakes once rather
  than Waveshare's five times.

## Fast partials

The default between full refreshes (`grey_partials: false`).

- **For:** clocks, counters, minute dashboards, buttons and the dial.
- **Each update:** 0.4 to 0.5 s (5.79") or 0.44 s (4.2"), no flash. The first
  after a boot or wake is a balanced 1.3 s or 0.98 s.
- **Needs:** 54 KB or 30 KB of RAM to track balance.
- **Watch:** changed pixels show black or white until the next full refresh (or
  `request_grey_partial()` on the 4.2"). A large black area from a full refresh
  clears only to light grey. Each push is out of balance until it is cancelled
  or settled, which the driver does for you.

## Grey partials (4.2" only)

`grey_partials: true`

- **For:** menus or dashboards whose changed regions must keep exact greys,
  without flashing the rest.
- **Each update:** about 3.75 s, or 2.3 s with fast greys (seven greys). Only
  the changed window flashes; the rest holds within 0.01.
- **Needs:** PSRAM (60 KB).
- **Watch:** as slow as a grey refresh, and changes far apart redraw everything
  between them in one rectangle.
- **Why only the 4.2":** its controller can refresh just a window of the panel.
  The 5.79"'s two SSD1683s have no partial window: every refresh drives every
  pixel, so a grey refresh there always flashes the whole panel.

## Fast partials, then a grey partial (4.2" only)

Fast partials, with `request_grey_partial()` once input stops.

- **For:** screens that should answer at once on input and clean up once still.
- **Each update:** 0.44 s a press, then one grey refresh of the changed window
  after a delay.
- **Needs:** PSRAM.
- **Watch:** one rectangle around everything changed, and your YAML decides
  when to settle (see the [guide](guide.md)).
- **Why only the 4.2":** the settle is a grey partial, which needs the partial
  window the 5.79" lacks (see above).

## Glides

`grey_partials: glide`

- **For:** dashboards or interfaces with greys, where a flash is unwelcome.
- **Each update:** a box in 0.32 to 0.38 s (4.2") or 0.5 s (5.79"); the whole
  panel in 0.93 s or 1.3 s. No flash.
- **Needs:** PSRAM, about 330 KB (4.2") or 610 KB (5.79").
- **Watch:** greys land near, not exact (about 0.04 out), until the next grey
  refresh. A reflash or power cut mid-glide loses the drive record.

## Live

`grey_partials: live`

- **For:** short bursts of motion, items that slide in and hold, live
  indicators.
- **Each update:** 22 to 27 ticks a second (4.2") or 14 to 16 (5.79"), no
  flash; the ink takes 0.3 to 0.5 s to settle.
- **Needs:** PSRAM, as for glides.
- **Watch:** every tick is a real refresh, 50,000 to 97,000 an hour, and ticks
  do not count towards `full_update_every`. Use it while something moves, then
  stop; never leave it running unattended. A reflash or power cut mid-tick
  loses the drive record.

## Turbo (experimental)

`crowpanel_579_turbo` or `crowpanel_42_turbo`.

- **For:** bench experiments only.
- **Full refresh:** seven greys in 0.9 s (4.2") or 1.4 s (5.79"); black and
  white in 0.46 s or 0.67 to 0.87 s.
- **Needs:** `grey: true`, PSRAM, and the board's own component listed too.
- **Watch:** no charge balance, so it builds image retention on a still picture
  and may shorten the panel's life. Never use it on a display you rely on; see
  [turbo](turbo.md).

## Rules of thumb

- **A device that wakes, draws once and sleeps.** Use `crowpanel_579` or
  `crowpanel_42` with `grey: true`. The one update after a wake is the balanced
  cold partial (on the 4.2" with `grey_partials: true`, a grey partial of the
  changed rows), so nothing needs settling before the next sleep. Add
  `fast_greys: true` on the 4.2" to halve grey refreshes.
- **A dashboard updating every minute.** Use `grey: true`, and either the
  default fast partials, if only numbers or black and white parts change, or
  `grey_partials: glide`, if greys change and a flash is unwelcome. Keep
  `full_update_every` (default 30) so periodic full refreshes clear what
  partials and glides leave behind.
- **Unpaid drive after a reflash or power cut.** Fast partials, glides and
  ticks keep their balance records in memory, and the balanced platforms settle
  them before a planned deep sleep or a grey refresh. A reflash, crash or power
  cut mid-flight loses that record, and its drive stays on the glass until later
  refreshes wear it off: a faint ghost of the live demo's ball showed under grey
  refreshes on the 5.79" after one.
- **Temperature.** Without a `temperature_sensor`, the 5.79"'s waveforms assume
  13 to 20 °C at 50 Hz; give it a sensor if the room runs much warmer or colder.
  The 4.2" reads its own panel's temperature back from its controller over the
  SPI data line. The 5.79"'s controllers could answer the same way, but that
  read has not been built for them yet.
