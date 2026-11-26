#pragma once

#include "esphome/components/crowpanel_epaper/crowpanel_epaper.h"

namespace esphome::crowpanel_579 {

/**
 * ELECROW CrowPanel ESP32-S3 5.79" e-paper: 792x272, four or seven greys. CrowPanelBase holds what
 * the CrowPanel drivers share; this is the panel's controllers.
 *
 * The panel is two SSD1683 controllers in cascade behind one chip select. The left controller
 * takes the same commands with 0x80 set (0xA4 for 0x24 and so on); everything without a RAM or
 * address argument goes to both. Each controller has 400 columns of RAM but shows 396, so a
 * row of RAM bytes splits at byte 49, which both receive: the left controller shows its high
 * nibble and the right one its low nibble.
 *
 * An update resets and initialises the controllers unless the last one left them awake, sends
 * the RAM planes (0x26 the old, 0x24 the new), and refreshes, leaving the ±15 V supplies on for a
 * few seconds in case another update follows, and the controllers awake a while longer so the
 * next skips their reset. With grey off, a full refresh is the panel's own black and white one.
 * Through deep sleep the controllers are in deep sleep mode 1, which keeps their RAM.
 */
class CrowPanel579 : public crowpanel_epaper::CrowPanelBase {
 public:
  static constexpr uint16_t WIDTH = 792;
  static constexpr uint16_t HEIGHT = 272;
  static constexpr size_t PLANE_ROW = WIDTH / 8;  // a RAM plane's row: 8 pixels a byte

  CrowPanel579() : CrowPanelBase("CrowPanel 5.79", WIDTH, HEIGHT) { this->frame_rate_ = 0x02; }

 protected:
  static constexpr size_t HALF = 50;                       // one controller's 400 columns
  static constexpr size_t RIGHT_OFFSET = 49;               // the right half starts with the shared byte
  static constexpr size_t BIT_PLANE = PLANE_ROW * HEIGHT;  // a bit a pixel
  static constexpr size_t ROWS_A_WRITE = 16;

  bool reset() override;
  bool initialise(bool partial) override;
  void refresh_screen(bool partial) override;
  void power_on() override {}
  void deep_sleep() override;

  bool send_planes_(const Source &old_source, const Source &new_source) override;
  void start_pass_(Pass pass) override;
  // A hardware reset, which also switches the supplies off, then deep sleep.
  void reset_controllers_() override;
  void settle_now_() override;
  void prepare_for_sleep_(bool settled) override { this->sleep_controllers_(); }
  void plan_started_() override { this->frame_rate_ = this->frame_rate_for_temperature_(); }
  // Both planes get the reading after a refresh with a loaded waveform, as GxEPD2's writeImageAgain
  // does, because such a refresh swaps the banks 0x24 and 0x26 address until the next reset (RAM
  // ping-pong, 0x37): with both holding the same frame, the next partial is right with or without a
  // reset in between. The panel's own black and white refresh writes both planes before it and
  // does not swap.
  bool copies_after_(Pass last) const override { return last != Pass::BLACK_WHITE; }
  void forget_() override { this->init_step_ = 0; }

  // Point one controller's RAM window and address counters at rows first_y to end_y of its half.
  void set_ram_area_(bool left, uint16_t first_y, uint16_t end_y);
  // Load a 233-byte waveform (0x32 and its tail) at a frame rate and run it.
  void load_waveform_(const uint8_t *waveform, uint8_t frame_rate);
  uint8_t frame_rate_for_temperature_() const;
  int band_() const;
  void wind_down_();
  void switch_supplies_off_();
  // Put the controllers into deep sleep now, supplies off first.
  void sleep_controllers_();

  uint8_t init_step_{0};
  bool awake_{false};           // the controllers have been reset and not slept since
};

}  // namespace esphome::crowpanel_579
