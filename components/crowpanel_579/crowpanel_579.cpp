#include "crowpanel_579.h"

#include <cstring>

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::crowpanel_579 {

static const char *const TAG = "crowpanel_579";

// OR'd into a command to address the left controller instead of the right.
static constexpr uint8_t LEFT = 0x80;
static constexpr uint8_t LAST_X = 0x31;  // the 50th byte of a controller's RAM row
static constexpr uint8_t LAST_Y_LOW = (CrowPanel579::HEIGHT - 1) & 0xFF;
static constexpr uint8_t LAST_Y_HIGH = (CrowPanel579::HEIGHT - 1) >> 8;

// Display update control (0x22) sequences. The full and partial ones read the panel's
// temperature sensor and load the matching OTP waveform first; the grey one runs the waveform
// loaded with 0x32.
static constexpr uint8_t UPDATE_FULL = 0xF7;
static constexpr uint8_t UPDATE_PARTIAL = 0xFF;
static constexpr uint8_t UPDATE_LOADED = 0xCF;
// Border waveform (0x3C): white on a full refresh, left alone on a partial one, and the
// loaded waveform's on a grey one.
static constexpr uint8_t BORDER_FULL = 0x01;
static constexpr uint8_t BORDER_PARTIAL = 0x80;
static constexpr uint8_t BORDER_GREY = 0x03;

// Waveshare's four-grey waveform for their 4.2in V2 panel, also an SSD1683, from their
// epd4in2_V2.py: 227 bytes for 0x32, then 0x3F, 0x03 (VGH), 0x04 (VSH1, VSH2, VSL) and 0x2C
// (VCOM). One change: group 1, which only shakes the particles (the same balanced drive in every
// block), runs once rather than five times. That takes the refresh from 5.5 s to 3.7 s; on this
// panel the four greys measure the same, and a run of partials before it clears as well.
static constexpr uint8_t GREY_LUT[233] = {
  // VCOM, then a LUT per pixel state; 6 groups each of RP, phases A to D, SR_AB, SR_CD
  0x01, 0x0A, 0x1B, 0x0F, 0x03, 0x01, 0x01,  // VCOM group 0
  0x01, 0x0A, 0x01, 0x0A, 0x01, 0x01, 0x01,  // VCOM group 1
  0x05, 0x08, 0x03, 0x02, 0x04, 0x01, 0x01,  // VCOM group 2
  0x01, 0x04, 0x04, 0x02, 0x00, 0x01, 0x01,  // VCOM group 3
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // VCOM group 4
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // VCOM group 5
  0x01, 0x0A, 0x1B, 0x0F, 0x03, 0x01, 0x01,  // WW group 0
  0x01, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,  // WW group 1
  0x05, 0x48, 0x03, 0x82, 0x84, 0x01, 0x01,  // WW group 2
  0x01, 0x84, 0x84, 0x82, 0x00, 0x01, 0x01,  // WW group 3
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // WW group 4
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // WW group 5
  0x01, 0x0A, 0x1B, 0x8F, 0x03, 0x01, 0x01,  // BW group 0
  0x01, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,  // BW group 1
  0x05, 0x48, 0x83, 0x82, 0x04, 0x01, 0x01,  // BW group 2
  0x01, 0x04, 0x04, 0x02, 0x00, 0x01, 0x01,  // BW group 3
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // BW group 4
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // BW group 5
  0x01, 0x8A, 0x1B, 0x8F, 0x03, 0x01, 0x01,  // WB group 0
  0x01, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,  // WB group 1
  0x05, 0x48, 0x83, 0x02, 0x04, 0x01, 0x01,  // WB group 2
  0x01, 0x04, 0x04, 0x02, 0x00, 0x01, 0x01,  // WB group 3
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // WB group 4
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // WB group 5
  0x01, 0x8A, 0x9B, 0x8F, 0x03, 0x01, 0x01,  // BB group 0
  0x01, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,  // BB group 1
  0x05, 0x48, 0x03, 0x42, 0x04, 0x01, 0x01,  // BB group 2
  0x01, 0x04, 0x04, 0x42, 0x00, 0x01, 0x01,  // BB group 3
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // BB group 4
  0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,  // BB group 5
  // 210..223 reserved, 224 FR, 225..226 XON
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
  // tail: 0x3F, 0x03, 0x04 x3, 0x2C
  0x07, 0x17, 0x41, 0xA8, 0x32, 0x30
};

// A grey level from 0 (black) to 3 (white), in even steps of luma so anti-aliased text, which
// blends at 0, 85, 170 and 255, keeps all four.
static uint8_t grey_level(Color color) { return epaper_spi::rec601_luma(color) >> 6; }

// Where the last update left the full_update_every count, kept through deep sleep so the count
// carries on across wakes. Only read on a wake from deep sleep, so it is always one this
// driver wrote.
static RTC_DATA_ATTR uint8_t slept_update_count;

static void hold(InternalGPIOPin *pin, bool on) {
  if (pin == nullptr)
    return;
  const auto num = static_cast<gpio_num_t>(pin->get_pin());
  if (on) {
    gpio_hold_en(num);
  } else {
    gpio_hold_dis(num);
  }
}

void CrowPanel579::setup() {
  // The pins are already high if a deep sleep held them; set them up before letting go.
  EPaperBase::setup();
  hold(this->power_pin_, false);
  hold(this->hold_reset_pin_, false);
  if (this->is_failed())
    return;
  // Waking from deep sleep, the panel kept its power, its picture and its RAM, so the count
  // carries on and the first update can be partial.
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED && this->is_using_partial_update_()) {
    this->update_count_ = slept_update_count % this->full_update_every_;
    ESP_LOGD(TAG, "Woke from deep sleep; the panel still holds its frame (update %u of %u)",
             (unsigned) this->update_count_, (unsigned) this->full_update_every_);
  }
}

// Before a deep sleep: finish sending the planes if a partial refresh has just run, then sleep
// the controllers and hold the panel's power and reset pins high so it keeps its RAM.
void CrowPanel579::on_safe_shutdown() {
  while (this->copying_ && !this->send_planes_(false)) {
  }
  this->copying_ = false;
  // An update cut short before its refresh and that copy may leave the panel's RAM out of step
  // with the glass, so the next wake starts with a full refresh.
  using epaper_spi::EPaperState;
  const bool settled = this->state_ == EPaperState::IDLE || this->state_ == EPaperState::DEEP_SLEEP;
  slept_update_count = settled ? this->update_count_ : 0;
  this->deep_sleep();
  hold(this->power_pin_, true);
  hold(this->hold_reset_pin_, true);
  gpio_deep_sleep_hold_en();
}

void CrowPanel579::fill(Color color) {
  if (this->get_clipping().is_set()) {
    display::Display::fill(color);  // the clipped rectangle, a pixel at a time
    return;
  }
  this->buffer_.fill(grey_level(color) * 0x55);  // the level in all four pixels of a byte
  this->x_low_ = 0;
  this->y_low_ = 0;
  this->x_high_ = this->width_;
  this->y_high_ = this->height_;
}

void HOT CrowPanel579::draw_pixel_at(int x, int y, Color color) {
  if (!this->rotate_coordinates_(x, y))
    return;
  const size_t index = y * this->row_width_ + x / 4;
  const uint8_t shift = 6 - 2 * (x % 4);  // the first pixel in the top bits
  const uint8_t kept = this->buffer_[index] & ~(0x03 << shift);
  this->buffer_[index] = kept | (grey_level(color) << shift);
}

bool CrowPanel579::reset() {
  if (!EPaperBase::reset())
    return false;  // reset is held low; the base calls back after reset_duration_
  this->next_delay_ = 10;  // give the controllers time to come out of reset before BUSY is read
  return true;
}

// A software reset, then the internal temperature sensor; the refresh loads its own waveform.
// The reset sets the controllers working, so BUSY is polled again after it.
bool CrowPanel579::initialise(bool partial) {
  if (!this->is_idle_())
    return false;
  if (!this->reset_sent_) {
    this->command(0x12);
    this->reset_sent_ = true;
    this->delay_until_ = millis() + 10;  // BUSY rises a moment after the command
    return false;
  }
  this->cmd_data(0x18, {0x80});
  this->reset_sent_ = false;
  return true;
}

void CrowPanel579::set_ram_area_(bool left) {
  const uint8_t to = left ? LEFT : 0;
  // The right controller is mounted the other way round: its X counts down from the seam,
  // so both halves take a row's bytes left to right.
  const uint8_t first_x = left ? 0 : LAST_X;
  const uint8_t last_x = left ? LAST_X : 0;
  this->cmd_data(0x11 | to, {static_cast<uint8_t>(left ? 0x03 : 0x02)});  // data entry mode
  this->cmd_data(0x44 | to, {first_x, last_x});
  this->cmd_data(0x45 | to, {0x00, 0x00, LAST_Y_LOW, LAST_Y_HIGH});
  this->cmd_data(0x4E | to, {first_x});
  this->cmd_data(0x4F | to, {0x00, 0x00});
}

// A buffer byte is four pixels of two bits; these give its four high bits, or its four low
// bits, as a nibble, first pixel in the top bit.
struct Nibbles {
  uint8_t high[256], low[256];
  constexpr Nibbles() : high(), low() {
    for (int v = 0; v < 256; v++) {
      high[v] = ((v >> 4) & 8) | ((v >> 3) & 4) | ((v >> 2) & 2) | ((v >> 1) & 1);
      low[v] = ((v >> 3) & 8) | ((v >> 2) & 4) | ((v >> 1) & 2) | (v & 1);
    }
  }
};
static constexpr Nibbles NIBBLES{};

void HOT CrowPanel579::plane_half_(size_t y, size_t offset, bool high, uint8_t *out) {
  const uint8_t *table = high ? NIBBLES.high : NIBBLES.low;
  // Plane byte offset + i holds the 8 pixels of buffer bytes 2 (offset + i) and the next.
  size_t index = y * this->row_width_ + 2 * offset;
  for (size_t i = 0; i != HALF; i++, index += 2)
    out[i] = (table[this->buffer_[index]] << 4) | table[this->buffer_[index + 1]];
}

// Sends passes pass_ to 3, a 10 ms slice at a time: 0x26 then 0x24, each for the left half
// then the right. 0x26 always gets each pixel's high bit, its black or white reading; 0x24 gets
// the same, or each pixel's low bit when low_new is set. Returns true once the last pass is out.
bool HOT CrowPanel579::send_planes_(bool low_new) {
  const uint32_t start = millis();
  uint8_t row[HALF];
  while (this->pass_ != 4) {
    const bool left = (this->pass_ & 1) == 0;
    const bool new_plane = this->pass_ >= 2;
    if (this->current_data_index_ == 0) {
      this->set_ram_area_(left);
      this->command((new_plane ? 0x24 : 0x26) | (left ? LEFT : 0));
    }
    const size_t offset = left ? 0 : RIGHT_OFFSET;
    this->start_data_();
    while (this->current_data_index_ != HEIGHT) {
      this->plane_half_(this->current_data_index_, offset, !(new_plane && low_new), row);
      this->write_array(row, HALF);
      ++this->current_data_index_;
      if (this->current_data_index_ != HEIGHT && millis() - start > epaper_spi::MAX_TRANSFER_TIME) {
        this->disable();
        return false;
      }
    }
    this->disable();
    this->current_data_index_ = 0;
    this->pass_++;
  }
  this->pass_ = 0;
  return true;
}

// A full refresh sends all four passes, so 0x26 holds the frame's black and white reading once
// it has run. A partial refresh sends only 0x24, the new frame: 0x26 already holds what is on
// the glass.
bool CrowPanel579::transfer_data() {
  const bool partial = this->update_count_ != 0;
  if (this->pass_ == 0 && this->current_data_index_ == 0) {
    this->partial_sent_ = partial;
    if (partial)
      this->pass_ = 2;
  }
  return this->send_planes_(!partial && this->grey_);
}

// Runs once the refresh has finished. After a partial one, 0x26 still holds the frame before
// it, so both planes get the frame now on the glass, for the next partial to compare with. The
// base moves straight on to sleeping the controllers, so loop() sends them first, in slices.
// Both planes, as GxEPD2's writeImageAgain does: on this panel, writing 0x26 alone leaves the
// next partial in the same wake comparing against the wrong frame.
void CrowPanel579::power_off() {
  if (!this->partial_sent_)
    return;
  this->partial_sent_ = false;
  this->copying_ = true;
}

void CrowPanel579::loop() {
  if (this->copying_) {
    if (!this->send_planes_(false))
      return;
    this->copying_ = false;
  }
  EPaperBase::loop();
}

void CrowPanel579::refresh_screen(bool partial) {
  if (partial) {
    ESP_LOGV(TAG, "Partial refresh");
    this->cmd_data(0x3C, {BORDER_PARTIAL});
    // Changed pixels are only driven all the way when 0x21's second byte has 0x10 set
    // (GxEPD2 calls it the cascade bit); without it they stop half way, at grey.
    this->cmd_data(0x21, {0x00, 0x10});
    this->cmd_data(0x22, {UPDATE_PARTIAL});
  } else if (this->grey_) {
    ESP_LOGV(TAG, "Grey refresh");
    this->cmd_data(0x3C, {BORDER_GREY});
    this->cmd_data(0x21, {0x00, 0x10});
    this->cmd_data(0x0C, {0x8B, 0x9C, 0xA4, 0x0F});  // booster soft start, as Waveshare sets it
    this->cmd_data(0x32, GREY_LUT, 227);
    this->cmd_data(0x3F, {GREY_LUT[227]});
    this->cmd_data(0x03, {GREY_LUT[228]});
    this->cmd_data(0x04, {GREY_LUT[229], GREY_LUT[230], GREY_LUT[231]});
    this->cmd_data(0x2C, {GREY_LUT[232]});
    this->cmd_data(0x22, {UPDATE_LOADED});
  } else {
    ESP_LOGV(TAG, "Full refresh");
    this->cmd_data(0x3C, {BORDER_FULL});
    this->cmd_data(0x22, {UPDATE_FULL});
  }
  this->command(0x20);
}

// Deep sleep mode 1 keeps RAM. The next update wakes the controllers with a hardware reset.
void CrowPanel579::deep_sleep() { this->cmd_data(0x10, {0x01}); }

}  // namespace esphome::crowpanel_579
