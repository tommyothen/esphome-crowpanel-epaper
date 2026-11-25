#pragma once

#include "esphome/components/epaper_spi/epaper_spi.h"

namespace esphome::crowpanel_579 {

/**
 * ELECROW CrowPanel ESP32-S3 5.79" e-paper: 792x272, four greys.
 *
 * The panel is two SSD1683 controllers in cascade behind one chip select. The left controller
 * takes the same commands with 0x80 set (0xA4 for 0x24 and so on); everything without a RAM or
 * address argument goes to both. Each controller has 400 columns of RAM but shows 396, so a
 * row of RAM bytes splits at byte 49, which both receive: the left controller shows its high
 * nibble and the right one its low nibble.
 *
 * EPaperBase runs the update as a state machine from loop(): BUSY is polled, never waited on,
 * and the data goes out in 10 ms slices. Every update resets and initialises the controllers,
 * sends the RAM planes, refreshes, then sleeps them.
 *
 * The buffer holds 2 bits a pixel, 0 black to 3 white. A partial refresh is black and white:
 * it compares RAM 0x26 (what the panel shows) with 0x24 (the new frame), each pixel light or
 * dark, and runs a loaded waveform that drives only the pixels that change. A full refresh is
 * either the panel's own black and white one or, with grey enabled, a four-grey one from a
 * loaded waveform, where 0x26 holds each pixel's high bit and 0x24 its low bit. Greys a partial
 * refresh does not change stay as they are. Colours follow epaper_spi: Color::BLACK is ink,
 * Color::WHITE is paper.
 *
 * After every refresh, 0x26 holds what is on the glass, light or dark: a full refresh leaves it
 * so, and after a partial one the driver sends its new frame to both planes. A partial refresh
 * therefore sends only the new frame, and the panel's RAM is the only copy of what it shows,
 * which is what lets the ESP sleep. Through deep sleep the driver holds the panel's power and reset pins
 * high, so the panel keeps its picture and its RAM; after a wake the full_update_every count
 * carries on, and the first update can be partial.
 */
class CrowPanel579 : public epaper_spi::EPaperBase {
 public:
  static constexpr uint16_t WIDTH = 792;
  static constexpr uint16_t HEIGHT = 272;

  CrowPanel579() : EPaperBase("CrowPanel 5.79", WIDTH, HEIGHT, nullptr, 0, display::DISPLAY_TYPE_GRAYSCALE) {
    this->row_width_ = WIDTH / 4;  // the buffer's row: 4 pixels a byte
    this->buffer_length_ = this->row_width_ * HEIGHT;
  }

  void setup() override;
  void loop() override;
  void on_safe_shutdown() override;
  void fill(Color color) override;
  void draw_pixel_at(int x, int y, Color color) override;
  // Full refreshes use the four-grey waveform instead of the panel's black and white one.
  void set_grey(bool grey) { this->grey_ = grey; }
  // The pin that switches the panel's power, held high through deep sleep.
  void set_power_pin(InternalGPIOPin *pin) { this->power_pin_ = pin; }
  // The reset pin, held high through deep sleep so the panel stays asleep rather than reset.
  void set_hold_reset_pin(InternalGPIOPin *pin) { this->hold_reset_pin_ = pin; }

 protected:
  static constexpr size_t PLANE_ROW = WIDTH / 8;  // a RAM plane's row: 8 pixels a byte
  static constexpr size_t HALF = 50;              // one controller's 400 columns
  static constexpr size_t RIGHT_OFFSET = 49;      // the right half starts with the shared byte

  bool reset() override;
  bool initialise(bool partial) override;
  bool transfer_data() override;
  void refresh_screen(bool partial) override;
  void power_on() override {}
  void power_off() override;
  void deep_sleep() override;

  // Point one controller's RAM window and address counters at the start of its half.
  void set_ram_area_(bool left);
  // One controller's half of a plane row: each pixel's high bit (also its black or white
  // reading) or its low bit.
  void plane_half_(size_t y, size_t offset, bool high, uint8_t *out);
  // Load a 233-byte waveform (0x32 and its tail) and select it for the refresh.
  void load_waveform_(const uint8_t *waveform);
  // Send the RAM plane passes from pass_ on, in slices; true once all four are out.
  bool send_planes_(bool low_new);

  InternalGPIOPin *power_pin_{nullptr};
  InternalGPIOPin *hold_reset_pin_{nullptr};
  bool grey_{false};
  bool partial_sent_{false};  // the refresh in progress is partial, so the planes need the new frame after it
  bool copying_{false};       // sending the new frame to both planes after a partial refresh
  bool reset_sent_{false};
  uint8_t pass_{0};  // 0 and 1: 0x26, left then right; 2 and 3: 0x24
};

}  // namespace esphome::crowpanel_579
