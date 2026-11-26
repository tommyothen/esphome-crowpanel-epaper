#pragma once

#include "esphome/components/epaper_spi/epaper_spi.h"
#include "esphome/core/defines.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

namespace esphome::crowpanel_epaper {

/**
 * What ELECROW's CrowPanel ESP32-S3 e-paper drivers share, whatever their controllers: the buffer
 * and its levels, the RAM planes made from it, the record of what the glass shows and of which
 * pixels a fast partial left out of balance, the passes an update runs, and the loop that runs them.
 * A board's driver supplies its controllers: how planes go out, how a waveform is loaded and run,
 * and how the supplies and the controllers come and go.
 *
 * EPaperBase runs the update as a state machine from loop(): BUSY is polled, never waited on, and
 * the data goes out in 10 ms slices, several to a loop() call. Once the driver knows what the glass
 * shows (after the first update since boot or a wake), partial refreshes push each changing pixel
 * one way only, twice as fast as a balanced one. Each such push is recorded, a bit a pixel, and
 * cancelled before the next full refresh, or settled before a deep sleep, so no pixel is ever more
 * than one push out of balance.
 *
 * The buffer holds 4 bits a pixel: a level from 0 (black) to 6 (white). With four greys only the
 * even levels are drawn. Each refresh picks one of four waveforms for every pixel from its bit in
 * the old plane and its bit in the new one, and what those bits mean depends on the refresh:
 *  - a partial one compares the old plane (what the panel shows) with the new (the new frame),
 *    each pixel white from level 4 up, and drives only the pixels that change, so greys stay;
 *  - cancelling and settling read which pixels are out of balance from the old plane, and what
 *    the glass shows from the new;
 *  - a four-grey one takes level / 2 as two bits, high in the old plane and low in the new;
 *  - a seven-grey one runs three passes: a half step towards black for levels 2, 3 and 5, a
 *    four-grey refresh that draws levels 1 to 3 as dark grey and 4 and 5 as light grey, and the
 *    same half step towards white, so every pixel's drive still nets to nothing.
 * Colours follow epaper_spi: Color::BLACK is ink, Color::WHITE is paper.
 *
 * After every update the panel's RAM holds the glass's black and white reading in both planes, so
 * a partial refresh sends only the new frame, and the RAM is the only copy of what the panel shows:
 * that is what lets the ESP sleep. Through deep sleep the driver holds the panel's power and reset
 * pins high, so the panel keeps its picture and its RAM; after a wake the full_update_every count
 * carries on, and the first update can be partial.
 */
class CrowPanelBase : public epaper_spi::EPaperBase {
 public:
  // The bounds of what the base keeps for any panel: the widest's plane row and the tallest's rows.
  static constexpr size_t MAX_PLANE_ROW = 99;
  static constexpr uint16_t MAX_HEIGHT = 300;

  CrowPanelBase(const char *name, uint16_t width, uint16_t height)
      : EPaperBase(name, width, height, nullptr, 0, display::DISPLAY_TYPE_GRAYSCALE),
        plane_row_(width / 8),
        bit_plane_(width / 8 * height) {
    this->row_width_ = width / 2;  // the buffer's row: 2 pixels a byte
    this->buffer_length_ = this->row_width_ * height;
    this->end_row_ = height;
  }

  void setup() override;
  void loop() override;
  void update() override;
  void on_safe_shutdown() override;
  void fill(Color color) override;
  void draw_pixel_at(int x, int y, Color color) override;
  // Full refreshes in four greys instead of black and white.
  void set_grey(bool grey) { this->grey_ = grey; }
  // 4, or 7 for three-pass grey refreshes with a half step between each of the four greys.
  void set_grey_levels(uint8_t levels) {
    this->seven_ = levels == 7;
    this->last_colour_ = UINT32_MAX;  // levels map differently now, but white is 6 in both
    this->last_level_ = 6;
  }
  // The pin that switches the panel's power, held high through deep sleep.
  void set_power_pin(InternalGPIOPin *pin) { this->power_pin_ = pin; }
  // The reset pin, held high through deep sleep so the panel stays as it is rather than reset.
  void set_hold_reset_pin(InternalGPIOPin *pin) { this->hold_reset_pin_ = pin; }
#ifdef USE_SENSOR
  // The panel's temperature, or near enough, for running the waveforms faster or slower.
  void set_temperature_sensor(sensor::Sensor *sensor) { this->temperature_ = sensor; }
#endif

  // Where a RAM plane's bytes come from: the buffer through a table from a buffer byte (two pixels)
  // to their two bits, or a bit plane as it is.
  struct Source {
    const uint8_t *table;
    const uint8_t *plane;
  };

 protected:
  // One refresh of an update; most updates are one, cancelling and seven greys add more.
  enum class Pass : uint8_t {
    PARTIAL,        // balanced, from cold
    FAST,           // one way, in an active spell
    CORRECT,        // cancel the fast partials' pushes before a full refresh
    BLACK_WHITE,    // a full refresh in black and white
    GREY,           // four greys
    HALF_TO_BLACK,  // seven greys: the half step, the even four, the half step back
    EVEN_GREY,
    HALF_TO_WHITE,
    DOSE,           // a glide: one of its scans pushing each pixel part way to its new grey
    UNDOSE,         // one of the scans cancelling glides' drive before a grey refresh
  };

  // The controllers, which a board's driver supplies.
  // Send the planes from plane_ on, old then new, in slices; true once they are all out.
  virtual bool send_planes_(const Source &old_source, const Source &new_source) = 0;
  // Before a pass's planes go out: which planes and rows it sends.
  virtual void start_pass_(Pass pass) = 0;
  // A hardware reset that leaves the supplies off, for a boot and for giving up on an update.
  virtual void reset_controllers_() = 0;
  // Cancel every fast partial's push now, waiting on BUSY, for a deep sleep.
  virtual void settle_now_() = 0;
  // The supplies off and the controllers as they should spend a deep sleep; settled when no update
  // was cut short.
  virtual void prepare_for_sleep_(bool settled) = 0;
  // Hooks a driver may use.
  virtual void setup_controllers_() {}
  virtual void plan_started_() {}
  // A driver's own kind of update, planned in passes_; false to plan as the base does, after any
  // passes the driver planned first.
  virtual bool plan_own_update_() { return false; }
  // Sends the planes of a pass only the driver knows; true once they are all out.
  virtual bool send_own_planes_(Pass pass) { return true; }
  virtual void pass_finished_() {}
  // After the last pass is recorded.
  virtual void update_finished_(Pass last) {}
  // A fast partial changed the black and white reading of plane byte i of row y.
  virtual void reading_changed_(size_t y, size_t i) {}
  // Whether the panel's RAM needs the glass's reading written to both planes after the last pass.
  virtual bool copies_after_(Pass last) const { return true; }
  // The rows the last update can have changed, first to end, for the glass's record.
  virtual void changed_rows_(uint16_t &first, uint16_t &end) const {
    first = 0;
    end = this->height_;
  }
  // Whether cancelling cleared the record of the pixels in plane byte i of row y.
  virtual bool corrects_(size_t y, size_t i) const { return true; }
  // State a driver forgets when an update is given up on.
  virtual void forget_() {}

  bool transfer_data() override;
  void power_off() override;

  uint8_t level_(Color color) const;
  // Plane bytes offset to offset + count of row y.
  void plane_bytes_(const Source &source, size_t y, size_t offset, size_t count, uint8_t *out);
  bool find_changed_rows_();
  void find_unsettled_rows_();
  void plan_update_();
  void plan_grey_passes_();
  void record_glass_(bool fast, bool corrected);
  void give_up_();
  void wait_idle_(uint32_t timeout_ms);

  const size_t plane_row_;  // a RAM plane's row: 8 pixels a byte
  const size_t bit_plane_;  // a bit a pixel
  InternalGPIOPin *power_pin_{nullptr};
  InternalGPIOPin *hold_reset_pin_{nullptr};
  bool grey_{false};
  bool seven_{false};
  Pass passes_[16]{};
  uint8_t pass_count_{0};
  uint8_t pass_index_{0};
  bool pass_started_{false};
  bool next_pass_{false};       // a pass has finished and loop() starts the next
  bool unchanged_{false};       // a partial update in which no row changed
  bool copying_{false};         // writing the black and white reading to both planes
  bool supplies_on_{false};     // the ±15 V supplies are on
  uint32_t supplies_since_{0};
  uint8_t frame_rate_{0};       // this update's, for its temperature
#ifdef USE_SENSOR
  sensor::Sensor *temperature_{nullptr};
#endif
  uint8_t plane_{0};            // the plane part way out
  bool sending_{false};         // a plane is part way out
  uint16_t row_{0};             // the next row of it
  uint16_t first_row_{0};       // the rows this pass sends
  uint16_t end_row_{0};
  uint8_t *glass_{nullptr};     // the glass's black and white reading, a bit a pixel
  uint8_t *unsettled_{nullptr}; // pixels a fast partial left a push out of balance
  bool glass_valid_{false};
  uint32_t unsettled_count_{0};
  uint8_t *pixels_{nullptr};          // the buffer, when it is one block
  uint32_t last_colour_{UINT32_MAX};  // draw_pixel_at's last colour and its level: white, all ones
  uint8_t last_level_{6};
  uint32_t started_{0};  // when the update running began
};

// For each plane bit a refresh needs, the levels (0 to 6) that set it, and from that a table from
// a buffer byte, two pixels with the first in the high nibble, to their two bits.
inline constexpr uint8_t WHITE = 0b1110000;      // levels 4 to 6: a partial's reading, and the grey's high bit
// The middle pass makes levels 0, 1, 4 and 6 directly (black, dark grey, light grey, white) and
// levels 2, 3 and 5 as dark, dark and light grey before their half steps; with four greys only
// the even levels are drawn, and level 2 is dark grey.
inline constexpr uint8_t GREY_LOW = 0b1001110;   // levels 1, 2, 3 and 6: dark grey or white
inline constexpr uint8_t HALF_HIGH = 0b0101000;  // levels 3 and 5: the half step's high bit
inline constexpr uint8_t HALF_LOW = 0b1100100;   // levels 2, 5 and 6: the half step's low bit
struct PlaneBits {
  uint8_t table[256];
  constexpr PlaneBits(uint8_t levels) : table() {
    for (int v = 0; v < 256; v++)
      table[v] = (((levels >> (v >> 4)) & 1) << 1) | ((levels >> (v & 0x0F)) & 1);
  }
};
inline constexpr PlaneBits WHITE_BITS{WHITE}, GREY_LOW_BITS{GREY_LOW}, HALF_HIGH_BITS{HALF_HIGH},
    HALF_LOW_BITS{HALF_LOW};
inline constexpr CrowPanelBase::Source WHITE_SOURCE{WHITE_BITS.table, nullptr};

}  // namespace esphome::crowpanel_epaper
