#pragma once

#include <driver/spi_master.h>

#include "esphome/components/crowpanel_epaper/crowpanel_epaper.h"

namespace esphome::crowpanel_42 {

/**
 * ELECROW CrowPanel ESP32-S3 4.2" e-paper, hardware V1.2: 400x300, four or seven greys.
 * CrowPanelBase holds what the CrowPanel drivers share; this is the panel's controller.
 *
 * The panel's controller is a UC8276C whose OTP holds no waveforms, so every refresh runs one this
 * driver loads. Its waveform format is the SSD1683's: per table, groups of a repeat count, four
 * phases (the level in the top two bits, frames in the rest) and two state repeats, with VSH
 * (+15 V) driving a pixel black and VSL (-15 V) white. Each pixel picks a table from its bit in the
 * NEW plane (DTM2, 0x13) and its bit in the OLD plane (DTM1, 0x10), 1 white. With grey off, a full
 * refresh is a balanced black and white one.
 *
 * An update resets and initialises the controller unless the last one left it so, sends the
 * planes, and refreshes, leaving the ±15 V supplies on for a few seconds in case another update
 * follows. The controller never copies NEW to OLD, so after every update the driver writes the
 * glass's black and white reading to both. Its RAM survives a hardware reset, and through deep
 * sleep the controller stays in standby with the panel's power and reset pins held high.
 *
 * With grey partials, a partial update is a grey refresh of a partial window around what changed,
 * which no other pixel feels; after a wake, of the band of rows whose levels changed.
 */
class CrowPanel42 : public crowpanel_epaper::CrowPanelBase {
 public:
  static constexpr uint16_t WIDTH = 400;
  static constexpr uint16_t HEIGHT = 300;
  static constexpr size_t PLANE_ROW = WIDTH / 8;  // a RAM plane's row: 8 pixels a byte
  static constexpr size_t WAVEFORM = 5 * 42;      // a waveform: five tables of six seven-byte groups

  CrowPanel42() : CrowPanelBase("CrowPanel 4.2", WIDTH, HEIGHT) { this->frame_rate_ = 0x09; }

  // Partial updates redraw the window around what changed in greys, flashing only it, instead of
  // pushing the changed pixels to black or white. Needs grey.
  void set_grey_partials(bool grey_partials) { this->grey_partials_ = grey_partials; }
  // Grey refreshes in two shakes and a push: about 2 s for four greys rather than 3.7, erasing the
  // picture before as well.
  void set_fast_greys(bool fast_greys) { this->fast_greys_ = fast_greys; }
  // Make the next update, if partial, a grey refresh of the window around everything changed since
  // the last frame drawn in greys, black and white partials included: so a busy screen can answer
  // in black and white at once and settle into greys where it changed, once it is still. Needs grey.
  void request_grey_partial() { this->settle_requested_ = true; }
  // The temperature the panel sensed when its supplies last came on, in °C; NAN before then.
  float panel_temperature() const { return this->panel_celsius_; }

 protected:
  static constexpr size_t BIT_PLANE = PLANE_ROW * HEIGHT;  // a bit a pixel
  static constexpr size_t ROWS_A_WRITE = 16;

  bool reset() override;
  bool initialise(bool partial) override;
  void refresh_screen(bool partial) override;
  void power_on() override;
  void deep_sleep() override;

  bool send_planes_(const Source &old_source, const Source &new_source) override;
  bool send_own_planes_(Pass pass) override;
  void start_pass_(Pass pass) override;
  // A hardware reset, which switches the supplies off and the registers back to their defaults but
  // keeps the RAM.
  void reset_controllers_() override;
  void settle_now_() override;
  void prepare_for_sleep_(bool settled) override;
  void setup_controllers_() override;
  void plan_started_() override { this->windowed_ = false; }
  bool plan_own_update_() override;
  void pass_finished_() override;
  void update_finished_(Pass last) override;
  void reading_changed_(size_t y, size_t i) override;
  const GlideTuning *glide_tuning_() const override;
  float speed_() const override;
  void restore_reading_() override;
  bool settles_windows_() const override { return true; }
  void request_settle_() override { this->settle_requested_ = true; }
  void rows_drawn_(uint16_t first, uint16_t last) override;
  bool corrects_(size_t y, size_t i) const override { return !this->windowed_ || this->in_window_(y, i); }
  void forget_() override;

  void send_init_();
  // Load a waveform's five tables at a frame rate.
  void load_waveform_(const uint8_t *waveform, uint8_t frame_rate);
  // This update's frame rates, from the temperature.
  void set_frame_rates_();
  void add_reader_();
  void read_panel_temperature_();
  bool find_changed_band_();
  uint32_t level_row_hash_(size_t y) const;
  bool in_window_(size_t y, size_t bank) const;
  bool unsettled_in_window_() const;
  void send_window_();
  void enter_window_(const Window &window);
  void wind_down_();
  void switch_supplies_on_();
  void switch_supplies_off_();

  bool grey_partials_{false};
  bool fast_greys_{false};
  bool ready_{false};           // the controller has been reset and initialised
  uint8_t half_rate_{0x1A};     // the seven-grey half steps', and the steps themselves
  uint8_t half_to_black_[WAVEFORM]{};
  uint8_t half_to_white_[WAVEFORM]{};
  uint8_t fast_rate_{0x13};     // the fast greys', and the waveforms at it
  uint8_t fast_four_[WAVEFORM]{};
  uint8_t fast_even_[WAVEFORM]{};
  bool read_temperature_{false};  // the supplies have just come on, so the sensor has a new reading
  float panel_celsius_{NAN};
  spi_device_handle_t reader_{nullptr};
  bool windowed_{false};              // this update refreshes only window_
};

}  // namespace esphome::crowpanel_42
