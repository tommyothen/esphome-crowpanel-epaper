# crowpanel-epaper

ESPHome display drivers for ELECROW's CrowPanel ESP32-S3 e-paper boards, the
5.79" and the 4.2" V1.2.

- **Even greys.** Four or seven, tuned on each panel and measured as an eye sees them.
- **Partial refreshes** in about half a second, without a flash, that leave greys alone.
- **Glides.** Changed pixels go straight to their new grey, without flashing.
- **Live mode.** 14 to 27 updates a second, for things that move.
- **Kind to the panel.** Every waveform is charge-balanced, checked when it compiles.
- **Deep sleep** that keeps the picture.

No other driver for these boards that we know of has greys, glides or a live
mode: see [how it compares](docs/other-drivers.md).

## Boards

| Board | Platform |
|---|---|
| CrowPanel 5.79" (792x272, two SSD1683s) | `crowpanel_579` |
| CrowPanel 4.2" V1.2 (400x300, UC8276C) | `crowpanel_42` |
| Either, without charge balance (experimental) | `crowpanel_579_turbo`, `crowpanel_42_turbo`: see [turbo](docs/turbo.md) |

The 4.2" V1.0, with an SSD1683, is not supported.

## Getting started

```yaml
external_components:
  - source: github://tommyothen/crowpanel-epaper@v1.0.0
    components: [crowpanel_579, crowpanel_epaper]

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

For the 4.2", use `crowpanel_42` in both places. Pin to a release tag or a full
commit SHA, since GitHub will not fetch a short one. The USB-C port resets the
board into its bootloader, so flashing needs no BOOT button.

## Which mode?

| You want | Set | Full refresh (5.79" / 4.2") | Each update |
|---|---|---|---|
| Black and white | nothing | 3.7 s / 1.2 s | 0.4 s, no flash |
| Greys, such as anti-aliased text | `grey: true` | 4.2 s / 3.7 s | 0.4 s, no flash |
| Smooth shading or pictures | `grey_levels: 7` | 4.5 s / 3.95 s | 0.4 s, no flash |
| Greys that change without flashing | `grey_partials: glide` | as above | 0.3 to 0.5 s |
| Something that moves | `grey_partials: live` | as above | 14 to 16 / 22 to 27 a second |

What each one costs, and when not to use it: [choosing a mode](docs/choosing-a-mode.md).

## More

- [Guide](docs/guide.md): every option, drawing, the buttons, deep sleep, examples and limitations
- [How it works](docs/how-it-works.md): each board, charge balance, glides, live mode and temperature
- [Turbo](docs/turbo.md): the experimental unbalanced drivers, and their risks
- [Examples](examples/): two demos, two live demos and two test cards
- Panel notes and measurements: [5.79"](docs/crowpanel-579.md), [4.2"](docs/crowpanel-42.md)

## Thanks and licence

Built on what [other projects](docs/other-drivers.md) worked out first. MIT
licence; the four-grey waveform is adapted from Waveshare's, under the notice in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
