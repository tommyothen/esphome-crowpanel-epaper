#pragma once

#include "esphome/components/epaper_spi/epaper_spi.h"

namespace esphome::crowpanel_579 {

/**
 * ELECROW CrowPanel ESP32-S3 5.79" e-paper: 792x272, four or seven greys.
 *
 * The panel is two SSD1683 controllers in cascade behind one chip select. The left controller
 * takes the same commands with 0x80 set (0xA4 for 0x24 and so on); everything without a RAM or
 * address argument goes to both. Each controller has 400 columns of RAM but shows 396, so a
 * row of RAM bytes splits at byte 49, which both receive: the left controller shows its high
 * nibble and the right one its low nibble.
 *
 * EPaperBase runs the update as a state machine from loop(): BUSY is polled, never waited on,
 * and the data goes out in 10 ms slices, several to a loop() call. An update resets and
 * initialises the controllers unless the last one left them awake, sends the RAM planes,
 * refreshes, and leaves them awake for ten seconds before deep sleep.
 *
 * The buffer holds 4 bits a pixel: a level from 0 (black) to 6 (white). With four greys only
 * the even levels are drawn. Each refresh picks one of four waveforms for every pixel from the
 * pixel's bit in RAM 0x26 and its bit in 0x24, and what those bits mean depends on the refresh:
 *  - a partial one compares 0x26 (what the panel shows) with 0x24 (the new frame), each pixel
 *    white from level 4 up, and drives only the pixels that change, so greys elsewhere stay;
 *  - a four-grey one takes level / 2 as two bits, high in 0x26 and low in 0x24;
 *  - a seven-grey one runs three passes: a half step towards black for the odd levels, the
 *    four-grey refresh of level / 2, and the same half step towards white, so every pixel's
 *    drive still nets to nothing;
 *  - with grey off, a full refresh is the panel's own black and white one.
 * Colours follow epaper_spi: Color::BLACK is ink, Color::WHITE is paper.
 *
 * After every update, 0x26 holds what is on the glass, black or white: the panel's own black and
 * white refresh leaves it so, and after any other the driver writes that reading to both planes.
 * A partial refresh therefore sends only the new frame, and the panel's
 * RAM is the only copy of what it shows, which is what lets the ESP sleep. Through deep sleep the
 * driver holds the panel's power and reset pins high, so the panel keeps its picture and its RAM;
 * after a wake the full_update_every count carries on, and the first update can be partial.
 */
class CrowPanel579 : public epaper_spi::EPaperBase {
 public:
  static constexpr uint16_t WIDTH = 792;
  static constexpr uint16_t HEIGHT = 272;

  CrowPanel579() : EPaperBase("CrowPanel 5.79", WIDTH, HEIGHT, nullptr, 0, display::DISPLAY_TYPE_GRAYSCALE) {
    this->row_width_ = WIDTH / 2;  // the buffer's row: 2 pixels a byte
    this->buffer_length_ = this->row_width_ * HEIGHT;
  }

  void setup() override;
  void loop() override;
  void update() override;
  void on_safe_shutdown() override;
  void fill(Color color) override;
  void draw_pixel_at(int x, int y, Color color) override;
  // Full refreshes use loaded grey waveforms instead of the panel's black and white one.
  void set_grey(bool grey) { this->grey_ = grey; }
  // 4, or 7 for three-pass grey refreshes with a half step between each of the four greys.
  void set_grey_levels(uint8_t levels) {
    this->seven_ = levels == 7;
    this->last_colour_ = UINT32_MAX;  // levels map differently now
  }
  // The pin that switches the panel's power, held high through deep sleep.
  void set_power_pin(InternalGPIOPin *pin) { this->power_pin_ = pin; }
  // The reset pin, held high through deep sleep so the panel stays asleep rather than reset.
  void set_hold_reset_pin(InternalGPIOPin *pin) { this->hold_reset_pin_ = pin; }

 protected:
  static constexpr size_t HALF = 50;          // one controller's 400 columns
  static constexpr size_t RIGHT_OFFSET = 49;  // the right half starts with the shared byte

  enum class Kind : uint8_t { BLACK_WHITE, PARTIAL, GREY, SEVEN_GREYS };

  bool reset() override;
  bool initialise(bool partial) override;
  bool transfer_data() override;
  void refresh_screen(bool partial) override;
  void power_on() override {}
  void power_off() override;
  void deep_sleep() override;

  uint8_t level_(Color color) const;
  // Point one controller's RAM window and address counters at the start of its half.
  void set_ram_area_(bool left, uint16_t first_y, uint16_t end_y);
  // One controller's half of a plane row, through a table from a buffer byte (two pixels) to
  // their two bits.
  void plane_half_(size_t y, size_t offset, const uint8_t *bits, uint8_t *out);
  // Put the controllers into deep sleep now rather than after CONTROLLER_IDLE_MS.
  void sleep_controllers_();
  // Load a 233-byte waveform (0x32 and its tail) and select it for the refresh.
  void load_waveform_(const uint8_t *waveform);
  // Send the RAM plane passes from plane_ on, in slices, through the given tables; true once all
  // four are out.
  bool send_planes_(const uint8_t *old_bits, const uint8_t *new_bits);
  uint32_t row_hash_(size_t y);
  bool find_changed_rows_();

  InternalGPIOPin *power_pin_{nullptr};
  InternalGPIOPin *hold_reset_pin_{nullptr};
  bool grey_{false};
  bool seven_{false};
  Kind kind_{Kind::BLACK_WHITE};  // the update in progress
  uint8_t grey_pass_{0};          // which of a seven-grey refresh's three passes is running
  bool next_pass_{false};         // a pass has finished and loop() starts the next
  bool copying_{false};           // writing the black and white reading to both planes
  bool reset_sent_{false};
  bool awake_{false};  // the controllers have been reset and not slept since
  uint8_t *pixels_{nullptr};          // the buffer, when it is one block
  uint32_t last_colour_{UINT32_MAX};  // draw_pixel_at's last colour and its level
  uint8_t last_level_{0};
  uint8_t plane_{0};  // 0 and 1: 0x26, left then right; 2 and 3: 0x24
  bool sending_{false};             // a plane is part way out
  uint16_t row_{0};                 // the next row of it
  uint16_t first_row_{0};           // the rows this update sends: all, or a partial's changed band
  uint16_t end_row_{HEIGHT};
  bool unchanged_{false};           // a partial update in which no row changed
  uint32_t new_rows_[HEIGHT]{};     // a partial's row hashes, for the glass once it has run
};

}  // namespace esphome::crowpanel_579
