# Choosing a mode

Start with the balanced platform for your board (`crowpanel_579` or
`crowpanel_42`). Pick the refresh mode by how often the picture changes and
whether it needs greys.

| Platform or mode | Use for | Time and look | Needs | Risks and limits |
|---|---|---|---|---|
| `crowpanel_579` / `crowpanel_42`, `grey: false` (default) | Plain black and white, or the quickest full refresh on the 4.2" | Full B&W: about 3.7 s (5.79" OTP) or 1.15 to 1.3 s (4.2"). Flashes. Fast partials 0.4 to 0.5 s | Standard pins; 54 KB / 30 KB RAM for fast partials | Black and white only; no anti-aliased text |
| `grey: true` (4 greys) | Dashboards, anti-aliased text (`bpp: 2`), wake-draw-sleep displays | Full refresh: about 4.2 s (5.79") or 3.7 s (4.2"). Flashes | `grey: true` | On the 5.79" dark grey sits darker than an even third; on the 4.2" greys are near even thirds |
| `grey_levels: 7` | Shading, pictures, `bpp: 4` text, or even grey steps on the 5.79" | Full refresh: 4.4 to 4.5 s (5.79") or 3.95 s (4.2"), three passes. Flashes | `grey: true` | Longer than four greys |
| `fast_greys: true` (4.2" only) | 4.2" dashboards or pictures that run grey refreshes often | Full 4 or 7 greys in about 2.0 s; 7-grey partial in 2.3 s. Flashes | `crowpanel_42`, `grey: true` | Erases the picture before as well as slow greys (each grey within 0.09) |
| Fast partials (`grey_partials: false`, default) | Clocks, counters, minute dashboards, button and dial input | 0.4 to 0.5 s (5.79") or 0.44 s (4.2"); cold partial after wake 1.3 s or 0.98 s. No flash | 54 KB / 30 KB RAM for balance tracking | Changed pixels show B&W until the next full refresh (or `request_grey_partial()` on 4.2"); a large black area from a full refresh clears only to light grey; each push is out of balance until cancelled or settled |
| `grey_partials: true` (4.2" only) | Menus or dashboards where changed regions must keep exact greys without flashing the rest | About 3.75 s slow, or 2.3 s with `fast_greys: true` (7 greys). Flashes only the changed window; rest holds within 0.01 | `crowpanel_42`, `grey: true`, PSRAM (60 KB) | As slow as a full grey refresh; changes far apart redraw everything between them in one rectangle |
| Fast partials + `request_grey_partial()` (4.2" only) | Interactive screens that answer quickly on input, then clean up once still | 0.44 s per press, then one windowed grey refresh after a delay | `crowpanel_42`, `grey: true`, PSRAM | Single rectangle around everything changed; your YAML script triggers the settle |
| `grey_partials: glide` | Dashboards or UI updates with greys where flashing is unwelcome | Box in 0.32 to 0.38 s (4.2") or 0.5 s (5.79"); whole panel 0.93 s or 1.3 s. No flash | `grey: true`, PSRAM (about 330 KB on 4.2", 610 KB on 5.79") | Greys land near, not exact (about 0.04 out on both, in perceptual lightness) until the next grey refresh; power loss or reflashing mid-flight loses the drive record |
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
