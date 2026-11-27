# Turbo

`crowpanel_579_turbo` and `crowpanel_42_turbo` are the same drivers with charge
balance switched off, at the same ±15 V voltages. They are for experiments:
every refresh leaves its drive on the panel, which builds image retention on a
still picture and may shorten the panel's life. Keep a display you rely on on
the balanced platform. Each takes its board's options, needs `grey: true` and
PSRAM, and needs its board's component listed too:

```yaml
external_components:
  - source: github://tommyothen/esphome-crowpanel-epaper@v1.0.0
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
| Full refresh of a black and white picture | 3.9 s | 0.46 s | 4.4 to 4.6 s | 0.67 to 0.87 s |
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

The full measurements are in [the 4.2" notes](crowpanel-42.md) and
[the 5.79" notes](crowpanel-579.md).
