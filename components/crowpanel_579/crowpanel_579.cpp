#include "crowpanel_579.h"

#include <cstring>
#include <initializer_list>

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
// How long one loop() call may run the update's steps back to back, well inside ESPHome's 50 ms.
static constexpr uint32_t STEP_BUDGET_MS = 25;
// How long the controllers stay awake after an update, so the next one skips their reset.
static constexpr uint32_t CONTROLLER_IDLE_MS = 10000;

// Display update control (0x22) sequences. The full one reads the panel's temperature sensor and
// loads the matching OTP waveform first; the partial and grey ones run a waveform loaded with 0x32.
static constexpr uint8_t UPDATE_FULL = 0xF7;
static constexpr uint8_t UPDATE_LOADED = 0xCF;
// Border waveform (0x3C): white on a full refresh, left alone on a partial one, and the
// loaded waveform's on a grey one.
static constexpr uint8_t BORDER_FULL = 0x01;
static constexpr uint8_t BORDER_PARTIAL = 0x80;
static constexpr uint8_t BORDER_GREY = 0x03;

// Loaded waveforms use the SSD1683's black and white mode: 227 bytes for 0x32, then 0x3F (end
// option), 0x03 (VGH), 0x04 (VSH1, VSH2, VSL) and 0x2C (VCOM). 0x32 takes five tables of 42
// bytes, VCOM's and then one for each pixel state, named by the pixel's 0x24 bit then its 0x26
// bit, 1 white: WW, BW, WB, BB. A table is six groups of seven bytes: a repeat count, four phases
// (the level in the top two bits, frames in the rest) and two state repeats. VSH1 (+15 V) drives
// a pixel black and VSL (-15 V) white. Every table below spends as long at one as at the other,
// so no refresh leaves a net drive on any pixel.

// Waveshare's four-grey waveform for their 4.2in V2 panel, also an SSD1683, from their
// epd4in2_V2.py, with two changes. Group 1, which only shakes the particles (the same balanced
// drive in every table), runs once rather than five times: the refresh takes 3.7 s rather than
// 5.5 s, and the four greys measure the same. And the end option is 0x22, as the datasheet asks,
// which discharges the pixels after the last phase; Waveshare's 0x07 leaves them charged to it.
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
  0x22, 0x17, 0x41, 0xA8, 0x32, 0x30
};

// The partial refresh's waveform, balanced on every change rather than only over time: a pixel
// turning black is pushed towards white for 15 frames, then towards black for 15, and one turning
// white the other way round. The first push only deepens the colour the pixel already shows, so
// nothing flashes. Pixels that keep their colour get no drive at all, so greys survive any number
// of partial refreshes, where the panel's own partial waveform fades them. Busy for 0.85 s.
struct PartialWaveform {
  uint8_t bytes[233];
  constexpr PartialWaveform() : bytes() {
    constexpr uint8_t FRAMES = 15, NONE = FRAMES, BLACK = 0x40 | FRAMES, WHITE = 0x80 | FRAMES;
    // Group 0's phases A and B for VCOM, WW, BW, WB and BB. The other groups repeat 0 times.
    constexpr uint8_t phases[5][2] = {{NONE, NONE}, {NONE, NONE}, {WHITE, BLACK}, {BLACK, WHITE}, {NONE, NONE}};
    for (int t = 0; t < 5; t++) {
      bytes[t * 42] = 1;
      bytes[t * 42 + 1] = phases[t][0];
      bytes[t * 42 + 2] = phases[t][1];
      bytes[t * 42 + 5] = 1;
      bytes[t * 42 + 6] = 1;
    }
    bytes[224] = GREY_LUT[224];  // the frame rate
    for (int i = 227; i < 233; i++)
      bytes[i] = GREY_LUT[i];  // the end option and the voltages
  }
};
static constexpr PartialWaveform PARTIAL{};

// The seven-grey refresh's middle pass: the four-grey waveform with its dark and light tables
// driven less, so dark grey stands clear of black and a half step fits between each grey. The
// dark table becomes the light one above, 15 frames of black drive; the light table gets 10,
// balanced by 10 frames of white first.
struct EvenGreyWaveform {
  uint8_t bytes[233];
  constexpr EvenGreyWaveform() : bytes() {
    for (int i = 0; i < 233; i++)
      bytes[i] = GREY_LUT[i];
    constexpr int BW = 84, WB = 126;  // the light and dark tables
    for (int i = 0; i < 42; i++)
      bytes[WB + i] = GREY_LUT[BW + i];
    constexpr uint8_t group0[4] = {0x8A, 0x1B, 0x0F, 0x03}, group2[4] = {0x48, 0x03, 0x82, 0x84};
    for (int i = 0; i < 4; i++) {
      bytes[BW + 1 + i] = group0[i];
      bytes[BW + 14 + 1 + i] = group2[i];
    }
  }
};
static constexpr EvenGreyWaveform EVEN_GREY{};

// The seven-grey refresh's outer passes: a half step for the odd levels, towards black before the
// middle pass and towards white after it, so the two cancel. A grey answers a push by how dark it
// is, so each band gets its own length: 4 frames up from black, 1 from dark grey, 2 from light.
// The pixel's two RAM bits say which: 0 none (BB), 1 from black (WB), 2 from dark (BW), 3 from
// light (WW).
struct HalfStepWaveform {
  uint8_t bytes[233];
  constexpr HalfStepWaveform(bool white) : bytes() {
    constexpr uint8_t lengths[3] = {1, 1, 2};  // phases A, B and C
    constexpr uint8_t phases_of_table[5] = {0, 0b011, 0b001, 0b111, 0};  // VCOM, WW, BW, WB, BB
    const uint8_t drive = white ? 0x80 : 0x40;
    for (int t = 0; t < 5; t++) {
      bytes[t * 42] = 1;
      for (int p = 0; p < 3; p++)
        bytes[t * 42 + 1 + p] = (((phases_of_table[t] >> p) & 1) ? drive : 0) | lengths[p];
      bytes[t * 42 + 5] = 1;
      bytes[t * 42 + 6] = 1;
    }
    bytes[224] = GREY_LUT[224];
    for (int i = 227; i < 233; i++)
      bytes[i] = GREY_LUT[i];
  }
};
static constexpr HalfStepWaveform HALF_TO_BLACK{false}, HALF_TO_WHITE{true};

// The net drive a waveform leaves on pixels using table t, in frames: VSH1 counts one way and VSL
// the other, with the group and state repeats. Returns a value far from 0 for anything this driver
// should never send: VSH2, a VCOM table that leaves its DC level, or VSH1 and VSL other than
// +15 V and -15 V.
static constexpr int net_drive(const uint8_t *waveform, int t) {
  constexpr int INVALID = 1 << 20;
  if (waveform[229] != 0x41 || waveform[231] != 0x32)
    return INVALID;
  int net = 0;
  for (int g = 0; g < 6; g++) {
    const uint8_t *group = waveform + t * 42 + g * 7;
    for (int pair = 0; pair < 2; pair++) {
      int frames = 0;
      for (int i = 0; i < 2; i++) {
        const int level = group[1 + 2 * pair + i] >> 6, length = group[1 + 2 * pair + i] & 0x3F;
        if (level == 3 || (t == 0 && level != 0))
          return INVALID;
        frames += level == 1 ? length : level == 2 ? -length : 0;
      }
      net += frames * group[5 + pair] * group[0];
    }
  }
  return net;
}
// Whether the waveforms run in turn leave no net drive on any pixel.
static constexpr bool balanced(std::initializer_list<const uint8_t *> waveforms) {
  for (int t = 0; t < 5; t++) {
    int net = 0;
    for (const uint8_t *waveform : waveforms)
      net += net_drive(waveform, t);
    if (net != 0)
      return false;
  }
  return true;
}
static_assert(balanced({PARTIAL.bytes}), "the partial waveform must leave no net drive on a pixel");
static_assert(balanced({GREY_LUT}), "the grey waveform must leave no net drive on a pixel");
static_assert(balanced({EVEN_GREY.bytes}), "the even grey waveform must leave no net drive on a pixel");
static_assert(balanced({HALF_TO_BLACK.bytes, HALF_TO_WHITE.bytes}), "the half steps must cancel");

// For each RAM bit a refresh needs, the levels (0 to 6) that set it, and from that a table from a
// buffer byte, two pixels with the first in the high nibble, to their two bits.
static constexpr uint8_t WHITE = 0b1110000;       // levels 4 to 6: a partial's reading, and level / 2's high bit
static constexpr uint8_t GREY_LOW = 0b1001100;    // levels 2, 3 and 6: level / 2's low bit
static constexpr uint8_t HALF_HIGH = 0b0101000;   // levels 3 and 5: the half step's high bit
static constexpr uint8_t HALF_LOW = 0b0100010;    // levels 1 and 5: the half step's low bit
struct PlaneBits {
  uint8_t table[256];
  constexpr PlaneBits(uint8_t levels) : table() {
    for (int v = 0; v < 256; v++)
      table[v] = (((levels >> (v >> 4)) & 1) << 1) | ((levels >> (v & 0x0F)) & 1);
  }
};
static constexpr PlaneBits WHITE_BITS{WHITE}, GREY_LOW_BITS{GREY_LOW}, HALF_HIGH_BITS{HALF_HIGH},
    HALF_LOW_BITS{HALF_LOW};

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
  // With PSRAM the buffer is one block, and drawing indexes it directly.
  size_t length = 0;
  uint8_t *pixels = this->buffer_.get_span(0, length);
  this->pixels_ = length == this->buffer_length_ ? pixels : nullptr;
  // Waking from deep sleep, the panel kept its power, its picture and its RAM, so the count
  // carries on and the first update can be partial.
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED && this->is_using_partial_update_()) {
    this->update_count_ = slept_update_count % this->full_update_every_;
    ESP_LOGD(TAG, "Woke from deep sleep; the panel still holds its frame (update %u of %u)",
             (unsigned) this->update_count_, (unsigned) this->full_update_every_);
  }
}

// Before a deep sleep: finish writing the planes if a refresh has just run, then sleep the
// controllers and hold the panel's power and reset pins high so it keeps its RAM.
void CrowPanel579::on_safe_shutdown() {
  while (this->copying_ && !this->send_planes_(WHITE_BITS.table, WHITE_BITS.table)) {
  }
  this->copying_ = false;
  // An update cut short, a seven-grey one between passes included, may leave the panel's RAM out
  // of step with the glass, so the next wake starts with a full refresh.
  using epaper_spi::EPaperState;
  const bool settled =
      !this->next_pass_ && (this->state_ == EPaperState::IDLE || this->state_ == EPaperState::DEEP_SLEEP);
  slept_update_count = settled ? this->update_count_ : 0;
  this->sleep_controllers_();
  hold(this->power_pin_, true);
  hold(this->hold_reset_pin_, true);
  gpio_deep_sleep_hold_en();
}

// Four greys use the even levels, in even steps of luma so anti-aliased text, which blends at
// 0, 85, 170 and 255, keeps all four. Seven use every level.
uint8_t CrowPanel579::level_(Color color) const {
  const uint8_t luma = epaper_spi::rec601_luma(color);
  return this->seven_ ? luma * 7 / 256 : (luma >> 6) * 2;
}

void CrowPanel579::fill(Color color) {
  if (this->get_clipping().is_set()) {
    display::Display::fill(color);  // the clipped rectangle, a pixel at a time
    return;
  }
  const uint8_t level = this->level_(color);
  this->buffer_.fill(level << 4 | level);
  this->x_low_ = 0;
  this->y_low_ = 0;
  this->x_high_ = this->width_;
  this->y_high_ = this->height_;
}

void HOT CrowPanel579::draw_pixel_at(int x, int y, Color color) {
  if (!this->rotate_coordinates_(x, y))
    return;
  // A rectangle or a run of text is one colour, so its level is worked out once.
  if (color.raw_32 != this->last_colour_) {
    this->last_colour_ = color.raw_32;
    this->last_level_ = this->level_(color);
  }
  const size_t index = y * this->row_width_ + x / 2;
  uint8_t &pair = this->pixels_ != nullptr ? this->pixels_[index] : this->buffer_[index];
  const uint8_t level = this->last_level_;
  pair = (x & 1) ? (pair & 0xF0) | level : (pair & 0x0F) | level << 4;  // the first pixel in the high nibble
}

void CrowPanel579::update() {
  this->cancel_timeout("sleep");
  EPaperBase::update();
}

// Controllers still awake from the last update need no reset or initialisation: every refresh
// loads its own waveform and settings, and every plane it reads is written for it.
bool CrowPanel579::reset() {
  if (this->awake_)
    return true;
  if (!EPaperBase::reset())
    return false;  // reset is held low; the base calls back after reset_duration_
  this->next_delay_ = 10;  // give the controllers time to come out of reset before BUSY is read
  return true;
}

// A software reset, then the internal temperature sensor; the refresh loads its own waveform.
// The reset sets the controllers working, so BUSY is polled again after it.
bool CrowPanel579::initialise(bool partial) {
  if (this->awake_)
    return true;
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
  this->awake_ = true;
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

void HOT CrowPanel579::plane_half_(size_t y, size_t offset, const uint8_t *bits, uint8_t *out) {
  // Plane byte offset + i holds the 8 pixels of buffer bytes 4 (offset + i) to the third after.
  const size_t start = y * this->row_width_ + 4 * offset;
  uint8_t copy[4 * HALF];
  const uint8_t *in = this->pixels_ + start;
  if (this->pixels_ == nullptr) {
    for (size_t i = 0; i != sizeof copy; i++)
      copy[i] = this->buffer_[start + i];
    in = copy;
  }
  for (size_t i = 0; i != HALF; i++, in += 4)
    out[i] = bits[in[0]] << 6 | bits[in[1]] << 4 | bits[in[2]] << 2 | bits[in[3]];
}

// Sends planes plane_ to 3, a 10 ms slice at a time: 0x26 then 0x24, each for the left half then
// the right, through old_bits and new_bits. Returns true once the last is out.
bool HOT CrowPanel579::send_planes_(const uint8_t *old_bits, const uint8_t *new_bits) {
  const uint32_t start = millis();
  uint8_t row[HALF];
  while (this->plane_ != 4) {
    const bool left = (this->plane_ & 1) == 0;
    const bool new_plane = this->plane_ >= 2;
    if (this->current_data_index_ == 0) {
      this->set_ram_area_(left);
      this->command((new_plane ? 0x24 : 0x26) | (left ? LEFT : 0));
    }
    const size_t offset = left ? 0 : RIGHT_OFFSET;
    this->start_data_();
    while (this->current_data_index_ != HEIGHT) {
      this->plane_half_(this->current_data_index_, offset, new_plane ? new_bits : old_bits, row);
      this->write_array(row, HALF);
      ++this->current_data_index_;
      if (this->current_data_index_ != HEIGHT && millis() - start > epaper_spi::MAX_TRANSFER_TIME) {
        this->disable();
        return false;
      }
    }
    this->disable();
    this->current_data_index_ = 0;
    this->plane_++;
  }
  this->plane_ = 0;
  return true;
}

// The first call of an update decides what kind it is. A partial refresh sends only 0x24, the new
// frame: 0x26 already holds what is on the glass. Every other refresh sends both planes.
bool CrowPanel579::transfer_data() {
  if (this->plane_ == 0 && this->current_data_index_ == 0 && this->grey_pass_ == 0) {
    if (this->update_count_ != 0) {
      this->kind_ = Kind::PARTIAL;
      this->plane_ = 2;
    } else {
      this->kind_ = !this->grey_ ? Kind::BLACK_WHITE : this->seven_ ? Kind::SEVEN_GREYS : Kind::GREY;
    }
  }
  switch (this->kind_) {
    case Kind::PARTIAL:
    case Kind::BLACK_WHITE:
      return this->send_planes_(WHITE_BITS.table, WHITE_BITS.table);
    case Kind::GREY:
      return this->send_planes_(WHITE_BITS.table, GREY_LOW_BITS.table);
    case Kind::SEVEN_GREYS:
      if (this->grey_pass_ == 1)
        return this->send_planes_(WHITE_BITS.table, GREY_LOW_BITS.table);
      return this->send_planes_(HALF_HIGH_BITS.table, HALF_LOW_BITS.table);
  }
  return true;
}

// The loaded waveforms need 0x21's second byte at 0x10, which GxEPD2 calls the cascade bit;
// without it, changed pixels stop half way, at grey.
void CrowPanel579::refresh_screen(bool partial) {
  switch (this->kind_) {
    case Kind::PARTIAL:
      ESP_LOGV(TAG, "Partial refresh");
      this->cmd_data(0x3C, {BORDER_PARTIAL});
      this->cmd_data(0x21, {0x00, 0x10});
      this->load_waveform_(PARTIAL.bytes);
      break;
    case Kind::GREY:
      ESP_LOGV(TAG, "Grey refresh");
      this->cmd_data(0x3C, {BORDER_GREY});
      this->cmd_data(0x21, {0x00, 0x10});
      this->load_waveform_(GREY_LUT);
      break;
    case Kind::SEVEN_GREYS:
      ESP_LOGV(TAG, "Seven-grey refresh, pass %u", (unsigned) this->grey_pass_ + 1);
      this->cmd_data(0x3C, {this->grey_pass_ == 1 ? BORDER_GREY : BORDER_PARTIAL});
      this->cmd_data(0x21, {0x00, 0x10});
      this->load_waveform_(this->grey_pass_ == 0   ? HALF_TO_BLACK.bytes
                           : this->grey_pass_ == 1 ? EVEN_GREY.bytes
                                                   : HALF_TO_WHITE.bytes);
      break;
    case Kind::BLACK_WHITE:
      ESP_LOGV(TAG, "Full refresh");
      this->cmd_data(0x3C, {BORDER_FULL});
      this->cmd_data(0x22, {UPDATE_FULL});
      break;
  }
  this->command(0x20);
}

// Runs once a refresh has finished. A seven-grey refresh has passes left: loop() starts the next.
// After the last, the frame's black and white reading goes to both planes for the next partial
// to compare with; the base moves straight on to sleeping the controllers, so loop() writes them
// first, in slices. Both planes, as GxEPD2's writeImageAgain does, because a refresh with a loaded
// waveform swaps the banks 0x24 and 0x26 address until the next reset (RAM ping-pong, 0x37): with
// both holding the same frame, the next partial is right with or without a reset in between.
// The panel's own black and white refresh writes both planes before it and does not swap.
void CrowPanel579::power_off() {
  if (this->kind_ == Kind::SEVEN_GREYS && this->grey_pass_ < 2) {
    this->grey_pass_++;
    this->next_pass_ = true;
    return;
  }
  this->grey_pass_ = 0;
  this->copying_ = this->kind_ != Kind::BLACK_WHITE;
}

// The base runs one step of the update a loop() call, and ESPHome calls loop() about every 16 ms,
// so the steps run back to back here instead, until one has to wait (BUSY, a delay) or the
// budget is spent.
void CrowPanel579::loop() {
  const uint32_t start = millis();
  while (millis() - start < STEP_BUDGET_MS) {
    if (this->copying_) {
      if (!this->send_planes_(WHITE_BITS.table, WHITE_BITS.table))
        continue;
      this->copying_ = false;
    }
    // Each pass of a seven-grey refresh is a full update to the base, and the count goes back so
    // the three count as one.
    if (this->next_pass_ && this->state_ == epaper_spi::EPaperState::DEEP_SLEEP) {
      this->next_pass_ = false;
      this->update_count_ = 0;
      this->set_state_(epaper_spi::EPaperState::RESET);
    }
    const auto state = this->state_;
    const auto row = this->current_data_index_;
    const auto plane = this->plane_;
    EPaperBase::loop();
    if (this->state_ == state && this->current_data_index_ == row && this->plane_ == plane)
      return;  // waiting
  }
}

// Loads a waveform and runs it: 0x22 0xCF skips the OTP's temperature lookup and waveform.
void CrowPanel579::load_waveform_(const uint8_t *waveform) {
  this->cmd_data(0x0C, {0x8B, 0x9C, 0xA4, 0x0F});  // booster soft start, as Waveshare sets it
  this->cmd_data(0x32, waveform, 227);
  this->cmd_data(0x3F, {waveform[227]});
  this->cmd_data(0x03, {waveform[228]});
  this->cmd_data(0x04, {waveform[229], waveform[230], waveform[231]});
  this->cmd_data(0x2C, {waveform[232]});
  this->cmd_data(0x22, {UPDATE_LOADED});
}

// The base sleeps the controllers after every update; here they stay awake for a while, so a quick
// run of updates, a dial turning, skips the reset and initialisation each time.
void CrowPanel579::deep_sleep() {
  this->set_timeout("sleep", CONTROLLER_IDLE_MS, [this]() { this->sleep_controllers_(); });
}

// Deep sleep mode 1 keeps RAM. The next update wakes the controllers with a hardware reset.
void CrowPanel579::sleep_controllers_() {
  this->cancel_timeout("sleep");
  this->cmd_data(0x10, {0x01});
  this->awake_ = false;
}

}  // namespace esphome::crowpanel_579
