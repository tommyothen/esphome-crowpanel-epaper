#pragma once

#include <driver/spi_master.h>

#include "esphome/components/epaper_spi/epaper_spi.h"
#include "esphome/core/defines.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

namespace esphome::crowpanel_42 {

/**
 * ELECROW CrowPanel ESP32-S3 4.2" e-paper, hardware V1.2: 400x300, four or seven greys.
 *
 * The panel's controller is a UC8276C whose OTP holds no waveforms, so every refresh runs one this
 * driver loads. Its waveform format is the SSD1683's: per table, groups of a repeat count, four
 * phases (the level in the top two bits, frames in the rest) and two state repeats, with VSH
 * (+15 V) driving a pixel black and VSL (-15 V) white. Each pixel picks a table from its bit in the
 * NEW plane (DTM2, 0x13) and its bit in the OLD plane (DTM1, 0x10), 1 white.
 *
 * EPaperBase runs the update as a state machine from loop(): BUSY is polled, never waited on,
 * and the data goes out in 10 ms slices, several to a loop() call. An update resets and
 * initialises the controller unless the last one left it so, sends the planes, and refreshes,
 * leaving the ±15 V supplies on for a few seconds in case another update follows. Once the driver
 * knows what the glass shows (after the first update since boot or a wake), partial refreshes
 * push each changing pixel one way only, twice as fast as a balanced one. Each such push is
 * recorded, a bit a pixel, and cancelled before the next full refresh, or settled before a deep
 * sleep, so no pixel is ever more than one push out of balance.
 *
 * The buffer holds 4 bits a pixel: a level from 0 (black) to 6 (white). With four greys only
 * the even levels are drawn. What the planes' bits mean depends on the refresh:
 *  - a partial one compares OLD (what the panel shows) with NEW (the new frame), each pixel
 *    white from level 4 up, and drives only the pixels that change, so greys elsewhere stay;
 *  - cancelling and settling read which pixels are out of balance from OLD, and what the glass
 *    shows from NEW;
 *  - a four-grey one takes level / 2 as two bits, high in OLD and low in NEW;
 *  - a seven-grey one runs three passes: a half step towards black for levels 2, 3 and 5, a
 *    four-grey refresh that draws levels 1 to 3 as dark grey and 4 and 5 as light grey, and the
 *    same half step towards white, so every pixel's drive still nets to nothing;
 *  - with grey off, a full refresh is a balanced black and white one.
 * Colours follow epaper_spi: Color::BLACK is ink, Color::WHITE is paper.
 *
 * The controller never copies NEW to OLD, so after every update the driver writes the glass's
 * black and white reading to both. A partial refresh then sends only the new frame, and the
 * panel's RAM is the only copy of what it shows, which is what lets the ESP sleep: the RAM
 * survives a hardware reset, and through deep sleep the driver holds the panel's power and reset
 * pins high with the controller in standby. After a wake the full_update_every count carries on,
 * and the first update can be partial.
 */
class CrowPanel42 : public epaper_spi::EPaperBase {
 public:
  static constexpr uint16_t WIDTH = 400;
  static constexpr uint16_t HEIGHT = 300;
  static constexpr size_t PLANE_ROW = WIDTH / 8;  // a RAM plane's row: 8 pixels a byte
  static constexpr size_t WAVEFORM = 5 * 42;      // a waveform: five tables of six seven-byte groups

  CrowPanel42() : EPaperBase("CrowPanel 4.2", WIDTH, HEIGHT, nullptr, 0, display::DISPLAY_TYPE_GRAYSCALE) {
    this->row_width_ = WIDTH / 2;  // the buffer's row: 2 pixels a byte
    this->buffer_length_ = this->row_width_ * HEIGHT;
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
    this->last_colour_ = UINT32_MAX;  // levels map differently now
  }
  // The pin that switches the panel's power, held high through deep sleep.
  void set_power_pin(InternalGPIOPin *pin) { this->power_pin_ = pin; }
  // The reset pin, held high through deep sleep so the controller keeps its RAM.
  void set_hold_reset_pin(InternalGPIOPin *pin) { this->hold_reset_pin_ = pin; }
#ifdef USE_SENSOR
  // A temperature to use instead of the panel's own, for running the waveforms faster or slower.
  void set_temperature_sensor(sensor::Sensor *sensor) { this->temperature_ = sensor; }
#endif
  // The temperature the panel sensed when its supplies last came on, in °C; NAN before then.
  float panel_temperature() const { return this->panel_celsius_; }

  // Where a RAM plane's bytes come from: the buffer through a table from a buffer byte (two pixels)
  // to their two bits, or a bit plane as it is.
  struct Source {
    const uint8_t *table;
    const uint8_t *plane;
  };

 protected:
  static constexpr size_t BIT_PLANE = PLANE_ROW * HEIGHT;  // a bit a pixel
  static constexpr size_t ROWS_A_WRITE = 16;

  // One refresh of an update; most updates are one, cancelling and seven greys add more.
  enum class Pass : uint8_t {
    PARTIAL,        // balanced, from cold
    FAST,           // one way, in an active spell
    CORRECT,        // cancel the fast partials' pushes before a full refresh
    BLACK_WHITE,    // balanced black and white
    GREY,           // four greys
    HALF_TO_BLACK,  // seven greys: the half step, the even four, the half step back
    EVEN_GREY,
    HALF_TO_WHITE,
  };

  bool reset() override;
  bool initialise(bool partial) override;
  bool transfer_data() override;
  void refresh_screen(bool partial) override;
  void power_on() override;
  void power_off() override;
  void deep_sleep() override;

  uint8_t level_(Color color) const;
  void plane_bytes_(const Source &source, size_t y, uint8_t *out);
  void send_init_();
  // Load a waveform's five tables at a frame rate.
  void load_waveform_(const uint8_t *waveform, uint8_t frame_rate);
  // This update's frame rates, from the temperature.
  void set_frame_rates_();
  void add_reader_();
  void read_panel_temperature_();
  // Send the planes from plane_ on, OLD then NEW, in slices; true once both are out.
  bool send_planes_(const Source &old_source, const Source &new_source);
  bool find_changed_rows_();
  void plan_update_();
  void record_glass_(bool fast, bool corrected);
  void wind_down_();
  void give_up_();
  void reset_controller_();
  void settle_now_();
  void wait_idle_(uint32_t timeout_ms);
  void switch_supplies_on_();
  void switch_supplies_off_();

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
  bool ready_{false};           // the controller has been reset and initialised
  bool supplies_on_{false};     // the ±15 V supplies are on
  uint8_t frame_rate_{0x09};    // this update's, for its temperature
  uint8_t half_rate_{0x1A};     // the seven-grey half steps', and the steps themselves
  uint8_t half_to_black_[WAVEFORM]{};
  uint8_t half_to_white_[WAVEFORM]{};
  bool read_temperature_{false};  // the supplies have just come on, so the sensor has a new reading
  float panel_celsius_{NAN};
  spi_device_handle_t reader_{nullptr};
#ifdef USE_SENSOR
  sensor::Sensor *temperature_{nullptr};
#endif
  uint32_t supplies_since_{0};
  uint8_t plane_{0};            // 0: OLD, 1: NEW
  bool sending_{false};         // a plane is part way out
  uint16_t row_{0};             // the next row of it
  uint8_t *glass_{nullptr};     // the glass's black and white reading, a bit a pixel
  uint8_t *unsettled_{nullptr}; // pixels a fast partial left a push out of balance
  bool glass_valid_{false};
  uint32_t unsettled_count_{0};
  uint8_t *pixels_{nullptr};          // the buffer, when it is one block
  uint32_t last_colour_{UINT32_MAX};  // draw_pixel_at's last colour and its level
  uint8_t last_level_{0};
};

}  // namespace esphome::crowpanel_42
