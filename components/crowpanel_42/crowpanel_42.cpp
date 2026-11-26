#include "crowpanel_42.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_sleep.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::crowpanel_42 {

static const char *const TAG = "crowpanel_42";

// How long one loop() call may run the update's steps back to back, well inside ESPHome's 50 ms.
static constexpr uint32_t STEP_BUDGET_MS = 25;
// The supplies stay on until ACTIVE_MS passes without an update, or for at most MAX_ACTIVE_MS at a
// stretch however often updates come. Switching them on takes about 80 ms and off about 40 ms.
static constexpr uint32_t ACTIVE_MS = 5000;
static constexpr uint32_t MAX_ACTIVE_MS = 60000;
// An update the panel has not finished in this long has failed.
static constexpr uint32_t UPDATE_DEADLINE_MS = 20000;

// Frame rates (0x30): 5 Hz steps from 5 Hz at 0x00 to 120 Hz at 0x17, then 10 Hz steps to 200 Hz.
static constexpr uint8_t FRAMES_50_HZ = 0x09;
static constexpr float MAX_HZ = 200;
// This panel's waveforms were tuned at 11 °C: the self-balanced ones at 50 Hz, the seven-grey
// half steps at 150 Hz.
static constexpr float TUNED_CELSIUS = 11, TUNED_HZ = 50, TUNED_HALF_HZ = 150;

// A waveform is five tables of six groups of seven bytes, in the SSD1683's order: VCOM, then one
// for each pixel state, named by the pixel's NEW bit then its OLD bit, 1 white: WW, BW (white to
// black), WB (black to white), BB. A group is a repeat count, four phases (the level in the top two
// bits, frames in the rest: 0 V, VSH 0x40, VSL 0x80) and two state repeats. VSH (+15 V) drives a
// pixel black and VSL (-15 V) white. Every waveform below spends as long at one as at the other,
// so no refresh leaves a net drive on any pixel.
static constexpr size_t TABLE = 42, WAVEFORM = CrowPanel42::WAVEFORM;
static_assert(WAVEFORM == 5 * TABLE);
static constexpr uint8_t TABLE_WW = 1, TABLE_BW = 2, TABLE_WB = 3, TABLE_BB = 4;

// How many frames table t of a waveform runs.
static constexpr int frames(const uint8_t *waveform, int t) {
  int total = 0;
  for (int g = 0; g < 6; g++) {
    const uint8_t *group = waveform + t * TABLE + g * 7;
    const int ab = (group[1] & 0x3F) + (group[2] & 0x3F), cd = (group[3] & 0x3F) + (group[4] & 0x3F);
    total += group[0] * (ab * group[5] + cd * group[6]);
  }
  return total;
}

// Waveshare's four-grey waveform for their 4.2in V2 panel, as the 5.79" driver runs it: group 1
// once rather than five times. The grey waveforms here keep its VCOM, white and black tables and
// retune its dark and light ones.
static constexpr uint8_t GREY_LUT[WAVEFORM] = {
  // VCOM, then a table per pixel state; 6 groups each of RP, phases A to D, SR_AB, SR_CD
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
};

// The cold partial refresh's waveform, for the first update after a boot or a wake, before the
// driver knows what the glass shows: a pixel turning black is pushed towards white for 25 frames,
// then towards black for 25, and one turning white the other way round. Pixels that keep their
// colour get no drive at all, so greys survive.
struct PartialWaveform {
  uint8_t bytes[WAVEFORM];
  constexpr PartialWaveform() : bytes() {
    constexpr uint8_t FRAMES = 25, NONE = FRAMES, BLACK = 0x40 | FRAMES, WHITE = 0x80 | FRAMES;
    constexpr uint8_t phases[5][2] = {{NONE, NONE}, {NONE, NONE}, {WHITE, BLACK}, {BLACK, WHITE}, {NONE, NONE}};
    for (int t = 0; t < 5; t++) {
      bytes[t * TABLE] = 1;
      bytes[t * TABLE + 1] = phases[t][0];
      bytes[t * TABLE + 2] = phases[t][1];
      bytes[t * TABLE + 5] = 1;
      bytes[t * TABLE + 6] = 1;
    }
  }
};
static constexpr PartialWaveform PARTIAL{};

// A grey waveform: Waveshare's, with groups 0 to 3 of its dark (WB) and light (BW) tables given.
// VCOM's first phase grows until its table runs exactly as long as the longest of the others: the
// refresh lasts as long as that, and a frame of VCOM at its null beyond it would change nothing.
struct GreyWaveform {
  uint8_t bytes[WAVEFORM];
  constexpr GreyWaveform(const uint8_t (&dark)[4][4], const uint8_t (&light)[4][4]) : bytes() {
    for (size_t i = 0; i < WAVEFORM; i++)
      bytes[i] = GREY_LUT[i];
    for (int g = 0; g < 4; g++) {
      for (int p = 0; p < 4; p++) {
        bytes[TABLE_WB * TABLE + g * 7 + 1 + p] = dark[g][p];
        bytes[TABLE_BW * TABLE + g * 7 + 1 + p] = light[g][p];
      }
    }
    int longest = 0;
    for (int t = 1; t < 5; t++)
      longest = frames(this->bytes, t) > longest ? frames(this->bytes, t) : longest;
    const int extra = longest - frames(this->bytes, 0);
    if (extra > 0)
      bytes[1] = static_cast<uint8_t>(bytes[1] + extra);
  }
};
// Tuned on this panel with VCOM at its null, against flat white and black frames. Where a grey
// lands hangs on the table's last few frames: 9 of black against 3 of white in each of group 2's
// five repeats leaves dark grey at 0.22, one more of black at the very end 0.12. So the tables end
// on a short pair of their own instead, which also tunes them finer than a frame in each repeat:
// for dark grey at a third (0.34 as four greys), 1 frame of black and 2 of white; at a sixth (0.18
// as seven), 1 and 1. Light grey, two thirds (0.71 as four, 0.66 as seven): 7 of black against 4 of
// white in each repeat, then 1 more of white. Each is balanced by its white in group 0.
static constexpr uint8_t DARK_THIRD[4][4] = {
    {0x8F, 0x1B, 0x8E, 0x03}, {0x4A, 0x01, 0x8A, 0x01}, {0x49, 0x83, 0x02, 0x04}, {0x41, 0x82, 0x02, 0x00}};
static constexpr uint8_t DARK_SIXTH[4][4] = {
    {0x8F, 0x1B, 0x8F, 0x03}, {0x4A, 0x01, 0x8A, 0x01}, {0x49, 0x83, 0x02, 0x04}, {0x41, 0x81, 0x02, 0x00}};
static constexpr uint8_t LIGHT[4][4] = {
    {0x0A, 0x1B, 0x8E, 0x03}, {0x4A, 0x01, 0x8A, 0x01}, {0x47, 0x83, 0x81, 0x04}, {0x81, 0x04, 0x02, 0x00}};
// Four greys in even steps, as anti-aliased text blends them.
static constexpr GreyWaveform FOUR_GREY{DARK_THIRD, LIGHT};
// The seven-grey refresh's middle pass: black, a sixth, two thirds and white.
static constexpr GreyWaveform EVEN_GREY{DARK_SIXTH, LIGHT};

// The seven-grey refresh's outer passes: a half step, towards black before the middle pass and
// towards white after it, so the two cancel. The pixel's two RAM bits say which: level 2 (and
// white, which a push and a pull leave white) from dark grey (WB), level 3 from dark grey (BW),
// level 5 from light grey (WW), or for levels 0, 1 and 4, a one-frame trim (BB), towards white
// before and towards black after. They run at 150 Hz, where after the middle pass at 50 Hz 3, 5 and 4
// frames land levels 2, 3 and 5 at 0.34, 0.49 and 0.80 on the camera. They speed up with the middle
// pass; past the controller's 200 Hz, they run at 200 Hz with their frames scaled to match.
struct HalfStepWaveform {
  uint8_t bytes[WAVEFORM];
  constexpr HalfStepWaveform(bool white, uint8_t level_2, uint8_t level_3, uint8_t level_5) : bytes() {
    constexpr uint8_t TRIM = 1;
    // Phase A is level 2's length; B and C add what levels 3 and 5 need beyond it.
    const uint8_t shorter = level_3 < level_5 ? level_3 : level_5, longer = level_3 < level_5 ? level_5 : level_3;
    const uint8_t lengths[3] = {level_2, static_cast<uint8_t>(shorter - level_2),
                                static_cast<uint8_t>(longer - shorter)};
    const uint8_t phases_3 = level_3 == longer ? 0b111 : 0b011, phases_5 = level_5 == longer ? 0b111 : 0b011;
    const uint8_t phases_of_table[5] = {0, phases_5, phases_3, 0b001, 0};  // VCOM, WW, BW, WB, BB
    const uint8_t drive = white ? 0x80 : 0x40;
    for (int t = 0; t < 5; t++) {
      bytes[t * TABLE] = 1;
      for (int p = 0; p < 3; p++)
        bytes[t * TABLE + 1 + p] = (((phases_of_table[t] >> p) & 1) ? drive : 0) | lengths[p];
      bytes[t * TABLE + 4] = TRIM;  // phase D: the trim, driven only in BB
      bytes[t * TABLE + 5] = 1;
      bytes[t * TABLE + 6] = 1;
    }
    bytes[TABLE_BB * TABLE + 4] = (white ? 0x40 : 0x80) | TRIM;  // BB: towards white before, black after
  }
};
static constexpr uint8_t HALF_LENGTHS[3] = {3, 5, 4};  // levels 2, 3 and 5
static constexpr HalfStepWaveform HALF_TO_BLACK{false, HALF_LENGTHS[0], HALF_LENGTHS[1], HALF_LENGTHS[2]};
static constexpr HalfStepWaveform HALF_TO_WHITE{true, HALF_LENGTHS[0], HALF_LENGTHS[1], HALF_LENGTHS[2]};

// Waveforms of group 0 only, with phases A to C of the given lengths and, for each table, the level
// in each phase (0 none, 1 VSH, black, 2 VSL, white).
struct SimpleWaveform {
  uint8_t bytes[WAVEFORM];
  constexpr SimpleWaveform(const uint8_t (&lengths)[3], const uint8_t (&levels)[5][3]) : bytes() {
    for (int t = 0; t < 5; t++) {
      bytes[t * TABLE] = 1;
      for (int p = 0; p < 3; p++)
        bytes[t * TABLE + 1 + p] = levels[t][p] << 6 | lengths[p];
      bytes[t * TABLE + 5] = 1;
      bytes[t * TABLE + 6] = 1;
    }
  }
};
// The full black and white refresh, Elecrow's: every pixel pushed the far way for 20 frames, rested
// for 10, then pushed to its colour for 20. About 1 s at 50 Hz.
static constexpr SimpleWaveform BLACK_WHITE{{20, 10, 20}, {{0, 0, 0}, {1, 0, 2}, {2, 0, 1}, {1, 0, 2}, {2, 0, 1}}};
static constexpr uint8_t FAST_FRAMES = 12;
// A fast partial pushes a changing pixel one way only: 12 frames towards its new colour, against
// the balanced partial's 50. At 11 °C, 6, 9, 12 and 15 frames take white to 0.09, 0.02, 0.00 and
// -0.02 on the camera, and black to 0.86, 0.93, 0.96 and 0.98. Each push leaves its pixel out of
// balance, which the driver records and later cancels.
static constexpr SimpleWaveform ONE_WAY{{FAST_FRAMES, 0, 0}, {{0}, {0}, {1}, {2}, {0}}};
// Cancelling, with OLD saying which pixels are out of balance and NEW what the glass shows: a push
// back the other way. It runs just before a full refresh, whose flash hides it.
static constexpr SimpleWaveform CORRECTION{{FAST_FRAMES, 0, 0}, {{0}, {1}, {2}, {0}, {0}}};
// The same cancelling when no full refresh follows: a push back long enough to take the pixel all
// the way over, then 25 frames of its own colour, so it ends where it was, having blinked.
static constexpr uint8_t SETTLE_RETURN = 25;
static constexpr SimpleWaveform SETTLE{
    {FAST_FRAMES + SETTLE_RETURN, SETTLE_RETURN, 0}, {{0}, {1, 2}, {2, 1}, {0}, {0}}};

// The net drive a waveform leaves on pixels using table t, in frames: VSH counts one way and VSL
// the other, with the group and state repeats. Returns a value far from 0 for anything this driver
// should never send: VDHR, or a VCOM table that leaves its DC level.
static constexpr int net_drive(const uint8_t *waveform, int t) {
  constexpr int INVALID = 1 << 20;
  int net = 0;
  for (int g = 0; g < 6; g++) {
    const uint8_t *group = waveform + t * TABLE + g * 7;
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
// Whether VCOM's table runs at least as long as every pixel's. The controller runs each table on
// its own timing, the longest setting the refresh, and fills a shorter one's end with 0 V; VCOM
// should hold its DC level to the end. Each table's net drive counts the same whatever its timing.
static constexpr bool vcom_lasts(const uint8_t *waveform) {
  for (int t = 1; t < 5; t++) {
    if (frames(waveform, t) > frames(waveform, 0))
      return false;
  }
  return true;
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
static_assert(vcom_lasts(PARTIAL.bytes) && vcom_lasts(FOUR_GREY.bytes) && vcom_lasts(EVEN_GREY.bytes) &&
                  vcom_lasts(BLACK_WHITE.bytes) && vcom_lasts(HALF_TO_BLACK.bytes) &&
                  vcom_lasts(HALF_TO_WHITE.bytes),
              "VCOM's table must run as long as every pixel's");
static_assert(balanced({PARTIAL.bytes}), "the partial waveform must leave no net drive on a pixel");
static_assert(balanced({BLACK_WHITE.bytes}), "the black and white waveform must leave no net drive on a pixel");
static_assert(balanced({FOUR_GREY.bytes}), "the grey waveform must leave no net drive on a pixel");
static_assert(balanced({EVEN_GREY.bytes}), "the even grey waveform must leave no net drive on a pixel");
static_assert(balanced({HALF_TO_BLACK.bytes, HALF_TO_WHITE.bytes}),
              "the half steps must cancel");

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
static_assert(vcom_lasts(ONE_WAY.bytes) && vcom_lasts(CORRECTION.bytes) && vcom_lasts(SETTLE.bytes),
              "VCOM's table must run as long as every pixel's");

// For each plane bit a refresh needs, the levels (0 to 6) that set it, and from that a table from
// a buffer byte, two pixels with the first in the high nibble, to their two bits.
static constexpr uint8_t WHITE = 0b1110000;      // levels 4 to 6: a partial's reading, and the grey's high bit
static constexpr uint8_t GREY_LOW = 0b1001110;   // levels 1, 2, 3 and 6: dark grey or white
static constexpr uint8_t HALF_HIGH = 0b0101000;  // levels 3 and 5: the half step's high bit
static constexpr uint8_t HALF_LOW = 0b1100100;   // levels 2, 5 and 6: the half step's low bit
struct PlaneBits {
  uint8_t table[256];
  constexpr PlaneBits(uint8_t levels) : table() {
    for (int v = 0; v < 256; v++)
      table[v] = (((levels >> (v >> 4)) & 1) << 1) | ((levels >> (v & 0x0F)) & 1);
  }
};
static constexpr PlaneBits WHITE_BITS{WHITE}, GREY_LOW_BITS{GREY_LOW}, HALF_HIGH_BITS{HALF_HIGH},
    HALF_LOW_BITS{HALF_LOW};
static constexpr CrowPanel42::Source WHITE_SOURCE{WHITE_BITS.table, nullptr};

// Where the last update left the full_update_every count, kept through deep sleep so the count
// carries on across wakes.
static RTC_DATA_ATTR uint8_t slept_update_count;
// A hash of each row's black and white reading as the glass shows it, so an update in which no row
// changed sends and refreshes nothing. Kept through deep sleep, and only trusted when
// glass_rows_valid says the last update finished.
static RTC_DATA_ATTR uint32_t glass_rows[CrowPanel42::HEIGHT];
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

void CrowPanel42::setup() {
  // The pins are already high if a deep sleep held them; set them up before letting go.
  EPaperBase::setup();
  hold(this->power_pin_, false);
  hold(this->hold_reset_pin_, false);
  if (this->is_failed())
    return;
  size_t length = 0;
  uint8_t *pixels = this->buffer_.get_span(0, length);
  this->pixels_ = length == this->buffer_length_ ? pixels : nullptr;
  // What the glass shows and which pixels are out of balance, a bit a pixel each. Without them
  // there are no fast partials.
  this->add_reader_();
  RAMAllocator<uint8_t> allocator;
  this->glass_ = allocator.allocate(2 * BIT_PLANE);
  if (this->glass_ != nullptr) {
    this->unsettled_ = this->glass_ + BIT_PLANE;
    memset(this->unsettled_, 0, BIT_PLANE);
  }
  // Grey partials compare each frame with the last one drawn, level for level.
  if (this->grey_partials_ && this->pixels_ != nullptr)
    this->shown_ = allocator.allocate(this->buffer_length_);
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    glass_rows_valid = false;
    // A reboot part way through, a crash or an update over the air, can leave the controller with
    // its supplies on. A hardware reset switches them off.
    this->reset_controller_();
  } else if (this->is_using_partial_update_()) {
    // The controller kept its RAM through the sleep, so the count carries on and the first update
    // can be partial.
    this->update_count_ = slept_update_count % this->full_update_every_;
    ESP_LOGD(TAG, "Woke from deep sleep; the panel still holds its frame (update %u of %u)",
             (unsigned) this->update_count_, (unsigned) this->full_update_every_);
  }
}

// Before a deep sleep: finish writing the planes if a refresh has just run, cancel any fast
// partials' pushes, switch the supplies off and hold the panel's power and reset pins high so the
// controller keeps its RAM, in standby.
void CrowPanel42::on_safe_shutdown() {
  this->cancel_timeout("idle");
  while (this->copying_ && !this->send_planes_(WHITE_SOURCE, WHITE_SOURCE)) {
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
  // An update cut short may leave the panel's RAM out of step with the glass, so the next wake
  // starts with a full refresh.
  slept_update_count = settled ? this->update_count_ : 0;
  glass_rows_valid = glass_rows_valid && settled;
  if (this->supplies_on_) {
    this->wait_idle_(3000);
    this->switch_supplies_off_();
    this->wait_idle_(500);
  }
  hold(this->power_pin_, true);
  hold(this->hold_reset_pin_, true);
  gpio_deep_sleep_hold_en();
}

// Four greys use the even levels, in even steps of luma so anti-aliased text, which blends at
// 0, 85, 170 and 255, keeps all four. Seven use every level.
uint8_t CrowPanel42::level_(Color color) const {
  const uint8_t luma = epaper_spi::rec601_luma(color);
  return this->seven_ ? luma * 7 / 256 : (luma >> 6) * 2;
}

void CrowPanel42::fill(Color color) {
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

void HOT CrowPanel42::draw_pixel_at(int x, int y, Color color) {
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

// An update cancels the wind-down.
void CrowPanel42::update() {
  this->cancel_timeout("idle");
  EPaperBase::update();
}

// A controller already set up needs no reset or initialisation: every refresh loads its own
// waveform and frame rate, and every plane it reads is written for it.
bool CrowPanel42::reset() {
  if (this->ready_)
    return true;
  if (!EPaperBase::reset())
    return false;  // reset is held low; the base calls back after reset_duration_
  this->next_delay_ = 10;  // give the controller time to come out of reset before BUSY is read
  return true;
}

bool CrowPanel42::initialise(bool partial) {
  if (!this->is_idle_())
    return false;
  if (!this->ready_) {
    this->send_init_();
    this->ready_ = true;
    this->supplies_on_ = false;
  }
  return true;
}

// Waveforms from the registers in black and white mode, with the temperature sensed before every
// power on; ±15 V for the pixels, as every waveform here assumes, and 20 V for the gates; Elecrow's
// booster soft start, which brings the supplies up in 80 ms rather than 310; VCOM at -1.4 V; and
// the border left floating, so no refresh drives it.
//
// VCOM is where a frame driving 0 V leaves a pixel as it is. Over a grey frame, 519 such frames moved
// black and dark grey by +0.02 and +0.01 at -0.9 V, 0.00 to +0.01 at -1.1 and -1.3 V, and 0.00 and
// -0.01 at -1.5 V; Elecrow's -0.6 V lightened them by 0.03 and 0.05 in only 173. Every waveform rests
// at 0 V for some frames, and every pixel outside a partial window does for the whole refresh, so a
// VCOM off its null both drifts the greys and leaves a DC bias on balanced waveforms.
void CrowPanel42::send_init_() {
  this->cmd_data(0x00, {0x3F, 0x0D});
  this->cmd_data(0x01, {0x03, 0x10, 0x3F, 0x3F, 0x03});
  this->cmd_data(0x06, {0x96, 0x96, 0x29});
  this->cmd_data(0x61, {0x01, 0x90, 0x01, 0x2C});
  this->cmd_data(0x82, {0x0D});
  this->cmd_data(0x50, {0xD7});
  this->cmd_data(0x60, {0x22});
  this->cmd_data(0xE3, {0x88});
}

// Row y's plane bytes: from the buffer through a table, or from a bit plane as they are. Plane
// byte j holds pixels 8j to 8j + 7, the first in the top bit.
void HOT CrowPanel42::plane_bytes_(const Source &source, size_t y, uint8_t *out) {
  if (source.plane != nullptr) {
    memcpy(out, source.plane + y * PLANE_ROW, PLANE_ROW);
    return;
  }
  const size_t start = y * this->row_width_;
  uint8_t copy[WIDTH / 2];
  const uint8_t *in = this->pixels_ + start;
  if (this->pixels_ == nullptr) {
    for (size_t i = 0; i != WIDTH / 2; i++)
      copy[i] = this->buffer_[start + i];
    in = copy;
  }
  const uint8_t *bits = source.table;
  for (size_t i = 0; i != PLANE_ROW; i++, in += 4)
    out[i] = bits[in[0]] << 6 | bits[in[1]] << 4 | bits[in[2]] << 2 | bits[in[3]];
}

// Sends planes plane_ to 1, OLD then NEW, a 10 ms slice at a time, ROWS_A_WRITE rows to a write.
// Returns true once the last is out.
bool HOT CrowPanel42::send_planes_(const Source &old_source, const Source &new_source) {
  const uint32_t start = millis();
  uint8_t data[ROWS_A_WRITE * PLANE_ROW];
  while (this->plane_ != 2) {
    const bool new_plane = this->plane_ == 1;
    if (!this->sending_) {
      this->command(new_plane ? 0x13 : 0x10);
      this->row_ = 0;
      this->sending_ = true;
    }
    const Source &source = new_plane ? new_source : old_source;
    this->start_data_();
    while (this->row_ != HEIGHT) {
      const size_t rows = std::min<size_t>(ROWS_A_WRITE, HEIGHT - this->row_);
      for (size_t r = 0; r != rows; r++)
        this->plane_bytes_(source, this->row_ + r, data + r * PLANE_ROW);
      this->write_array(data, rows * PLANE_ROW);
      this->row_ += rows;
      if (this->row_ != HEIGHT && millis() - start > epaper_spi::MAX_TRANSFER_TIME) {
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
  for (size_t i = 0; i != CrowPanel42::PLANE_ROW; i++)
    hash = (hash ^ row[i]) * 16777619u;
  return hash;
}

// Whether any row of the new frame reads differently, black and white, from the glass.
bool CrowPanel42::find_changed_rows_() {
  if (!glass_rows_valid)
    return true;
  uint8_t row[PLANE_ROW];
  for (size_t y = 0; y != HEIGHT; y++) {
    this->plane_bytes_(WHITE_SOURCE, y, row);
    if (row_hash(row) != glass_rows[y])
      return true;
  }
  return false;
}

// The 5.79" panel's maker's curve: how long its full refresh ran at each temperature (°C, s), read
// from its OTP with the temperature faked. Linear between the points, flat beyond them.
static constexpr float CURVE[][2] = {{10, 4.5f}, {16.5f, 3.28f}, {26, 2.27f}, {35, 1.66f}};
static constexpr int CURVE_POINTS = sizeof CURVE / sizeof CURVE[0];

static constexpr float refresh_seconds(float celsius) {
  if (celsius <= CURVE[0][0])
    return CURVE[0][1];
  for (int i = 1; i < CURVE_POINTS; i++) {
    if (celsius <= CURVE[i][0]) {
      const float along = (celsius - CURVE[i - 1][0]) / (CURVE[i][0] - CURVE[i - 1][0]);
      return CURVE[i - 1][1] + along * (CURVE[i][1] - CURVE[i - 1][1]);
    }
  }
  return CURVE[CURVE_POINTS - 1][1];
}

// The controller's nearest frame rate to hz, as its 0x30 code, and back.
static uint8_t frame_rate_code(float hz) {
  if (hz <= 122.5f)
    return std::clamp<long>(std::lround(hz / 5) - 1, 0, 0x17);
  return std::min<long>(0x17 + std::lround((hz - 120) / 10), 0x1F);
}
static float frame_rate_hz(uint8_t code) { return code <= 0x17 ? 5.0f * (code + 1) : 120 + 10.0f * (code - 0x17); }

// Particles move faster in the warm and slower in the cold, so the self-balanced waveforms run at
// a frame rate for the temperature, which stretches or shrinks every frame alike and leaves their
// balance as it is. This panel's maker publishes no curve and its OTP is blank, so the rate
// follows the 5.79" panel's maker's, scaled to 50 Hz at 11 °C where these were tuned: about 65 Hz at
// 16 °C, 75 at 20, 95 at 26 and 130 from 35. The controller's 5 Hz steps keep it smooth rather than
// banded. The temperature is the one given, or else the panel's own; without either, 50 Hz.
void CrowPanel42::set_frame_rates_() {
  float celsius = this->panel_celsius_;
#ifdef USE_SENSOR
  if (this->temperature_ != nullptr && !std::isnan(this->temperature_->state))
    celsius = this->temperature_->state;
#endif
  const float hz = std::isnan(celsius) ? TUNED_HZ
                                       : TUNED_HZ * refresh_seconds(TUNED_CELSIUS) / refresh_seconds(celsius);
  this->frame_rate_ = frame_rate_code(hz);
  // The half steps keep their length against the middle pass's frames: at its rate, faster, or
  // past 200 Hz at 200 Hz with fewer frames. Both are built from the same lengths, so they cancel.
  const float half_hz = TUNED_HALF_HZ * frame_rate_hz(this->frame_rate_) / TUNED_HZ;
  const float scale = half_hz > MAX_HZ ? MAX_HZ / half_hz : 1;
  this->half_rate_ = frame_rate_code(half_hz * scale);
  uint8_t lengths[3];
  for (int i = 0; i < 3; i++)
    lengths[i] = std::max<long>(1, std::lround(HALF_LENGTHS[i] * scale));
  lengths[0] = std::min({lengths[0], lengths[1], lengths[2]});
  memcpy(this->half_to_black_, HalfStepWaveform(false, lengths[0], lengths[1], lengths[2]).bytes, WAVEFORM);
  memcpy(this->half_to_white_, HalfStepWaveform(true, lengths[0], lengths[1], lengths[2]).bytes, WAVEFORM);
  ESP_LOGV(TAG, "%.0f Hz for %.0f °C; half steps %u, %u, %u at %.0f Hz", frame_rate_hz(this->frame_rate_), celsius,
           lengths[0], lengths[1], lengths[2], frame_rate_hz(this->half_rate_));
}

// A second device on the bus for reading: the board wires the panel's data line to the bus's MOSI
// pin alone, so reads turn that pin around (3-wire, half duplex). Only on a hardware bus.
void CrowPanel42::add_reader_() {
  const auto host = this->parent_->get_interface();
  if (host != SPI2_HOST && host != SPI3_HOST)
    return;
  spi_device_interface_config_t config = {};
  config.mode = 0;
  config.clock_speed_hz = 1000000;
  config.spics_io_num = -1;
  config.flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX;
  config.queue_size = 1;
  if (spi_bus_add_device(host, &config, &this->reader_) != ESP_OK)
    this->reader_ = nullptr;
}

// The temperature the controller sensed at its last power on (0x40), in whole degrees.
void CrowPanel42::read_panel_temperature_() {
  if (this->reader_ == nullptr || this->cs_ == nullptr)
    return;
  spi_transaction_t command = {};
  command.flags = SPI_TRANS_USE_TXDATA;
  command.length = 8;
  command.tx_data[0] = 0x40;
  spi_transaction_t reply = {};
  reply.flags = SPI_TRANS_USE_RXDATA;
  reply.rxlength = 16;
  if (spi_device_acquire_bus(this->reader_, portMAX_DELAY) != ESP_OK)
    return;
  this->dc_pin_->digital_write(false);
  this->cs_->digital_write(false);
  esp_err_t err = spi_device_polling_transmit(this->reader_, &command);
  this->dc_pin_->digital_write(true);
  if (err == ESP_OK)
    err = spi_device_polling_transmit(this->reader_, &reply);
  this->cs_->digital_write(true);
  spi_device_release_bus(this->reader_);
  const int celsius = static_cast<int8_t>(reply.rx_data[0]);
  // The sensor reads -25 to 49 °C; anything far outside is a misread.
  if (err != ESP_OK || celsius < -30 || celsius > 60) {
    ESP_LOGW(TAG, "Could not read the panel's temperature");
    return;
  }
  this->panel_celsius_ = celsius;
  ESP_LOGD(TAG, "Panel at %d °C", celsius);
}

// Decides what an update is, at its first transfer: a partial refresh when the count says so, fast
// whenever the driver knows what the glass shows, or a full refresh, after cancelling any fast
// partials' pushes. A partial in which no row changed has nothing to send or refresh. With grey
// partials, a partial is a grey refresh of the window around what changed instead.
void CrowPanel42::plan_update_() {
  this->pass_count_ = 0;
  this->unchanged_ = false;
  this->windowed_ = false;
  if (this->update_count_ != 0 && this->shown_valid_) {
    if (!this->find_changed_window_()) {
      this->unchanged_ = true;
      return;
    }
    this->windowed_ = true;
    if (this->unsettled_in_window_())
      this->passes_[this->pass_count_++] = Pass::CORRECT;
    this->plan_grey_passes_();
    ESP_LOGV(TAG, "Grey refresh of x %u to %u, y %u to %u", (unsigned) this->window_.first_bank * 8,
             (unsigned) this->window_.last_bank * 8 + 7, (unsigned) this->window_.first_row,
             (unsigned) this->window_.last_row);
  } else if (this->update_count_ != 0) {
    if (!this->find_changed_rows_()) {
      this->unchanged_ = true;
      return;
    }
    const bool fast = this->glass_valid_;
    this->passes_[this->pass_count_++] = fast ? Pass::FAST : Pass::PARTIAL;
    ESP_LOGV(TAG, "%s refresh", fast ? "Fast partial" : "Partial");
  } else {
    if (this->unsettled_count_ != 0)
      this->passes_[this->pass_count_++] = Pass::CORRECT;
    if (!this->grey_) {
      this->passes_[this->pass_count_++] = Pass::BLACK_WHITE;
    } else {
      this->plan_grey_passes_();
    }
  }
  glass_rows_valid = false;  // until this update has finished
}

void CrowPanel42::plan_grey_passes_() {
  if (!this->seven_) {
    this->passes_[this->pass_count_++] = Pass::GREY;
  } else {
    this->passes_[this->pass_count_++] = Pass::HALF_TO_BLACK;
    this->passes_[this->pass_count_++] = Pass::EVEN_GREY;
    this->passes_[this->pass_count_++] = Pass::HALF_TO_WHITE;
  }
}

// The smallest window holding every pixel whose level differs from the last frame drawn: whole
// rows down, and across in the controller's 8-pixel banks. False when no pixel differs.
bool CrowPanel42::find_changed_window_() {
  int first_row = -1, last_row = -1, first_bank = PLANE_ROW, last_bank = -1;
  const size_t width = this->row_width_;
  for (size_t y = 0; y != HEIGHT; y++) {
    const uint8_t *now = this->pixels_ + y * width, *was = this->shown_ + y * width;
    if (memcmp(now, was, width) == 0)
      continue;
    if (first_row < 0)
      first_row = y;
    last_row = y;
    size_t first = 0, last = width - 1;
    while (now[first] == was[first])
      first++;
    while (now[last] == was[last])
      last--;
    first_bank = std::min<int>(first_bank, first / 4);  // four buffer bytes to a bank
    last_bank = std::max<int>(last_bank, last / 4);
  }
  if (first_row < 0)
    return false;
  this->window_ = {static_cast<uint8_t>(first_bank), static_cast<uint8_t>(last_bank), static_cast<uint16_t>(first_row),
                   static_cast<uint16_t>(last_row)};
  return true;
}

bool CrowPanel42::in_window_(size_t y, size_t bank) const {
  return y >= this->window_.first_row && y <= this->window_.last_row && bank >= this->window_.first_bank &&
         bank <= this->window_.last_bank;
}

// Whether any pixel a fast partial left out of balance is in the window.
bool CrowPanel42::unsettled_in_window_() const {
  if (this->unsettled_count_ == 0)
    return false;
  for (size_t y = this->window_.first_row; y <= this->window_.last_row; y++) {
    for (size_t bank = this->window_.first_bank; bank <= this->window_.last_bank; bank++) {
      if (this->unsettled_[y * PLANE_ROW + bank] != 0)
        return true;
    }
  }
  return false;
}

// Sends the current pass's planes. A partial sends only NEW: OLD already holds what is on the
// glass. Cancelling sends which pixels are out of balance (OLD) and what the glass shows (NEW).
bool CrowPanel42::transfer_data() {
  if (!this->pass_started_) {
    if (this->pass_index_ == 0)
      this->plan_update_();
    if (this->unchanged_)
      return true;
    this->pass_started_ = true;
    const Pass pass = this->passes_[this->pass_index_];
    this->plane_ = pass == Pass::FAST || pass == Pass::PARTIAL ? 1 : 0;
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
      return this->send_planes_(Source{nullptr, this->unsettled_}, Source{nullptr, this->glass_});
  }
  return true;
}

// The supplies come on before the first refresh of a spell; the state machine waits on BUSY, which
// the controller holds low until they are up.
void CrowPanel42::power_on() {
  if (this->unchanged_ || this->supplies_on_)
    return;
  this->switch_supplies_on_();
  this->read_temperature_ = true;
}

void CrowPanel42::refresh_screen(bool partial) {
  if (this->unchanged_) {
    ESP_LOGV(TAG, "Nothing changed; no refresh");
    return;
  }
  // The controller senses the temperature as the supplies come on, so the update's frame rate is
  // settled here, once they are up.
  if (this->read_temperature_) {
    this->read_temperature_ = false;
    this->read_panel_temperature_();
  }
  if (this->pass_index_ == 0)
    this->set_frame_rates_();
  switch (this->passes_[this->pass_index_]) {
    // A fast partial's push may be cancelled hours later at another temperature, so the push, its
    // cancelling and its settling always run at 50 Hz: a frame's drive has to match exactly.
    case Pass::FAST:
      this->load_waveform_(ONE_WAY.bytes, FRAMES_50_HZ);
      break;
    case Pass::CORRECT:
      this->load_waveform_(CORRECTION.bytes, FRAMES_50_HZ);
      break;
    // These balance on their own, or, the half steps, within one update.
    case Pass::PARTIAL:
      this->load_waveform_(PARTIAL.bytes, this->frame_rate_);
      break;
    case Pass::BLACK_WHITE:
      this->load_waveform_(BLACK_WHITE.bytes, this->frame_rate_);
      break;
    case Pass::GREY:
      this->load_waveform_(FOUR_GREY.bytes, this->frame_rate_);
      break;
    case Pass::HALF_TO_BLACK:
      this->load_waveform_(this->half_to_black_, this->half_rate_);
      break;
    case Pass::EVEN_GREY:
      this->load_waveform_(EVEN_GREY.bytes, this->frame_rate_);
      break;
    case Pass::HALF_TO_WHITE:
      this->load_waveform_(this->half_to_white_, this->half_rate_);
      break;
  }
  // A partial window (0x91, 0x90) confines the refresh: pixels outside it rest at 0 V, which at
  // VCOM's null leaves them as they are. The gates scan the whole panel (PT_SCAN 1): scanning only
  // the window's rows let the rest drift by up to 0.5.
  if (this->windowed_) {
    const uint16_t x0 = this->window_.first_bank * 8, x1 = this->window_.last_bank * 8 + 7;
    const uint16_t y0 = this->window_.first_row, y1 = this->window_.last_row;
    this->command(0x91);
    this->cmd_data(0x90, {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0 & 0xF8), static_cast<uint8_t>(x1 >> 8),
                          static_cast<uint8_t>(x1 & 0xFF), static_cast<uint8_t>(y0 >> 8),
                          static_cast<uint8_t>(y0 & 0xFF), static_cast<uint8_t>(y1 >> 8),
                          static_cast<uint8_t>(y1 & 0xFF), 0x01});
  }
  this->command(0x12);
  this->next_delay_ = 2;  // BUSY falls a moment after the command
}

// Loads a waveform's tables and its frame rate. The controller's VCOM and white to black tables
// take eight groups and the others six; in black and white mode it runs six, so the last two are
// sent empty.
void CrowPanel42::load_waveform_(const uint8_t *waveform, uint8_t frame_rate) {
  // The registers in the controller's order: VCOM, WW, black to white (WB here), white to black
  // (BW here), BB.
  static constexpr uint8_t ORDER[5] = {0, TABLE_WW, TABLE_WB, TABLE_BW, TABLE_BB};
  uint8_t table[8 * 7];
  for (int r = 0; r < 5; r++) {
    const size_t length = (r == 0 || r == 2) ? 8 * 7 : TABLE;
    memset(table, 0, sizeof table);
    memcpy(table, waveform + ORDER[r] * TABLE, TABLE);
    this->cmd_data(0x20 + r, table, length);
  }
  this->cmd_data(0x30, {frame_rate});
}

// Runs once a pass has finished. With passes left, loop() starts the next. After the last, the
// glass's reading is brought up to date, fast partials' pushes are recorded or cleared, and the
// reading goes to both planes for the next partial to compare with: the controller never copies
// NEW to OLD itself. The base moves straight on, so loop() writes them, in slices.
void CrowPanel42::power_off() {
  this->pass_started_ = false;
  // Out of partial mode before any plane is written: in it, writes wrap within the window.
  if (this->windowed_)
    this->command(0x92);
  if (this->pass_index_ + 1 < this->pass_count_) {
    this->pass_index_++;
    this->next_pass_ = true;
    return;
  }
  const Pass last = this->passes_[this->pass_index_];
  this->pass_index_ = 0;
  if (this->unchanged_)
    return;
  this->record_glass_(last == Pass::FAST, this->passes_[0] == Pass::CORRECT);
  this->copying_ = true;
  if (this->shown_ != nullptr) {
    memcpy(this->shown_, this->pixels_, this->buffer_length_);
    this->shown_valid_ = true;
  }
}

// The new frame's black and white reading becomes the glass's, row by row: its hash for the next
// update, and with the bit planes, which pixels a fast partial has pushed out of balance (each
// change flips its pixel) or that cancelling has put right: all of them, or with a window, those
// in it.
void CrowPanel42::record_glass_(bool fast, bool corrected) {
  uint8_t row[PLANE_ROW];
  for (size_t y = 0; y != HEIGHT; y++) {
    this->plane_bytes_(WHITE_SOURCE, y, row);
    glass_rows[y] = row_hash(row);
    if (this->glass_ == nullptr)
      continue;
    uint8_t *glass = this->glass_ + y * PLANE_ROW;
    uint8_t *unsettled = this->unsettled_ + y * PLANE_ROW;
    for (size_t i = 0; i != PLANE_ROW; i++) {
      if (corrected && (!this->windowed_ || this->in_window_(y, i)))
        unsettled[i] = 0;
      if (fast)
        unsettled[i] ^= glass[i] ^ row[i];
      glass[i] = row[i];
    }
  }
  glass_rows_valid = true;
  this->glass_valid_ = this->glass_ != nullptr;
  this->status_clear_warning();
  this->unsettled_count_ = 0;
  if (this->glass_ != nullptr) {
    for (size_t i = 0; i != BIT_PLANE; i++)
      this->unsettled_count_ += __builtin_popcount(this->unsettled_[i]);
  }
  if (this->unsettled_count_ != 0) {
    ESP_LOGV(TAG, "%u pixels a push out of balance", (unsigned) this->unsettled_count_);
  }
}

// The base runs one step of the update a loop() call, and ESPHome calls loop() about every 16 ms,
// so the steps run back to back here instead, until one has to wait (BUSY, a delay) or the
// budget is spent.
void CrowPanel42::loop() {
  const uint32_t start = millis();
  if (this->state_ != epaper_spi::EPaperState::IDLE && start - this->update_start_time_ > UPDATE_DEADLINE_MS) {
    this->give_up_();
    return;
  }
  while (millis() - start < STEP_BUDGET_MS) {
    if (this->copying_) {
      if (!this->send_planes_(WHITE_SOURCE, WHITE_SOURCE))
        continue;
      this->copying_ = false;
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

// The base would sleep the controller after every update. Here the supplies stay on until
// ACTIVE_MS passes without another update, so the next one skips switching them on, but no longer
// than MAX_ACTIVE_MS at a stretch. The controller itself stays in standby, keeping its RAM.
void CrowPanel42::deep_sleep() {
  if (this->supplies_on_ && millis() - this->supplies_since_ >= MAX_ACTIVE_MS) {
    this->switch_supplies_off_();
    return;
  }
  this->set_timeout("idle", ACTIVE_MS, [this]() { this->wind_down_(); });
}

void CrowPanel42::wind_down_() {
  if (this->state_ != epaper_spi::EPaperState::IDLE || this->copying_)
    return;
  this->switch_supplies_off_();
}

// The panel has not finished an update in UPDATE_DEADLINE_MS: a loose ribbon or a failed
// controller. Stop waiting, reset the controller so the supplies are off and the device can sleep,
// and start again from a full refresh.
void CrowPanel42::give_up_() {
  ESP_LOGE(TAG, "The panel did not finish an update in %u s; resetting it", (unsigned) (UPDATE_DEADLINE_MS / 1000));
  this->status_set_warning();
  this->reset_controller_();
  this->copying_ = false;
  this->sending_ = false;
  this->plane_ = 0;
  this->pass_index_ = 0;
  this->pass_started_ = false;
  this->next_pass_ = false;
  this->glass_valid_ = false;
  this->shown_valid_ = false;
  this->windowed_ = false;
  glass_rows_valid = false;
  if (this->unsettled_ != nullptr)
    memset(this->unsettled_, 0, BIT_PLANE);
  this->unsettled_count_ = 0;
  this->update_count_ = 0;
  this->set_state_(epaper_spi::EPaperState::IDLE);
}

// A hardware reset, which switches the supplies off and the registers back to their defaults but
// keeps the RAM.
void CrowPanel42::reset_controller_() {
  this->cancel_timeout("idle");
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->digital_write(false);
    delay(2);
    this->reset_pin_->digital_write(true);
    delay(10);
  }
  this->ready_ = false;
  this->supplies_on_ = false;
  this->wait_idle_(100);
}

// Settling, waiting on BUSY, for a deep sleep that cannot wait for an update: only the pixels a
// fast partial left a push out blink, and land where they were.
void CrowPanel42::settle_now_() {
  if (!this->ready_) {
    this->reset_controller_();
    this->send_init_();
    this->ready_ = true;
  }
  this->wait_idle_(3000);
  this->plane_ = 0;
  while (!this->send_planes_(Source{nullptr, this->unsettled_}, Source{nullptr, this->glass_})) {
  }
  this->load_waveform_(SETTLE.bytes, FRAMES_50_HZ);
  if (!this->supplies_on_) {
    this->switch_supplies_on_();
    this->wait_idle_(500);
  }
  this->command(0x12);
  this->wait_idle_(3000);
  while (!this->send_planes_(Source{nullptr, this->glass_}, Source{nullptr, this->glass_})) {
  }
  memset(this->unsettled_, 0, BIT_PLANE);
  this->unsettled_count_ = 0;
}

void CrowPanel42::wait_idle_(uint32_t timeout_ms) {
  const uint32_t start = millis();
  delay(2);  // BUSY falls a moment after the command
  while (!this->is_idle_() && millis() - start < timeout_ms)
    delay(1);
}

void CrowPanel42::switch_supplies_on_() {
  ESP_LOGV(TAG, "Supplies on");
  this->command(0x04);
  this->next_delay_ = 2;
  this->supplies_on_ = true;
  this->supplies_since_ = millis();
}

// Switches the ±15 V supplies off, without waiting: the next command waits for BUSY anyway.
void CrowPanel42::switch_supplies_off_() {
  if (!this->supplies_on_)
    return;
  ESP_LOGV(TAG, "Supplies off after %u s", (unsigned) ((millis() - this->supplies_since_) / 1000));
  this->command(0x02);
  this->supplies_on_ = false;
}

}  // namespace esphome::crowpanel_42
