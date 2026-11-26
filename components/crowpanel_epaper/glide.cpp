#include "crowpanel_epaper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// Glides and live ticks, which push pixels part of the way rather than through a whole waveform.
//
// A scan is one refresh with the planes as masks: OLD white and NEW black (BW) pushes a pixel
// black, OLD black and NEW white (WB) pushes it white, and both the same (WW, BB) holds. A glide
// takes each changed pixel from the level last drawn to its new one with the frames its panel's
// tuning gives, as a scan for each binary digit of the counts in the window: a pixel that rests
// between scans loses its way, so as few as possible. A tick is one scan of TICK frames, pushing
// each pixel towards its level along curves measured for streams of such scans. Either way each
// pixel's net drive is recorded, and cancelled by scans before the next grey refresh covering it.

namespace esphome::crowpanel_epaper {

static const char *const TAG = "crowpanel_epaper";

// A pixel's drive record holds up to this many frames either way. A glide that would take a pixel
// further settles instead; a tick holds the pixel until something does.
static constexpr int MAX_DRIVE = 127;
// Each level's lightness, 0 to 255, as a grey refresh draws it.
static constexpr uint8_t LEVEL_LIGHT[7] = {0, 43, 85, 128, 170, 213, 255};

void CrowPanelBase::merge(Window &into, const Window &window) {
  if (is_empty(window))
    return;
  if (is_empty(into)) {
    into = window;
    return;
  }
  into.first_bank = std::min(into.first_bank, window.first_bank);
  into.last_bank = std::max(into.last_bank, window.last_bank);
  into.first_row = std::min(into.first_row, window.first_row);
  into.last_row = std::max(into.last_row, window.last_row);
}

void CrowPanelBase::set_live(bool live) {
  this->live_ = live;
  this->estimate_valid_ = false;
  if (!live) {
    this->fast_loop_.stop();
    this->prerendered_ = false;
  }
}

// The smallest window holding every pixel whose level differs from the last frame drawn: whole
// rows down, and across in plane bytes. False when no pixel differs.
bool CROWPANEL_FAST CrowPanelBase::find_changed_window_() {
  int first_row = -1, last_row = -1, first_bank = this->plane_row_, last_bank = -1;
  const size_t width = this->row_width_;
  for (size_t y = 0; y != this->height_; y++) {
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
    first_bank = std::min<int>(first_bank, first / 4);  // four buffer bytes to a plane byte
    last_bank = std::max<int>(last_bank, last / 4);
  }
  if (first_row < 0)
    return false;
  this->window_ = {static_cast<uint8_t>(first_bank), static_cast<uint8_t>(last_bank), static_cast<uint16_t>(first_row),
                   static_cast<uint16_t>(last_row)};
  return true;
}

// The drive record and the masks, allocated the first time they are wanted, in PSRAM.
bool CrowPanelBase::glide_buffers_() {
  if (this->glide_tuning_() == nullptr || this->shown_ == nullptr || this->glass_ == nullptr || this->pixels_ == nullptr)
    return false;
  RAMAllocator<uint8_t> allocator;
  const size_t pixels = this->width_ * this->height_;
  if (this->drive_ == nullptr) {
    this->drive_ = reinterpret_cast<int8_t *>(allocator.allocate(pixels));
    if (this->drive_ != nullptr)
      memset(this->drive_, 0, pixels);
  }
  if (this->masks_ == nullptr) {
    this->masks_ = allocator.allocate(2 * this->bit_plane_);
    if (this->masks_ != nullptr)
      memset(this->masks_, 0xFF, 2 * this->bit_plane_);
  }
  return this->drive_ != nullptr && this->masks_ != nullptr;
}

// This update's glide from each level to each other: the tuning's frames, fewer in the warm and
// more in the cold.
void CrowPanelBase::set_doses_() {
  const GlideTuning &tuning = *this->glide_tuning_();
  const float speed = this->speed_();
  for (int from = 0; from < 7; from++) {
    for (int to = 0; to < 7; to++)
      this->doses_[from][to] = static_cast<int8_t>(std::clamp<long>(std::lround(tuning.glide[from][to] / speed), -63, 63));
  }
}

// A glide, when the driver knows every pixel's level and nobody asked for exact greys: a scan for
// each binary digit the doses in the window around what changed need. Going to black or white,
// pushing further changes nothing to see, so a glide there also pays back drive the other way.
// Not when a pixel's drive would pass its record's limit: glide_overflowed_ says so.
bool CROWPANEL_FAST CrowPanelBase::plan_glide_() {
  this->glide_overflowed_ = false;
  if (!this->glide_ || this->settle_requested_ || !this->shown_valid_ || !this->glide_buffers_())
    return false;
  if (this->push_ == nullptr) {
    RAMAllocator<uint8_t> allocator;
    this->push_ = reinterpret_cast<int8_t *>(allocator.allocate(this->width_ * this->height_));
    if (this->push_ == nullptr)
      return false;
  }
  if (!this->find_changed_window_()) {
    this->unchanged_ = true;
    return true;
  }
  this->set_doses_();
  const Window &w = this->window_;
  uint8_t bits = 0;
  for (size_t y = w.first_row; y <= w.last_row; y++) {
    for (size_t x = w.first_bank * 8u; x != (w.last_bank + 1u) * 8u; x++) {
      const size_t i = y * this->width_ + x;
      const uint8_t to = this->level_at_(this->pixels_, x, y);
      const int drive = this->drive_[i];
      int dose = this->doses_[this->level_at_(this->shown_, x, y)][to];
      if (to == 0 && dose > 0)
        dose = std::min(63, std::max(dose, -drive));
      if (to == 6 && dose < 0)
        dose = std::max(-63, std::min(dose, -drive));
      if (std::abs(drive + dose) > MAX_DRIVE) {
        this->glide_overflowed_ = true;
        return false;
      }
      this->push_[i] = dose;
      bits |= std::abs(dose);
    }
  }
  if (bits == 0) {
    this->drew_window_();  // changes too small to push
    this->unchanged_ = true;
    return true;
  }
  this->plan_scans_(Pass::DOSE, bits);
  ESP_LOGV(TAG, "Glide of x %u to %u, y %u to %u in scans of 0x%02X frames", (unsigned) w.first_bank * 8,
           (unsigned) w.last_bank * 8 + 7, (unsigned) w.first_row, (unsigned) w.last_row, bits);
  return true;
}

// A pass for each binary digit of `bits`, the shortest first.
void CrowPanelBase::plan_scans_(Pass pass, uint8_t bits) {
  if (pass == Pass::UNDOSE)
    ESP_LOGV(TAG, "Cancelling glides' drive in scans of 0x%02X frames", bits);
  for (int k = 0; k < 8 && this->pass_count_ < sizeof this->passes_; k++) {
    if ((bits >> k) & 1) {
      this->passes_[this->pass_count_] = pass;
      this->scan_frames_[this->pass_count_] = 1 << k;
      this->pass_count_++;
    }
  }
}

// The binary digits of the drive on the window's pixels.
uint8_t CrowPanelBase::drive_bits_() const {
  uint8_t bits = 0;
  if (this->drive_ == nullptr)
    return 0;
  const Window &w = this->window_;
  for (size_t y = w.first_row; y <= w.last_row; y++) {
    const int8_t *row = this->drive_ + y * this->width_;
    for (size_t x = w.first_bank * 8u; x != (w.last_bank + 1u) * 8u; x++)
      bits |= std::abs(row[x]);
  }
  return bits;
}

// Back to holding wherever the last scan's masks were.
void CrowPanelBase::clear_masks_() {
  if (is_empty(this->masked_))
    return;
  const Window &m = this->masked_;
  const size_t count = m.last_bank - m.first_bank + 1;
  for (size_t y = m.first_row; y <= m.last_row; y++) {
    memset(this->masks_ + y * this->plane_row_ + m.first_bank, 0xFF, count);
    memset(this->masks_ + this->bit_plane_ + y * this->plane_row_ + m.first_bank, 0xFF, count);
  }
  this->masked_ = EMPTY_WINDOW;
}

// A glide scan's masks over the window, from each pixel's dose; or cancelling's, pushing it back by
// the bits of its drive.
void HOT CROWPANEL_FAST CrowPanelBase::build_masks_(Pass pass, uint8_t frames) {
  this->clear_masks_();
  uint8_t *old_plane = this->masks_, *new_plane = this->masks_ + this->bit_plane_;
  const int8_t *pushes = pass == Pass::DOSE ? this->push_ : this->drive_;
  const int sign = pass == Pass::DOSE ? 1 : -1;
  const Window &w = this->window_;
  for (size_t y = w.first_row; y <= w.last_row; y++) {
    const int8_t *row = pushes + y * this->width_;
    for (size_t bank = w.first_bank; bank <= w.last_bank; bank++) {
      uint8_t old_bits = 0xFF, new_bits = 0xFF;
      for (size_t j = 0; j != 8; j++) {
        const int push = sign * row[bank * 8 + j];  // + black, - white
        if ((std::abs(push) & frames) == 0)
          continue;
        if (push > 0) {
          new_bits &= ~(0x80 >> j);
        } else {
          old_bits &= ~(0x80 >> j);
        }
      }
      old_plane[y * this->plane_row_ + bank] = old_bits;
      new_plane[y * this->plane_row_ + bank] = new_bits;
    }
  }
  this->masked_ = w;
}

// After the last pass: a glide adds its doses to the window's drive, and cancelling clears it.
void CROWPANEL_FAST CrowPanelBase::record_drive_(Pass last) {
  if (this->drive_ == nullptr)
    return;
  const bool glide = last == Pass::DOSE, cancelled = this->passes_[0] == Pass::UNDOSE;
  if (!glide && !cancelled)
    return;
  const Window &w = this->window_;
  for (size_t y = w.first_row; y <= w.last_row; y++) {
    for (size_t x = w.first_bank * 8u; x != (w.last_bank + 1u) * 8u; x++) {
      const size_t i = y * this->width_ + x;
      int8_t &drive = this->drive_[i];
      this->driven_count_ -= drive != 0;
      drive = glide ? drive + this->push_[i] : 0;
      this->driven_count_ += drive != 0;
    }
  }
  ESP_LOGV(TAG, "%u pixels driven", (unsigned) this->driven_count_);
}

// A glide drew the window's rows near enough: the next starts from them, and the next grey
// refresh that settles must cover them.
void CROWPANEL_FAST CrowPanelBase::drew_window_() {
  this->drew_(this->window_);
  merge(this->dirty_, this->window_);
}

// The frame's levels in the rows that changed are the ones drawn from now on.
void CROWPANEL_FAST CrowPanelBase::drew_(const Window &changed) {
  const size_t row = this->row_width_;
  memcpy(this->shown_ + changed.first_row * row, this->pixels_ + changed.first_row * row,
         (changed.last_row + 1 - changed.first_row) * row);
  this->rows_drawn_(changed.first_row, changed.last_row);
}

// The model starts from the levels last drawn.
void CrowPanelBase::estimate_levels_() {
  for (size_t y = 0; y != this->height_; y++) {
    for (size_t x = 0; x != this->width_; x++)
      this->estimate_[y * this->width_ + x] = LEVEL_LIGHT[this->level_at_(this->shown_, x, y)];
  }
  this->estimate_valid_ = true;
}

// Lightness after a curve's frames, and the frames to a lightness, linear between the points.
using Curve = float[11][2];
static float curve_at(const Curve &curve, float frames) {
  for (int i = 1; i < 11; i++) {
    if (frames <= curve[i][0]) {
      const float along = (frames - curve[i - 1][0]) / (curve[i][0] - curve[i - 1][0]);
      return curve[i - 1][1] + along * (curve[i][1] - curve[i - 1][1]);
    }
  }
  return curve[10][1];
}
static float curve_frames(const Curve &curve, float light) {
  for (int i = 1; i < 11; i++) {
    const float a = curve[i - 1][1], b = curve[i][1];
    if ((light - a) * (light - b) <= 0)
      return curve[i - 1][0] + (light - a) / (b - a) * (curve[i][0] - curve[i - 1][0]);
  }
  return std::abs(light - curve[0][1]) < std::abs(light - curve[10][1]) ? 0 : curve[10][0];
}

// Where a tick of black or of white takes each lightness; and which way a pixel with no drive to
// pay back goes towards each level: a grey while a tick brings it nearer, black and white as far as
// ticks still darken or lighten them.
void CrowPanelBase::build_tick_tables_() {
  const GlideTuning &tuning = *this->glide_tuning_();
  const float speed = this->speed_();
  for (int e = 0; e < 256; e++) {
    const float light = e / 255.0f;
    const float b = curve_at(tuning.tick_black, curve_frames(tuning.tick_black, light) + TICK * speed);
    const float w = curve_at(tuning.tick_white, curve_frames(tuning.tick_white, light) + TICK * speed);
    this->tick_black_[e] = static_cast<uint8_t>(std::lround(std::min(light, b) * 255));
    this->tick_white_[e] = static_cast<uint8_t>(std::lround(std::max(light, w) * 255));
  }
  for (int to = 0; to < 7; to++) {
    for (int now = 0; now < 256; now++) {
      int push = 0;
      if (to == 0) {
        push = this->tick_black_[now] + 2 < now ? 1 : 0;
      } else if (to == 6) {
        push = this->tick_white_[now] > now + 2 ? -1 : 0;
      } else {
        const int target = LEVEL_LIGHT[to], away = std::abs(now - target);
        if (target < now && std::abs(this->tick_black_[now] - target) + 2 < away)
          push = 1;
        if (target > now && std::abs(this->tick_white_[now] - target) + 2 < away)
          push = -1;
      }
      this->tick_push_[to][now] = static_cast<int8_t>(push);
    }
  }
  this->tick_tables_ready_ = true;
}

// Which way a tick pushes a pixel: +1 black, -1 white, 0 hold. At black or white, while it carries
// drive the other way, on further: nothing to see, but it pays the drive back.
inline int CROWPANEL_FAST CrowPanelBase::decide_(uint8_t now, uint8_t to, int drive) const {
  const int push = this->tick_push_[to][now];
  if (push != 0)
    return push;
  if (to == 0 && drive <= -TICK)
    return 1;
  if (to == 6 && drive >= TICK)
    return -1;
  return 0;
}

// A tick, in live mode: the window around what changed since the last frame drawn and what is
// still on its way, each pixel in it pushed a tick towards its level or held. Eight pixels that
// have arrived and whose levels have not changed are skipped whole. The masks are built here, and
// the model, the drive record, the glass's reading and the frame drawn all move on now, so the
// next frame can be drawn while this one's refresh runs. A pixel whose drive would pass its
// record's limit holds until a grey refresh settles it.
bool CrowPanelBase::plan_tick_() {
  int tick;
  if (this->planned_ahead_) {
    this->planned_ahead_ = false;
    tick = this->ahead_pushes_ ? 1 : 0;
  } else {
    tick = this->compute_tick_();
  }
  if (tick < 0)
    return false;
  this->live_update_ = true;
  if (tick == 0) {
    this->unchanged_ = true;
    this->fast_loop_.stop();
    return true;
  }
  this->fast_loop_.start();
  this->ticking_ = true;
  this->passes_[this->pass_count_] = Pass::DOSE;
  this->scan_frames_[this->pass_count_] = TICK;
  this->pass_count_++;
  return true;
}

// The tick itself: -1 when there can be none, 0 when nothing moves, 1 when it pushes, its masks
// built and everything recorded.
int HOT CROWPANEL_FAST CrowPanelBase::compute_tick_() {
  if (!this->live_ || this->settle_requested_ || !this->shown_valid_ || !this->glide_buffers_())
    return -1;
  RAMAllocator<uint8_t> allocator;
  const size_t pixels = this->width_ * this->height_;
  if (this->estimate_ == nullptr)
    this->estimate_ = allocator.allocate(pixels);
  if (this->moving_banks_ == nullptr) {
    this->moving_banks_ = allocator.allocate(this->bit_plane_);
    if (this->moving_banks_ != nullptr)
      memset(this->moving_banks_, 0, this->bit_plane_);
  }
  if (this->estimate_ == nullptr || this->moving_banks_ == nullptr)
    return -1;
  if (!this->tick_tables_ready_)
    this->build_tick_tables_();
  if (!this->estimate_valid_)
    this->estimate_levels_();
  Window window = this->moving_, changed = EMPTY_WINDOW;
  if (this->find_changed_window_()) {
    changed = this->window_;
    merge(window, changed);
  }
  this->moving_ = EMPTY_WINDOW;
  if (is_empty(window))
    return 0;
  this->window_ = window;
  this->clear_masks_();
  // Half the rows on the other core, half here; they share nothing but tables neither writes.
  if (this->helper_ == nullptr) {
    this->helper_start_ = xSemaphoreCreateBinary();
    this->helper_done_ = xSemaphoreCreateBinary();
    if (this->helper_start_ != nullptr && this->helper_done_ != nullptr)
      xTaskCreatePinnedToCore(tick_helper_, "crowpanel", 4096, this, 5, &this->helper_, 1 - xPortGetCoreID());
  }
  const uint16_t mid = (window.first_row + window.last_row + 1) / 2;
  TickRows top{window.first_row, mid, EMPTY_WINDOW, 0, 0, 0, 0};
  TickRows bottom{mid, static_cast<uint16_t>(window.last_row + 1), EMPTY_WINDOW, 0, 0, 0, 0};
  if (this->helper_ != nullptr && window.last_row - window.first_row >= 8) {
    this->helper_rows_ = bottom;
    this->helper_window_ = &window;
    xSemaphoreGive(this->helper_start_);
    this->tick_rows_(top, window);
    xSemaphoreTake(this->helper_done_, portMAX_DELAY);
    bottom = this->helper_rows_;
  } else {
    this->tick_rows_(top, window);
    this->tick_rows_(bottom, window);
  }
  merge(this->moving_, top.moving);
  merge(this->moving_, bottom.moving);
  this->driven_count_ += top.driven + bottom.driven;
  const uint32_t blacks = top.blacks + bottom.blacks, whites = top.whites + bottom.whites,
                 held = top.held + bottom.held;
  this->masked_ = window;
  // Where the frame changed, its reading and levels are the ones drawn from now on, whether or not
  // anything moved; the window, pushed near enough, is for the next settle to cover.
  if (!is_empty(changed)) {
    this->record_glass_rows_(changed.first_row, changed.last_row + 1, false, false);
    this->drew_(changed);
  }
  merge(this->dirty_, window);
  if (blacks + whites == 0)
    return 0;
  ESP_LOGV(TAG, "Tick of x %u to %u, y %u to %u: %u black, %u white, %u held at their limit",
           (unsigned) window.first_bank * 8, (unsigned) window.last_bank * 8 + 7, (unsigned) window.first_row,
           (unsigned) window.last_row, (unsigned) blacks, (unsigned) whites, (unsigned) held);
  return 1;
}

// A tick's pass over some of its window's rows: each pixel pushed a tick towards its level or held,
// eight that have arrived and whose levels have not changed skipped whole, the masks built and the
// model and the drive record moved on.
void HOT CROWPANEL_FAST CrowPanelBase::tick_rows_(TickRows &rows, const Window &window) {
  uint8_t *old_plane = this->masks_, *new_plane = this->masks_ + this->bit_plane_;
  for (size_t y = rows.first; y != rows.end; y++) {
    for (size_t bank = window.first_bank; bank <= window.last_bank; bank++) {
      uint8_t &moving = this->moving_banks_[y * this->plane_row_ + bank];
      const size_t pair = y * this->row_width_ + bank * 4;
      if (!moving && memcmp(this->pixels_ + pair, this->shown_ + pair, 4) == 0)
        continue;  // the masks hold there already
      moving = 0;
      uint8_t old_bits = 0xFF, new_bits = 0xFF;
      const uint8_t *levels = this->pixels_ + pair;
      uint8_t *estimates = this->estimate_ + y * this->width_ + bank * 8;
      int8_t *drives = this->drive_ + y * this->width_ + bank * 8;
      for (size_t j = 0; j != 8; j++) {
        const uint8_t to = (j & 1) ? levels[j >> 1] & 0x0F : levels[j >> 1] >> 4;
        uint8_t &now = estimates[j];
        int8_t &drive = drives[j];
        const int push = this->decide_(now, to, drive);
        if (push == 0)
          continue;
        if (std::abs(drive + push * TICK) > MAX_DRIVE) {
          rows.held++;
          continue;
        }
        if (push > 0) {
          new_bits &= ~(0x80 >> j);
          now = this->tick_black_[now];
          rows.blacks++;
        } else {
          old_bits &= ~(0x80 >> j);
          now = this->tick_white_[now];
          rows.whites++;
        }
        rows.driven -= drive != 0;
        drive += push * TICK;
        rows.driven += drive != 0;
        const int next = this->decide_(now, to, drive);
        if (next != 0 && std::abs(drive + next * TICK) <= MAX_DRIVE)
          moving = 1;
      }
      if (moving)
        merge(rows.moving, {static_cast<uint8_t>(bank), static_cast<uint8_t>(bank), static_cast<uint16_t>(y),
                              static_cast<uint16_t>(y)});
      old_plane[y * this->plane_row_ + bank] = old_bits;
      new_plane[y * this->plane_row_ + bank] = new_bits;
    }
  }
}

void CrowPanelBase::tick_helper_(void *panel) {
  auto *self = static_cast<CrowPanelBase *>(panel);
  for (;;) {
    xSemaphoreTake(self->helper_start_, portMAX_DELAY);
    self->tick_rows_(self->helper_rows_, *self->helper_window_);
    xSemaphoreGive(self->helper_done_);
  }
}

// While this tick's refresh runs, the next frame is drawn and its tick planned, so the next only
// has to send and refresh: its masks are built (this tick's went out already) and the model moves
// on as if it had run, so it must run next. Not when an update was asked for during this tick:
// then the next draws afresh.
void CrowPanelBase::prerender_() {
  if (!this->live_ || !this->ticking_ || this->update_waiting_)
    return;
  this->do_update_();
  this->prerendered_ = true;
  const int tick = this->compute_tick_();
  this->planned_ahead_ = tick >= 0;
  this->ahead_pushes_ = tick > 0;
}

// At the end of an update: while live and anything is still moving, a frame is waiting to go, or
// an update was asked for, the next tick follows at once.
bool CrowPanelBase::continue_live_() {
  if (this->live_ && (this->update_waiting_ || this->prerendered_ || !is_empty(this->moving_))) {
    this->set_timeout("tick", 0, [this]() { this->update(); });
    return true;
  }
  this->fast_loop_.stop();
  return false;
}

// Before a deep sleep: a grey refresh of everything glides touched, waiting for it.
void CrowPanelBase::settle_glides_now_() {
  // A tick planned ahead is in the record already, so it runs first.
  if (this->planned_ahead_) {
    this->update();
    const uint32_t start = millis();
    while ((this->state_ != epaper_spi::EPaperState::IDLE || this->copying_) && millis() - start < 5000) {
      this->loop();
      App.feed_wdt();
      delay(1);
    }
  }
  ESP_LOGD(TAG, "Settling %u glided pixels before sleeping", (unsigned) this->driven_count_);
  this->prerendered_ = false;
  this->update_waiting_ = false;
  this->request_settle_();
  this->update();
  const uint32_t start = millis();
  while ((this->state_ != epaper_spi::EPaperState::IDLE || this->copying_) && millis() - start < 20000) {
    this->loop();
    App.feed_wdt();
    delay(1);
  }
}

}  // namespace esphome::crowpanel_epaper
