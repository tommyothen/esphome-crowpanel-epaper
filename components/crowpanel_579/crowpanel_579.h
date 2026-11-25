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
 * initialises the controllers unless the last one left them awake, sends the RAM planes, and
 * refreshes, leaving the ±15 V supplies on for a few seconds in case another update follows, and
 * the controllers awake a while longer so the next skips their reset. Once the driver knows what
 * the glass shows (after the first update since boot or a wake), partial refreshes push each
 * changing pixel one way only, twice as fast as a balanced one. Each such push is recorded, a bit a
 * pixel, and cancelled before the next grey refresh, or settled before a deep sleep, so no pixel is
 * ever more than one push out of balance.
 *
 * The buffer holds 4 bits a pixel: a level from 0 (black) to 6 (white). With four greys only
 * the even levels are drawn. Each refresh picks one of four waveforms for every pixel from the
 * pixel's bit in RAM 0x26 and its bit in 0x24, and what those bits mean depends on the refresh:
 *  - a partial one compares 0x26 (what the panel shows) with 0x24 (the new frame), each pixel
 *    white from level 4 up, and drives only the pixels that change, so greys elsewhere stay;
 *  - cancelling and settling read which pixels are out of balance from 0x26, and what the
 *    glass shows from 0x24;
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
  static constexpr size_t PLANE_ROW = WIDTH / 8;  // a RAM plane's row: 8 pixels a byte

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

  // Where a RAM plane's bytes come from: the buffer through a table from a buffer byte (two pixels)
  // to their two bits, or a bit plane as it is.
  struct Source {
    const uint8_t *table;
    const uint8_t *plane;
  };

 protected:
  static constexpr size_t HALF = 50;                       // one controller's 400 columns
  static constexpr size_t RIGHT_OFFSET = 49;               // the right half starts with the shared byte
  static constexpr size_t BIT_PLANE = PLANE_ROW * HEIGHT;  // a bit a pixel
  static constexpr size_t ROWS_A_WRITE = 16;

  // One refresh of an update; most updates are one, cancelling and seven greys add more.
  enum class Pass : uint8_t {
    PARTIAL,        // balanced, from cold
    FAST,           // one way, in an active spell
    CORRECT,        // cancel the fast partials' pushes before a grey refresh
    BLACK_WHITE,    // the panel's own full refresh
    GREY,           // four greys
    HALF_TO_BLACK,  // seven greys: the half step, the even four, the half step back
    EVEN_GREY,
    HALF_TO_WHITE,
  };

  bool reset() override;
  bool initialise(bool partial) override;
  bool transfer_data() override;
  void refresh_screen(bool partial) override;
  void power_on() override {}
  void power_off() override;
  void deep_sleep() override;

  uint8_t level_(Color color) const;
  // Point one controller's RAM window and address counters at rows first_y to end_y of its half.
  void set_ram_area_(bool left, uint16_t first_y, uint16_t end_y);
  void plane_bytes_(const Source &source, size_t y, size_t offset, size_t count, uint8_t *out);
  // Load a 233-byte waveform (0x32 and its tail) and run it.
  void load_waveform_(const uint8_t *waveform);
  // Send the RAM plane passes from plane_ on, in slices; true once all four are out.
  bool send_planes_(const Source &old_source, const Source &new_source);
  bool find_changed_rows_();
  void find_unsettled_rows_();
  void plan_update_();
  void record_glass_(bool fast, bool corrected);
  void wind_down_();
  void give_up_();
  void reset_controllers_();
  void settle_now_();
  void wait_idle_(uint32_t timeout_ms);
  void switch_supplies_off_();
  // Put the controllers into deep sleep now, supplies off first.
  void sleep_controllers_();

  InternalGPIOPin *power_pin_{nullptr};
  InternalGPIOPin *hold_reset_pin_{nullptr};
  bool grey_{false};
  bool seven_{false};
  Pass passes_[4]{};
  uint8_t pass_count_{0};
  uint8_t pass_index_{0};
  bool pass_started_{false};
  bool next_pass_{false};       // a pass has finished and loop() starts the next
  bool unchanged_{false};       // a partial update in which no row changed
  bool copying_{false};         // writing the black and white reading to both planes
  uint8_t init_step_{0};
  bool awake_{false};           // the controllers have been reset and not slept since
  bool supplies_on_{false};     // the ±15 V supplies are on
  uint32_t supplies_since_{0};
  uint8_t plane_{0};            // 0 and 1: 0x26, left then right; 2 and 3: 0x24
  bool sending_{false};         // a plane is part way out
  uint16_t row_{0};             // the next row of it
  uint16_t first_row_{0};       // the rows this pass sends
  uint16_t end_row_{HEIGHT};
  uint8_t *glass_{nullptr};     // the glass's black and white reading, a bit a pixel
  uint8_t *unsettled_{nullptr}; // pixels a fast partial left a push out of balance
  bool glass_valid_{false};
  uint32_t unsettled_count_{0};
  uint8_t *pixels_{nullptr};          // the buffer, when it is one block
  uint32_t last_colour_{UINT32_MAX};  // draw_pixel_at's last colour and its level
  uint8_t last_level_{0};
};

}  // namespace esphome::crowpanel_579
