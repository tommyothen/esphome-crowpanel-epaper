#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esphome/components/epaper_spi/epaper_spi.h"
#include "esphome/core/defines.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

namespace esphome::crowpanel_epaper {

// ESPHome builds for size; the loops a tick runs over every pixel of its window, and drawing, are
// built for speed instead, which more than halves their time.
#define CROWPANEL_FAST __attribute__((optimize("O2")))

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
  // While a tick runs, an update waits for it, and the latest frame wins.
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
  // Partial updates walk each changed pixel from its grey to its new one in a few short pushes,
  // without flashing, rather than pushing it to black or white. The greys land near enough; the
  // next grey refresh settles them exactly and cancels the pushes' drive. Needs grey.
  void set_glide(bool glide) { this->glide_ = glide; }
  // Partial updates as ticks, for things that move: each tick pushes every pixel a few frames
  // towards the frame just drawn, if that brings it nearer, and the driver draws and ticks on by
  // itself while anything is still on its way or the picture keeps changing, drawing the next
  // frame while the panel runs the last. Greys land as near as a glide's. Needs grey.
  void set_live(bool live);

  // A window of the panel: rows down, and across in plane bytes, 8 pixels each.
  struct Window {
    uint8_t first_bank, last_bank;
    uint16_t first_row, last_row;
  };
  static constexpr Window EMPTY_WINDOW{MAX_PLANE_ROW, 0, MAX_HEIGHT, 0};
  static bool is_empty(const Window &window) { return window.first_row > window.last_row; }
  static void merge(Window &into, const Window &window);

  // A panel's glides and ticks, measured on it: for each level (row) to each other (column), the
  // frames at its scan rate a glide pushes, + black and - white; the frames a tick pushes; and the
  // lightness (0 to 1) after so many frames of ticks, from a settled white pushed black and a
  // settled black pushed white.
  struct GlideTuning {
    int8_t glide[7][7];
    uint8_t tick_frames;
    float tick_black[11][2], tick_white[11][2];
  };

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
    DOSE,           // a glide's scan, or a tick, pushing each pixel part way to its new grey
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
  // Sends a scan's masks (masks_, in the window), in slices; true once they are all out.
  virtual bool send_own_planes_(Pass pass) { return true; }
  // The panel's glide and tick tuning, or null for none.
  virtual const GlideTuning *glide_tuning_() const { return nullptr; }
  // How much faster the ink moves now than where the tuning was measured.
  virtual float speed_() const { return 1; }
  // Writes the glass's reading back into both planes where scans left their masks (stale_).
  virtual void restore_reading_() {}
  // Whether the driver can settle a window exactly, in greys, without a full refresh.
  virtual bool settles_windows_() const { return false; }
  // Makes the next update settle glides' drive: a full refresh unless the driver settles windows.
  virtual void request_settle_() { this->request_full_update(); }
  // Rows first to last now hold, in shown_, the levels last drawn.
  virtual void rows_drawn_(uint16_t first, uint16_t last) {}
  virtual void pass_finished_() {}
  // After the last pass is recorded.
  virtual void update_finished_(Pass last) {}
  // A fast partial changed the black and white reading of plane byte i of row y.
  virtual void reading_changed_(size_t y, size_t i) {}
  // Whether the panel's RAM needs the glass's reading written to both planes after the last pass.
  virtual bool copies_after_(Pass last) const { return true; }
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
  void record_glass_rows_(uint16_t first, uint16_t end, bool fast, bool corrected);

  // Glides and ticks (glide.cpp).
  Window whole_window_() const {
    return {0, static_cast<uint8_t>(this->plane_row_ - 1), 0, static_cast<uint16_t>(this->height_ - 1)};
  }
  uint8_t level_at_(const uint8_t *buffer, size_t x, size_t y) const {
    const uint8_t pair = buffer[y * this->row_width_ + x / 2];
    return (x & 1) ? pair & 0x0F : pair >> 4;
  }
  bool find_changed_window_();
  bool glide_buffers_();
  void set_doses_();
  bool plan_glide_();
  bool plan_tick_();
  int compute_tick_();
  void plan_scans_(Pass pass, uint8_t bits);
  uint8_t drive_bits_() const;
  void clear_masks_();
  void build_masks_(Pass pass, uint8_t frames);
  void record_drive_(Pass last);
  void drew_window_();
  void drew_(const Window &changed);
  void estimate_levels_();
  void build_tick_tables_();
  int decide_(uint8_t now, uint8_t to, int drive) const;
  // Called by a driver once a tick's refresh is running: draws the next frame meanwhile.
  void prerender_();
  // Called by a driver at the end of an update: whether ticking goes on, and if so arranges it.
  bool continue_live_();
  void settle_glides_now_();
  // What a tick's pass over rows first to end of its window found.
  struct TickRows {
    uint16_t first, end;
    Window moving;
    uint32_t blacks, whites, held;
    int32_t driven;  // the change in the count of pixels with drive
  };
  void tick_rows_(TickRows &rows, const Window &window);
  // A task on the other core that takes half of each tick's rows.
  static void tick_helper_(void *panel);

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
  // The levels last drawn, for grey partials, glides and ticks, and where the glass may differ from
  // them: black and white partials left a reading there, or glides and ticks their near enough.
  uint8_t *shown_{nullptr};
  bool shown_valid_{false};
  Window window_{};                   // this update's window: grey partial, glide or tick
  Window dirty_{EMPTY_WINDOW};
  // Glides and ticks. Each pixel's net drive since its last balanced refresh, in frames at the scan
  // rate, + towards black; this glide's dose for each pixel in its window; a scan's masks, OLD then
  // NEW, a bit a pixel each, holding (both white) everywhere outside the window.
  bool glide_{false};
  bool live_{false};
  int8_t *drive_{nullptr};
  uint32_t driven_count_{0};
  int8_t doses_[7][7]{};
  int8_t *push_{nullptr};
  uint8_t scan_frames_[16]{};         // each DOSE or UNDOSE pass's frames
  uint8_t *masks_{nullptr};
  Window masked_{EMPTY_WINDOW};       // where masks_ holds anything but holding
  Window stale_{EMPTY_WINDOW};        // where the panel's planes hold masks, not the reading
  // Ticks: each pixel's lightness as modelled, 0 black to 255 white; which 8-pixel banks are still
  // on their way, and their window; where a tick takes a lightness, and which way each level wants.
  uint8_t *estimate_{nullptr};
  bool estimate_valid_{false};
  uint8_t *moving_banks_{nullptr};
  Window moving_{EMPTY_WINDOW};
  bool ticking_{false};               // this update is a tick, planned and recorded already
  bool prerendered_{false};           // the next frame is drawn already
  bool planned_ahead_{false};         // and its tick planned and recorded: it must run next
  bool ahead_pushes_{false};          // that tick pushes anything
  bool live_update_{false};           // this update is live, not counted towards a full refresh
  bool update_waiting_{false};        // an update was asked for during a tick
  bool tick_tables_ready_{false};
  uint8_t tick_frames_{4};            // the panel's tick, from its tuning
  bool glide_overflowed_{false};      // the last glide planned would have passed a drive's limit
  bool settle_requested_{false};      // the next partial settles exactly, where the driver can
  uint8_t tick_black_[256]{}, tick_white_[256]{};
  int8_t tick_push_[7][256]{};
  HighFrequencyLoopRequester fast_loop_;  // the main loop unpaced while ticking
  TaskHandle_t helper_{nullptr};
  SemaphoreHandle_t helper_start_{nullptr}, helper_done_{nullptr};
  TickRows helper_rows_{};
  const Window *helper_window_{nullptr};
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
