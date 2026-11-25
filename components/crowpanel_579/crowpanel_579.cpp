#include "crowpanel_579.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::crowpanel_579 {

static const char *const TAG = "crowpanel_579";

// OR'd into a command to address the left controller instead of the right.
static constexpr uint8_t LEFT = 0x80;
static constexpr uint8_t LAST_X = 0x31;  // the 50th byte of a controller's RAM row
// How long one loop() call may run the update's steps back to back, well inside ESPHome's 50 ms.
static constexpr uint32_t STEP_BUDGET_MS = 25;
// An active spell lasts until this long passes without an update; then the supplies go off, and
// after CONTROLLER_IDLE_MS more the controllers sleep, so an update in between skips their reset.
static constexpr uint32_t ACTIVE_MS = 5000;
static constexpr uint32_t CONTROLLER_IDLE_MS = 10000;

// Display update control (0x22) sequences. The full one switches the supplies on, reads the
// panel's temperature sensor, loads the matching OTP waveform, runs it and switches them off. The
// others run a waveform loaded with 0x32, switching the supplies on first if they are off and
// leaving them on; switching them on or off takes about 0.1 s each.
static constexpr uint8_t UPDATE_FULL = 0xF7;
static constexpr uint8_t UPDATE_SUPPLIES_DISPLAY = 0xCC;
static constexpr uint8_t UPDATE_DISPLAY = 0x0C;
static constexpr uint8_t SUPPLIES_OFF = 0x03;
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

// The waveforms of an active spell, while updates come quickly: group 0 only, with phases A to C
// of the given lengths and, for each table, the level in each phase (0 none, 1 VSH1, black,
// 2 VSL, white), and the grey waveform's frame rate and voltages.
struct SimpleWaveform {
  uint8_t bytes[233];
  constexpr SimpleWaveform(const uint8_t (&lengths)[3], const uint8_t (&levels)[5][3]) : bytes() {
    for (int t = 0; t < 5; t++) {
      bytes[t * 42] = 1;
      for (int p = 0; p < 3; p++)
        bytes[t * 42 + 1 + p] = levels[t][p] << 6 | lengths[p];
      bytes[t * 42 + 5] = 1;
      bytes[t * 42 + 6] = 1;
    }
    bytes[224] = GREY_LUT[224];
    for (int i = 227; i < 233; i++)
      bytes[i] = GREY_LUT[i];
  }
};
static constexpr uint8_t FAST_FRAMES = 15;
static constexpr uint8_t TABLE_WW = 1, TABLE_BW = 2, TABLE_WB = 3, TABLE_BB = 4;
// A fast partial pushes a changing pixel one way only, as the panel's own partial does: 15 frames
// towards its new colour, half the balanced partial's 30. Each one leaves its pixel a push out of
// balance, which the driver records and later cancels.
static constexpr SimpleWaveform ONE_WAY{{FAST_FRAMES, 0, 0}, {{0}, {0}, {1}, {2}, {0}}};
// Cancelling, with 0x26 saying which pixels are out of balance and 0x24 what the glass shows: a
// push back the other way. It runs just before a grey refresh, whose flash hides it.
static constexpr SimpleWaveform CORRECTION{{FAST_FRAMES, 0, 0}, {{0}, {1}, {2}, {0}, {0}}};
// The same cancelling when no grey refresh follows: a push back long enough to take the pixel all
// the way over, then 25 frames of its own colour, so it ends where it was, having blinked. 40
// frames against 25 is the one push it cancels; 30 against 15 left whites at 0.96 on the camera.
static constexpr uint8_t SETTLE_RETURN = 25;
static constexpr SimpleWaveform SETTLE{
    {FAST_FRAMES + SETTLE_RETURN, SETTLE_RETURN, 0}, {{0}, {1, 2}, {2, 1}, {0}, {0}}};

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

// A white pixel out of balance got there by a fast partial's push towards white (WB), a black one
// by its push towards black (BW); cancelling and settling must undo exactly that, and touch
// nothing else.
static constexpr bool cancels(const uint8_t *waveform) {
  return net_drive(waveform, TABLE_WW) + net_drive(ONE_WAY.bytes, TABLE_WB) == 0 &&
         net_drive(waveform, TABLE_BW) + net_drive(ONE_WAY.bytes, TABLE_BW) == 0 &&
         net_drive(waveform, TABLE_WB) == 0 && net_drive(waveform, TABLE_BB) == 0 && net_drive(waveform, 0) == 0;
}
static_assert(net_drive(ONE_WAY.bytes, TABLE_BW) == FAST_FRAMES && net_drive(ONE_WAY.bytes, TABLE_WB) == -FAST_FRAMES &&
                  net_drive(ONE_WAY.bytes, TABLE_WW) == 0 && net_drive(ONE_WAY.bytes, TABLE_BB) == 0,
              "a fast partial pushes only the pixels that change, one push each");
static_assert(cancels(CORRECTION.bytes), "cancelling must undo a fast partial's push exactly");
static_assert(cancels(SETTLE.bytes), "settling must undo a fast partial's push exactly");


// For each RAM bit a refresh needs, the levels (0 to 6) that set it, and from that a table from a
// buffer byte, two pixels with the first in the high nibble, to their two bits.
static constexpr uint8_t WHITE = 0b1110000;      // levels 4 to 6: a partial's reading, and level / 2's high bit
static constexpr uint8_t GREY_LOW = 0b1001100;   // levels 2, 3 and 6: level / 2's low bit
static constexpr uint8_t HALF_HIGH = 0b0101000;  // levels 3 and 5: the half step's high bit
static constexpr uint8_t HALF_LOW = 0b0100010;   // levels 1 and 5: the half step's low bit
struct PlaneBits {
  uint8_t table[256];
  constexpr PlaneBits(uint8_t levels) : table() {
    for (int v = 0; v < 256; v++)
      table[v] = (((levels >> (v >> 4)) & 1) << 1) | ((levels >> (v & 0x0F)) & 1);
  }
};
static constexpr PlaneBits WHITE_BITS{WHITE}, GREY_LOW_BITS{GREY_LOW}, HALF_HIGH_BITS{HALF_HIGH},
    HALF_LOW_BITS{HALF_LOW};
static constexpr CrowPanel579::Source WHITE_SOURCE{WHITE_BITS.table, nullptr};

// Where the last update left the full_update_every count, kept through deep sleep so the count
// carries on across wakes. Only read on a wake from deep sleep, so it is always one this
// driver wrote.
static RTC_DATA_ATTR uint8_t slept_update_count;
// A hash of each row's black and white reading as the glass shows it, so a partial refresh sends
// only the rows that changed, and nothing at all when none did. Kept through deep sleep, and only
// trusted when glass_rows_valid says the last update finished.
static RTC_DATA_ATTR uint32_t glass_rows[CrowPanel579::HEIGHT];
static RTC_DATA_ATTR bool glass_rows_valid;

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
  // What the glass shows and which pixels are out of balance, a bit a pixel each. Without them
  // there are no fast partials.
  RAMAllocator<uint8_t> allocator;
  this->glass_ = allocator.allocate(2 * BIT_PLANE);
  if (this->glass_ != nullptr) {
    this->unsettled_ = this->glass_ + BIT_PLANE;
    memset(this->unsettled_, 0, BIT_PLANE);
  }
  // Waking from deep sleep, the panel kept its power, its picture and its RAM, so the count
  // carries on and the first update can be partial. Anything else starts from nothing.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED)
    glass_rows_valid = false;
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED && this->is_using_partial_update_()) {
    this->update_count_ = slept_update_count % this->full_update_every_;
    ESP_LOGD(TAG, "Woke from deep sleep; the panel still holds its frame (update %u of %u)",
             (unsigned) this->update_count_, (unsigned) this->full_update_every_);
  }
}

// Before a deep sleep: finish writing the planes if a refresh has just run, cancel any fast
// partials' pushes, then sleep the controllers and hold the panel's power and reset pins high so
// it keeps its RAM.
void CrowPanel579::on_safe_shutdown() {
  this->cancel_timeout("idle");
  this->cancel_timeout("sleep");
  while (this->copying_ && !this->send_planes_(this->copy_source_(), this->copy_source_())) {
  }
  this->copying_ = false;
  using epaper_spi::EPaperState;
  const bool settled =
      !this->next_pass_ && (this->state_ == EPaperState::IDLE || this->state_ == EPaperState::DEEP_SLEEP);
  if (this->unsettled_count_ != 0) {
    if (settled) {
      this->settle_now_();
    } else {
      ESP_LOGW(TAG, "Sleeping part way through an update; %u pixels stay a push out of balance",
               (unsigned) this->unsettled_count_);
    }
  }
  // An update cut short, a seven-grey one between passes included, may leave the panel's RAM out
  // of step with the glass, so the next wake starts with a full refresh.
  slept_update_count = settled ? this->update_count_ : 0;
  glass_rows_valid = glass_rows_valid && settled;
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

// An update cancels the wind-down. One asked for while the driver is settling waits for it.
void CrowPanel579::update() {
  this->cancel_timeout("idle");
  this->cancel_timeout("sleep");
  if (this->state_ != epaper_spi::EPaperState::IDLE) {
    this->update_pending_ = true;
    return;
  }
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
  if (!this->is_idle_())
    return false;
  if (this->awake_)
    return true;
  if (!this->reset_sent_) {
    this->command(0x12);
    this->reset_sent_ = true;
    this->delay_until_ = millis() + 10;  // BUSY rises a moment after the command
    return false;
  }
  this->cmd_data(0x18, {0x80});
  this->reset_sent_ = false;
  this->awake_ = true;
  this->supplies_on_ = false;
  return true;
}

void CrowPanel579::set_ram_area_(bool left, uint16_t first_y, uint16_t end_y) {
  const uint8_t to = left ? LEFT : 0;
  // The right controller is mounted the other way round: its X counts down from the seam,
  // so both halves take a row's bytes left to right.
  const uint8_t first_x = left ? 0 : LAST_X;
  const uint8_t last_x = left ? LAST_X : 0;
  const uint16_t last_y = end_y - 1;
  this->cmd_data(0x11 | to, {static_cast<uint8_t>(left ? 0x03 : 0x02)});  // data entry mode
  this->cmd_data(0x44 | to, {first_x, last_x});
  this->cmd_data(0x45 | to, {static_cast<uint8_t>(first_y & 0xFF), static_cast<uint8_t>(first_y >> 8),
                             static_cast<uint8_t>(last_y & 0xFF), static_cast<uint8_t>(last_y >> 8)});
  this->cmd_data(0x4E | to, {first_x});
  this->cmd_data(0x4F | to, {static_cast<uint8_t>(first_y & 0xFF), static_cast<uint8_t>(first_y >> 8)});
}

// Plane bytes offset to offset + count of row y: from the buffer through a table, or from a bit
// plane as they are. Plane byte j holds pixels 8j to 8j + 7, the first in the top bit.
void HOT CrowPanel579::plane_bytes_(const Source &source, size_t y, size_t offset, size_t count, uint8_t *out) {
  if (source.plane != nullptr) {
    memcpy(out, source.plane + y * PLANE_ROW + offset, count);
    return;
  }
  // Plane byte offset + i holds the 8 pixels of buffer bytes 4 (offset + i) to the third after.
  const size_t start = y * this->row_width_ + 4 * offset;
  uint8_t copy[4 * PLANE_ROW];
  const uint8_t *in = this->pixels_ + start;
  if (this->pixels_ == nullptr) {
    for (size_t i = 0; i != 4 * count; i++)
      copy[i] = this->buffer_[start + i];
    in = copy;
  }
  const uint8_t *bits = source.table;
  for (size_t i = 0; i != count; i++, in += 4)
    out[i] = bits[in[0]] << 6 | bits[in[1]] << 4 | bits[in[2]] << 2 | bits[in[3]];
}

// Sends planes plane_ to 3 for rows first_row_ to end_row_, a 10 ms slice at a time: 0x26 then
// 0x24, each for the left half then the right. Rows go out ROWS_A_WRITE at a time, since each
// SPI write costs more than the bytes in it. Returns true once the last is out.
bool HOT CrowPanel579::send_planes_(const Source &old_source, const Source &new_source) {
  const uint32_t start = millis();
  uint8_t data[ROWS_A_WRITE * HALF];
  while (this->plane_ != 4) {
    const bool left = (this->plane_ & 1) == 0;
    const bool new_plane = this->plane_ >= 2;
    if (!this->sending_) {
      this->set_ram_area_(left, this->first_row_, this->end_row_);
      this->command((new_plane ? 0x24 : 0x26) | (left ? LEFT : 0));
      this->row_ = this->first_row_;
      this->sending_ = true;
    }
    const size_t offset = left ? 0 : RIGHT_OFFSET;
    const Source &source = new_plane ? new_source : old_source;
    this->start_data_();
    while (this->row_ != this->end_row_) {
      const size_t rows = std::min<size_t>(ROWS_A_WRITE, this->end_row_ - this->row_);
      for (size_t r = 0; r != rows; r++)
        this->plane_bytes_(source, this->row_ + r, offset, HALF, data + r * HALF);
      this->write_array(data, rows * HALF);
      this->row_ += rows;
      if (this->row_ != this->end_row_ && millis() - start > epaper_spi::MAX_TRANSFER_TIME) {
        this->disable();
        return false;
      }
    }
    this->disable();
    this->sending_ = false;
    this->plane_++;
  }
  this->plane_ = 0;
  return true;
}

// FNV-1a over a row's black and white reading.
static uint32_t row_hash(const uint8_t *row) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i != CrowPanel579::PLANE_ROW; i++)
    hash = (hash ^ row[i]) * 16777619u;
  return hash;
}

// The rows of the new frame whose black and white reading differs from the glass's, as one band.
// Returns false when there are none.
bool CrowPanel579::find_changed_rows_() {
  int first = -1, last = -1;
  uint8_t row[PLANE_ROW];
  for (size_t y = 0; y != HEIGHT; y++) {
    this->plane_bytes_(WHITE_SOURCE, y, 0, PLANE_ROW, row);
    if (!glass_rows_valid || row_hash(row) != glass_rows[y]) {
      if (first < 0)
        first = y;
      last = y;
    }
  }
  if (first < 0)
    return false;
  this->first_row_ = first;
  this->end_row_ = last + 1;
  return true;
}

// The band of rows holding a pixel out of balance.
void CrowPanel579::find_unsettled_rows_() {
  int first = -1, last = -1;
  for (size_t y = 0; y != HEIGHT; y++) {
    const uint8_t *row = this->unsettled_ + y * PLANE_ROW;
    for (size_t i = 0; i != PLANE_ROW; i++) {
      if (row[i] != 0) {
        if (first < 0)
          first = y;
        last = y;
        break;
      }
    }
  }
  this->first_row_ = first < 0 ? 0 : first;
  this->end_row_ = first < 0 ? 1 : last + 1;
}

// Decides what an update is, at its first transfer: a partial refresh when the count says so,
// fast when an active spell keeps the supplies on, or a full refresh, after cancelling any fast
// partials' pushes. A partial whose rows are all unchanged has nothing to send or refresh.
void CrowPanel579::plan_update_() {
  this->pass_count_ = 0;
  this->unchanged_ = false;
  if (this->update_count_ != 0) {
    if (!this->find_changed_rows_()) {
      this->unchanged_ = true;
      return;
    }
    const bool fast = this->supplies_on_ && this->glass_valid_;
    this->passes_[this->pass_count_++] = fast ? Pass::FAST : Pass::PARTIAL;
    ESP_LOGV(TAG, "%s refresh of rows %u to %u", fast ? "Fast partial" : "Partial", (unsigned) this->first_row_,
             (unsigned) this->end_row_ - 1);
  } else {
    if (this->unsettled_count_ != 0)
      this->passes_[this->pass_count_++] = Pass::CORRECT;
    if (!this->grey_) {
      this->passes_[this->pass_count_++] = Pass::BLACK_WHITE;
    } else if (!this->seven_) {
      this->passes_[this->pass_count_++] = Pass::GREY;
    } else {
      this->passes_[this->pass_count_++] = Pass::HALF_TO_BLACK;
      this->passes_[this->pass_count_++] = Pass::EVEN_GREY;
      this->passes_[this->pass_count_++] = Pass::HALF_TO_WHITE;
    }
  }
  glass_rows_valid = false;  // until this update has finished
}

// Sends the current pass's planes. A partial sends only 0x24, the new frame, and only the rows
// that changed: 0x26, and 0x24 everywhere else, already hold what is on the glass. Cancelling
// and settling send which pixels are out of balance (0x26) and what the glass shows (0x24) for the
// rows that have any. Every other pass sends both planes whole.
bool CrowPanel579::transfer_data() {
  if (!this->pass_started_) {
    if (this->pass_index_ == 0 && !this->settling_)
      this->plan_update_();
    if (this->unchanged_)
      return true;
    this->pass_started_ = true;
    const Pass pass = this->passes_[this->pass_index_];
    if (pass == Pass::CORRECT || pass == Pass::SETTLE) {
      this->find_unsettled_rows_();
    } else if (pass != Pass::FAST && pass != Pass::PARTIAL) {
      this->first_row_ = 0;
      this->end_row_ = HEIGHT;
    }
    this->plane_ = pass == Pass::FAST || pass == Pass::PARTIAL ? 2 : 0;
  }
  switch (this->passes_[this->pass_index_]) {
    case Pass::FAST:
    case Pass::PARTIAL:
    case Pass::BLACK_WHITE:
      return this->send_planes_(WHITE_SOURCE, WHITE_SOURCE);
    case Pass::GREY:
    case Pass::EVEN_GREY:
      return this->send_planes_(WHITE_SOURCE, Source{GREY_LOW_BITS.table, nullptr});
    case Pass::HALF_TO_BLACK:
    case Pass::HALF_TO_WHITE:
      return this->send_planes_(Source{HALF_HIGH_BITS.table, nullptr}, Source{HALF_LOW_BITS.table, nullptr});
    case Pass::CORRECT:
    case Pass::SETTLE:
      return this->send_planes_(Source{nullptr, this->unsettled_}, Source{nullptr, this->glass_});
  }
  return true;
}

// The loaded waveforms need 0x21's second byte at 0x10, which GxEPD2 calls the cascade bit;
// without it, changed pixels stop half way, at grey.
void CrowPanel579::refresh_screen(bool partial) {
  if (this->unchanged_) {
    ESP_LOGV(TAG, "Nothing changed; no refresh");
    return;
  }
  const Pass pass = this->passes_[this->pass_index_];
  if (pass == Pass::BLACK_WHITE) {
    ESP_LOGV(TAG, "Full refresh");
    this->cmd_data(0x3C, {BORDER_FULL});
    this->cmd_data(0x22, {UPDATE_FULL});
    this->command(0x20);
    this->supplies_on_ = false;  // the panel's own sequence switches them off
    return;
  }
  const bool grey = pass == Pass::GREY || pass == Pass::EVEN_GREY;
  this->cmd_data(0x3C, {grey ? BORDER_GREY : BORDER_PARTIAL});
  this->cmd_data(0x21, {0x00, 0x10});
  switch (pass) {
    case Pass::FAST:
      this->load_waveform_(ONE_WAY.bytes);
      break;
    case Pass::PARTIAL:
      this->load_waveform_(PARTIAL.bytes);
      break;
    case Pass::GREY:
      this->load_waveform_(GREY_LUT);
      break;
    case Pass::HALF_TO_BLACK:
      this->load_waveform_(HALF_TO_BLACK.bytes);
      break;
    case Pass::EVEN_GREY:
      this->load_waveform_(EVEN_GREY.bytes);
      break;
    case Pass::HALF_TO_WHITE:
      this->load_waveform_(HALF_TO_WHITE.bytes);
      break;
    case Pass::CORRECT:
      this->load_waveform_(CORRECTION.bytes);
      break;
    case Pass::SETTLE:
      this->load_waveform_(SETTLE.bytes);
      break;
    case Pass::BLACK_WHITE:
      break;
  }
  this->command(0x20);
}

// Loads a waveform and runs it without the OTP's temperature lookup and waveform, leaving the
// supplies on afterwards for whatever comes next.
void CrowPanel579::load_waveform_(const uint8_t *waveform) {
  this->cmd_data(0x0C, {0x8B, 0x9C, 0xA4, 0x0F});  // booster soft start, as Waveshare sets it
  this->cmd_data(0x32, waveform, 227);
  this->cmd_data(0x3F, {waveform[227]});
  this->cmd_data(0x03, {waveform[228]});
  this->cmd_data(0x04, {waveform[229], waveform[230], waveform[231]});
  this->cmd_data(0x2C, {waveform[232]});
  this->cmd_data(0x22, {this->supplies_on_ ? UPDATE_DISPLAY : UPDATE_SUPPLIES_DISPLAY});
  this->supplies_on_ = true;
}

// Runs once a pass has finished. With passes left, loop() starts the next. After the last, the
// glass's reading is brought up to date, fast partials' pushes are recorded or cleared, and the
// reading goes to both planes for the next partial to compare with; the base moves straight on to
// sleeping the controllers, so loop() writes them first, in slices. Both planes, as GxEPD2's
// writeImageAgain does, because a refresh with a loaded waveform swaps the banks 0x24 and 0x26
// address until the next reset (RAM ping-pong, 0x37): with both holding the same frame, the next
// partial is right with or without a reset in between. The panel's own black and white refresh
// writes both planes before it and does not swap.
void CrowPanel579::power_off() {
  this->pass_started_ = false;
  if (this->pass_index_ + 1 < this->pass_count_) {
    this->pass_index_++;
    this->next_pass_ = true;
    return;
  }
  const Pass last = this->passes_[this->pass_index_];
  this->pass_index_ = 0;
  if (this->settling_) {
    this->settling_ = false;
    this->update_count_ = this->settle_saved_count_;
    memset(this->unsettled_, 0, BIT_PLANE);
    this->unsettled_count_ = 0;
    this->copying_ = true;
    return;
  }
  if (this->unchanged_)
    return;
  this->record_glass_(last == Pass::FAST, this->passes_[0] == Pass::CORRECT);
  this->copying_ = last != Pass::BLACK_WHITE;
}

// The new frame's black and white reading becomes the glass's, row by row: its hash for the next
// partial, and with the bit planes, which pixels a fast partial has pushed out of balance (each
// change flips its pixel) or that cancelling has put right.
void CrowPanel579::record_glass_(bool fast, bool corrected) {
  uint8_t row[PLANE_ROW];
  for (size_t y = 0; y != HEIGHT; y++) {
    this->plane_bytes_(WHITE_SOURCE, y, 0, PLANE_ROW, row);
    glass_rows[y] = row_hash(row);
    if (this->glass_ == nullptr)
      continue;
    uint8_t *glass = this->glass_ + y * PLANE_ROW;
    uint8_t *unsettled = this->unsettled_ + y * PLANE_ROW;
    for (size_t i = 0; i != PLANE_ROW; i++) {
      if (corrected)
        unsettled[i] = 0;
      if (fast)
        unsettled[i] ^= glass[i] ^ row[i];
      glass[i] = row[i];
    }
  }
  glass_rows_valid = true;
  this->glass_valid_ = this->glass_ != nullptr;
  this->unsettled_count_ = 0;
  if (this->glass_ != nullptr) {
    for (size_t i = 0; i != BIT_PLANE; i++)
      this->unsettled_count_ += __builtin_popcount(this->unsettled_[i]);
  }
  if (this->unsettled_count_ != 0)
    ESP_LOGV(TAG, "%u pixels a push out of balance", (unsigned) this->unsettled_count_);
}

// What the planes get after a refresh: the frame just drawn, or for settling, the glass as it was.
CrowPanel579::Source CrowPanel579::copy_source_() const {
  return this->settling_copy_ ? Source{nullptr, this->glass_} : WHITE_SOURCE;
}

// The base runs one step of the update a loop() call, and ESPHome calls loop() about every 16 ms,
// so the steps run back to back here instead, until one has to wait (BUSY, a delay) or the
// budget is spent.
void CrowPanel579::loop() {
  const uint32_t start = millis();
  while (millis() - start < STEP_BUDGET_MS) {
    if (this->copying_) {
      if (!this->send_planes_(this->copy_source_(), this->copy_source_()))
        continue;
      this->copying_ = false;
      this->settling_copy_ = false;
    }
    // Each pass is a full update to the base, and the count goes back so they count as one.
    if (this->next_pass_ && this->state_ == epaper_spi::EPaperState::DEEP_SLEEP) {
      this->next_pass_ = false;
      this->update_count_ = 0;
      this->set_state_(epaper_spi::EPaperState::RESET);
    }
    const auto state = this->state_;
    const auto row = this->row_;
    const auto plane = this->plane_;
    EPaperBase::loop();
    if (this->state_ == state && this->row_ == row && this->plane_ == plane)
      return;  // waiting
  }
}

// The base sleeps the controllers after every update. Here an active spell carries on instead,
// the supplies on and the controllers awake, until ACTIVE_MS passes without another update.
void CrowPanel579::deep_sleep() {
  if (this->update_pending_) {
    this->update_pending_ = false;
    this->defer([this]() { this->update(); });
    return;
  }
  this->set_timeout("idle", ACTIVE_MS, [this]() { this->wind_down_(); });
}

// The end of an active spell: cancel any fast partials' pushes with a settling pass, which runs as
// an update of its own, then switch the supplies off and sleep the controllers a while later.
void CrowPanel579::wind_down_() {
  if (this->state_ != epaper_spi::EPaperState::IDLE)
    return;
  if (this->unsettled_count_ != 0) {
    ESP_LOGV(TAG, "Settling %u pixels", (unsigned) this->unsettled_count_);
    this->settling_ = true;
    this->settling_copy_ = true;
    this->settle_saved_count_ = this->update_count_;
    this->passes_[0] = Pass::SETTLE;
    this->pass_count_ = 1;
    this->pass_index_ = 0;
    this->unchanged_ = false;
    this->update_start_time_ = millis();
    this->set_state_(epaper_spi::EPaperState::RESET);
    this->enable_loop();
    return;
  }
  this->switch_supplies_off_();
  this->set_timeout("sleep", CONTROLLER_IDLE_MS, [this]() { this->sleep_controllers_(); });
}

// Settling straight away, waiting on BUSY, for a deep sleep that cannot wait for an update.
void CrowPanel579::settle_now_() {
  this->find_unsettled_rows_();
  this->plane_ = 0;
  while (!this->send_planes_(Source{nullptr, this->unsettled_}, Source{nullptr, this->glass_})) {
  }
  this->cmd_data(0x3C, {BORDER_PARTIAL});
  this->cmd_data(0x21, {0x00, 0x10});
  this->load_waveform_(SETTLE.bytes);
  this->command(0x20);
  this->wait_idle_(3000);
  while (!this->send_planes_(Source{nullptr, this->glass_}, Source{nullptr, this->glass_})) {
  }
  memset(this->unsettled_, 0, BIT_PLANE);
  this->unsettled_count_ = 0;
}

void CrowPanel579::wait_idle_(uint32_t timeout_ms) {
  const uint32_t start = millis();
  delay(2);  // BUSY rises a moment after the command
  while (!this->is_idle_() && millis() - start < timeout_ms)
    delay(1);
}

// Switches the ±15 V supplies off, without waiting: the next command waits for BUSY anyway.
void CrowPanel579::switch_supplies_off_() {
  if (!this->supplies_on_)
    return;
  this->cmd_data(0x22, {SUPPLIES_OFF});
  this->command(0x20);
  this->supplies_on_ = false;
}

// Deep sleep mode 1 keeps RAM. The next update wakes the controllers with a hardware reset.
void CrowPanel579::sleep_controllers_() {
  this->cancel_timeout("idle");
  this->cancel_timeout("sleep");
  if (this->supplies_on_) {
    this->switch_supplies_off_();
    this->wait_idle_(500);
  }
  this->cmd_data(0x10, {0x01});
  this->awake_ = false;
}

}  // namespace esphome::crowpanel_579
