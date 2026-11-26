#include "crowpanel_epaper.h"

#include <algorithm>
#include <cstring>

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::crowpanel_epaper {

static const char *const TAG = "crowpanel_epaper";

// How long one loop() call may run the update's steps back to back, well inside ESPHome's 50 ms.
static constexpr uint32_t STEP_BUDGET_MS = 25;
// An update the panel has not finished in this long has failed: the longest, seven greys with
// their cancelling pass, take about 5 s.
static constexpr uint32_t UPDATE_DEADLINE_MS = 20000;

// Where the last update left the full_update_every count, kept through deep sleep so the count
// carries on across wakes. Only read on a wake from deep sleep, so it is always one this driver
// wrote.
static RTC_DATA_ATTR uint8_t slept_update_count;
// A hash of each row's black and white reading as the glass shows it, so a partial refresh sends
// only the rows that changed, and nothing at all when none did. Kept through deep sleep, and only
// trusted when glass_rows_valid says the last update finished.
static RTC_DATA_ATTR uint32_t glass_rows[CrowPanelBase::MAX_HEIGHT];
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

void CrowPanelBase::setup() {
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
  this->glass_ = allocator.allocate(2 * this->bit_plane_);
  if (this->glass_ != nullptr) {
    this->unsettled_ = this->glass_ + this->bit_plane_;
    memset(this->unsettled_, 0, this->bit_plane_);
  }
  // Grey partials, glides and ticks compare each frame with the last one drawn, level for level.
  if (this->grey_ && this->pixels_ != nullptr)
    this->shown_ = allocator.allocate(this->buffer_length_);
  this->setup_controllers_();
  // Waking from deep sleep, the panel kept its power, its picture and its RAM, so the count
  // carries on and the first update can be partial. Anything else starts from nothing.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    glass_rows_valid = false;
    // A reboot part way through, a crash or an update over the air, can leave the controllers
    // with their supplies on.
    this->reset_controllers_();
  } else if (this->is_using_partial_update_()) {
    this->update_count_ = slept_update_count % this->full_update_every_;
    ESP_LOGD(TAG, "Woke from deep sleep; the panel still holds its frame (update %u of %u)",
             (unsigned) this->update_count_, (unsigned) this->full_update_every_);
  }
}

// Before a deep sleep: finish writing the planes if a refresh has just run, cancel any fast
// partials' pushes, then put the controllers as they spend the sleep and hold the panel's power and
// reset pins high so it keeps its RAM.
void CrowPanelBase::on_safe_shutdown() {
  this->cancel_timeout("idle");
  this->cancel_timeout("sleep");
  while (this->copying_ && !this->send_planes_(WHITE_SOURCE, WHITE_SOURCE)) {
  }
  this->copying_ = false;
  using epaper_spi::EPaperState;
  const bool settled =
      !this->next_pass_ && (this->state_ == EPaperState::IDLE || this->state_ == EPaperState::DEEP_SLEEP);
  // Glides' drive lives in PSRAM, which the sleep loses, so a grey refresh cancels it now; and the
  // planes get the glass's reading back wherever scans left masks.
  if (settled && this->driven_count_ != 0)
    this->settle_glides_now_();
  if (settled)
    this->restore_reading_();
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
  this->prepare_for_sleep_(settled);
  hold(this->power_pin_, true);
  hold(this->hold_reset_pin_, true);
  gpio_deep_sleep_hold_en();
}

// Four greys use the even levels, in even steps of luma so anti-aliased text, which blends at
// 0, 85, 170 and 255, keeps all four. Seven use every level.
uint8_t CrowPanelBase::level_(Color color) const {
  const uint8_t luma = epaper_spi::rec601_luma(color);
  return this->seven_ ? luma * 7 / 256 : (luma >> 6) * 2;
}

void CrowPanelBase::fill(Color color) {
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

// The base's rotate_coordinates_ inline: clipping, the rotation, the panel's bounds; and the
// drawn region, which only says something was drawn, grown to the whole panel once.
void HOT CROWPANEL_FAST CrowPanelBase::draw_pixel_at(int x, int y, Color color) {
  if (this->is_point_clipped(x, y))
    return;
  const uint8_t transform = this->effective_transform_;
  if (transform & epaper_spi::SWAP_XY)
    std::swap(x, y);
  if (transform & epaper_spi::MIRROR_X)
    x = this->width_ - x - 1;
  if (transform & epaper_spi::MIRROR_Y)
    y = this->height_ - y - 1;
  if (static_cast<unsigned>(x) >= static_cast<unsigned>(this->width_) ||
      static_cast<unsigned>(y) >= static_cast<unsigned>(this->height_))
    return;
  if (this->x_high_ == 0) {
    this->x_low_ = 0;
    this->y_low_ = 0;
    this->x_high_ = this->width_;
    this->y_high_ = this->height_;
  }
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

// The deadline runs from here. The wind-down is left running until the update starts, since an
// update the lambda draws nothing for never does and would leave nothing to set it again. While
// ticking, an update waits for the tick, and the next tick skips drawing when its frame was drawn
// while the last one ran.
void CrowPanelBase::update() {
  if (this->state_ != epaper_spi::EPaperState::IDLE) {
    if (this->live_)
      this->update_waiting_ = true;
    else
      EPaperBase::update();  // which says the display is busy
    return;
  }
  this->started_ = millis();
  // A tick planned ahead runs next whatever was asked for since; an update asked for during it
  // draws afresh after it.
  if (this->planned_ahead_ || (this->prerendered_ && !this->update_waiting_)) {
    this->prerendered_ = false;
    this->set_state_(epaper_spi::EPaperState::RESET);
    this->enable_loop();
    return;
  }
  this->prerendered_ = false;
  this->update_waiting_ = false;
  EPaperBase::update();
}

// Plane bytes offset to offset + count of row y: from the buffer through a table, or from a bit
// plane as they are. Plane byte j holds pixels 8j to 8j + 7, the first in the top bit.
void HOT CROWPANEL_FAST CrowPanelBase::plane_bytes_(const Source &source, size_t y, size_t offset, size_t count, uint8_t *out) {
  if (source.plane != nullptr) {
    memcpy(out, source.plane + y * this->plane_row_ + offset, count);
    return;
  }
  // Plane byte offset + i holds the 8 pixels of buffer bytes 4 (offset + i) to the third after.
  const size_t start = y * this->row_width_ + 4 * offset;
  uint8_t copy[4 * MAX_PLANE_ROW];
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

// FNV-1a over a row's black and white reading.
static uint32_t CROWPANEL_FAST row_hash(const uint8_t *row, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i != length; i++)
    hash = (hash ^ row[i]) * 16777619u;
  return hash;
}

// The rows of the new frame whose black and white reading differs from the glass's, as one band.
// Returns false when there are none.
bool CrowPanelBase::find_changed_rows_() {
  int first = -1, last = -1;
  uint8_t row[MAX_PLANE_ROW];
  for (size_t y = 0; y != this->height_; y++) {
    this->plane_bytes_(WHITE_SOURCE, y, 0, this->plane_row_, row);
    if (!glass_rows_valid || row_hash(row, this->plane_row_) != glass_rows[y]) {
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
void CrowPanelBase::find_unsettled_rows_() {
  int first = -1, last = -1;
  for (size_t y = 0; y != this->height_; y++) {
    const uint8_t *row = this->unsettled_ + y * this->plane_row_;
    for (size_t i = 0; i != this->plane_row_; i++) {
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

// Decides what an update is, at its first transfer: a partial refresh when the count says so, fast
// whenever the driver knows what the glass shows, or a full refresh, after cancelling any fast
// partials' pushes. A partial whose rows are all unchanged has nothing to send or refresh. A driver
// may plan its own kind of update first.
void CrowPanelBase::plan_update_() {
  this->pass_count_ = 0;
  this->unchanged_ = false;
  this->ticking_ = false;
  this->live_update_ = false;
  this->plan_started_();
  if (this->update_count_ != 0 && this->plan_tick_())
    return;  // a tick records itself as it plans
  this->restore_reading_();
  // A glide that would take a pixel's drive past its record's limit settles instead: in its window
  // where the driver can, else by the full refresh due anyway.
  bool planned = false;
  if (this->update_count_ != 0) {
    planned = this->plan_glide_();
    if (!planned && this->glide_overflowed_ && !this->settles_windows_())
      this->update_count_ = 0;
  }
  // A full refresh cancels every glide's drive first.
  if (this->update_count_ == 0 && this->driven_count_ != 0) {
    this->window_ = this->whole_window_();
    this->plan_scans_(Pass::UNDOSE, this->drive_bits_());
  }
  if (!planned && (this->update_count_ == 0 || !this->plan_own_update_())) {
    if (this->update_count_ != 0) {
      if (!this->find_changed_rows_()) {
        this->unchanged_ = true;
        return;
      }
      const bool fast = this->glass_valid_;
      this->passes_[this->pass_count_++] = fast ? Pass::FAST : Pass::PARTIAL;
      ESP_LOGV(TAG, "%s refresh of rows %u to %u", fast ? "Fast partial" : "Partial", (unsigned) this->first_row_,
               (unsigned) this->end_row_ - 1);
    } else {
      ESP_LOGV(TAG, "Full refresh");
      if (this->unsettled_count_ != 0)
        this->passes_[this->pass_count_++] = Pass::CORRECT;
      if (!this->grey_) {
        this->passes_[this->pass_count_++] = Pass::BLACK_WHITE;
      } else {
        this->plan_grey_passes_();
      }
    }
  }
  if (this->unchanged_)
    return;
  glass_rows_valid = false;  // until this update has finished
  this->estimate_valid_ = false;  // ticks model the levels afresh from the frame this one draws
  // Scans follow each other closely, so the main loop runs unpaced until the update ends.
  const Pass *passes = this->passes_, *end = passes + this->pass_count_;
  if (std::find(passes, end, Pass::DOSE) != end || std::find(passes, end, Pass::UNDOSE) != end)
    this->fast_loop_.start();
}

void CrowPanelBase::plan_grey_passes_() {
  if (!this->seven_) {
    this->passes_[this->pass_count_++] = Pass::GREY;
  } else {
    this->passes_[this->pass_count_++] = Pass::HALF_TO_BLACK;
    this->passes_[this->pass_count_++] = Pass::EVEN_GREY;
    this->passes_[this->pass_count_++] = Pass::HALF_TO_WHITE;
  }
}

// Sends the current pass's planes: which ones and which rows, the driver decides. A partial sends
// only the new frame: the old plane, and the new one where nothing changed, already hold what is
// on the glass. Cancelling and settling send which pixels are out of balance (old) and what the
// glass shows (new).
bool CrowPanelBase::transfer_data() {
  if (!this->pass_started_) {
    if (this->pass_index_ == 0)
      this->plan_update_();
    if (this->unchanged_)
      return true;
    this->pass_started_ = true;
    const Pass pass = this->passes_[this->pass_index_];
    if ((pass == Pass::DOSE || pass == Pass::UNDOSE) && !this->ticking_)
      this->build_masks_(pass, this->scan_frames_[this->pass_index_]);
    this->start_pass_(pass);
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
    case Pass::DOSE:
    case Pass::UNDOSE:
      return this->send_own_planes_(this->passes_[this->pass_index_]);
  }
  return true;
}

// Runs once a pass has finished. With passes left, loop() starts the next. After the last, the
// glass's reading is brought up to date, fast partials' pushes are recorded or cleared, and, if the
// driver asks, the reading goes to both planes for the next partial to compare with; the base moves
// straight on, so loop() writes them, in slices.
void CrowPanelBase::power_off() {
  this->pass_finished_();
  this->pass_started_ = false;
  if (this->pass_index_ + 1 < this->pass_count_) {
    this->pass_index_++;
    this->next_pass_ = true;
    return;
  }
  const Pass last = this->passes_[this->pass_index_];
  this->pass_index_ = 0;
  // Live updates do not count towards a full refresh, so none interrupts an animation.
  if (this->live_update_)
    this->update_count_ = (this->update_count_ + this->full_update_every_ - 1) % this->full_update_every_;
  if (this->unchanged_ || this->ticking_)
    return;  // a tick recorded itself as it planned
  const Pass *passes = this->passes_, *end = passes + this->pass_count_;
  this->record_glass_(last == Pass::FAST, std::find(passes, end, Pass::CORRECT) != end);
  // A scan leaves masks in the planes; the reading goes back when something else needs it.
  this->copying_ = last != Pass::DOSE && this->copies_after_(last);
  if (this->copying_)
    this->stale_ = EMPTY_WINDOW;
  this->record_drive_(last);
  if (last == Pass::DOSE) {
    this->drew_window_();  // a glide drew the frame near enough
  } else if ((last == Pass::GREY || last == Pass::HALF_TO_WHITE) && this->shown_ != nullptr) {
    memcpy(this->shown_, this->pixels_, this->buffer_length_);
    this->shown_valid_ = true;
    this->dirty_ = EMPTY_WINDOW;
    this->rows_drawn_(0, this->height_ - 1);
  }
  this->update_finished_(last);
}

// The new frame's black and white reading becomes the glass's, row by row: its hash for the next
// partial, and with the bit planes, which pixels a fast partial has pushed out of balance (each
// change flips its pixel) or that cancelling has put right.
void CrowPanelBase::record_glass_(bool fast, bool corrected) {
  this->record_glass_rows_(0, this->height_, fast, corrected);
}
void CROWPANEL_FAST CrowPanelBase::record_glass_rows_(uint16_t first, uint16_t end, bool fast, bool corrected) {
  uint8_t row[MAX_PLANE_ROW];
  for (size_t y = first; y != end; y++) {
    this->plane_bytes_(WHITE_SOURCE, y, 0, this->plane_row_, row);
    glass_rows[y] = row_hash(row, this->plane_row_);
    if (this->glass_ == nullptr)
      continue;
    uint8_t *glass = this->glass_ + y * this->plane_row_;
    uint8_t *unsettled = this->unsettled_ + y * this->plane_row_;
    for (size_t i = 0; i != this->plane_row_; i++) {
      if (corrected && this->corrects_(y, i))
        unsettled[i] = 0;
      if (fast && glass[i] != row[i])
        this->reading_changed_(y, i);
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
    for (size_t i = 0; i != this->bit_plane_; i++)
      this->unsettled_count_ += __builtin_popcount(this->unsettled_[i]);
  }
  if (this->unsettled_count_ != 0) {
    ESP_LOGV(TAG, "%u pixels a push out of balance", (unsigned) this->unsettled_count_);
  }
}

// The base runs one step of the update a loop() call, and ESPHome calls loop() about every 16 ms,
// so the steps run back to back here instead, until one has to wait (BUSY, a delay) or the
// budget is spent.
void CrowPanelBase::loop() {
  const uint32_t start = millis();
  if (this->state_ != epaper_spi::EPaperState::IDLE && start - this->started_ > UPDATE_DEADLINE_MS) {
    this->give_up_();
    return;
  }
  while (millis() - start < STEP_BUDGET_MS) {
    if (this->copying_) {
      if (!this->send_planes_(WHITE_SOURCE, WHITE_SOURCE))
        continue;
      this->copying_ = false;
    }
    // Each pass is an update to the base, which counts it, so the count goes back a step between
    // passes and they count as one.
    if (this->next_pass_ && this->state_ == epaper_spi::EPaperState::DEEP_SLEEP) {
      this->next_pass_ = false;
      this->update_count_ = (this->update_count_ + this->full_update_every_ - 1) % this->full_update_every_;
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

// The panel has not finished an update in UPDATE_DEADLINE_MS: a loose ribbon or a failed
// controller. Stop waiting, reset the controllers so the supplies are off and the device can
// sleep, and start again from a full refresh.
void CrowPanelBase::give_up_() {
  ESP_LOGE(TAG, "The panel did not finish an update in %u s; resetting it", (unsigned) (UPDATE_DEADLINE_MS / 1000));
  this->status_set_warning();
  this->reset_controllers_();
  this->copying_ = false;
  this->sending_ = false;
  this->plane_ = 0;
  this->pass_index_ = 0;
  this->pass_started_ = false;
  this->next_pass_ = false;
  this->ticking_ = false;
  this->prerendered_ = false;
  this->planned_ahead_ = false;
  this->update_waiting_ = false;
  this->moving_ = EMPTY_WINDOW;
  this->fast_loop_.stop();
  this->forget_();
  this->glass_valid_ = false;
  glass_rows_valid = false;
  if (this->unsettled_ != nullptr)
    memset(this->unsettled_, 0, this->bit_plane_);
  this->unsettled_count_ = 0;
  this->update_count_ = 0;
  this->set_state_(epaper_spi::EPaperState::IDLE);
}

void CrowPanelBase::wait_idle_(uint32_t timeout_ms) {
  const uint32_t start = millis();
  delay(2);  // BUSY changes a moment after the command
  while (!this->is_idle_() && millis() - start < timeout_ms)
    delay(1);
}

}  // namespace esphome::crowpanel_epaper
