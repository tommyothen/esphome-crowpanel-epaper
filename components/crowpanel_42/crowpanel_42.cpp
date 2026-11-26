#include "crowpanel_42.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_sleep.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::crowpanel_42 {

static const char *const TAG = "crowpanel_42";

// The supplies stay on until ACTIVE_MS passes without an update, or for at most MAX_ACTIVE_MS at a
// stretch however often updates come. Switching them on takes about 80 ms and off about 40 ms.
static constexpr uint32_t ACTIVE_MS = 5000;
static constexpr uint32_t MAX_ACTIVE_MS = 60000;

// Frame rates (0x30): 5 Hz steps from 5 Hz at 0x00 to 120 Hz at 0x17, then 10 Hz steps to 200 Hz.
static constexpr uint8_t FRAMES_50_HZ = 0x09;
static constexpr float MAX_HZ = 200;
// This panel's waveforms were tuned at 11 °C: the self-balanced ones at 50 Hz, the seven-grey
// half steps at 150 Hz.
static constexpr float TUNED_CELSIUS = 11, TUNED_HZ = 50, TUNED_HALF_HZ = 150;
// The fast greys were tuned there at 100 Hz.
static constexpr float TUNED_FAST_HZ = 100;

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

// Fast greys: two shakes and one push, not Waveshare's many. Every pixel is shaken, pushed to black
// and back to white for `shake` frames, `shakes` times; then white is pushed to black for `reset`
// frames and back to white for as many, black the other way round, and a grey to black, to white,
// and to black again for its last few frames, which set where it lands. Every table runs as long,
// and VCOM with them. At 100 Hz four greys take 1.8 s against the slow ones' 3.5. Drawn over black,
// dark, light and white, each grey varied by up to 0.09, as the slow greys do; without the shakes
// the single push took 1 s but varied by up to 0.23, and one 20-frame shake by up to 0.12. After a
// 40-frame push to white, 5 frames of black land at 0.68 and 9 at 0.33.
struct FastGreyWaveform {
  uint8_t bytes[WAVEFORM];
  constexpr FastGreyWaveform(uint8_t shake, uint8_t shakes, uint8_t reset, uint8_t light, uint8_t dark) : bytes() {
    constexpr uint8_t BLACK = 0x40, WHITE = 0x80;
    const uint8_t phases[5][3] = {
        {reset, reset, 0},                                                                       // VCOM
        {static_cast<uint8_t>(BLACK | reset), static_cast<uint8_t>(WHITE | reset), 0},          // WW: white
        {static_cast<uint8_t>(BLACK | (reset - light)), static_cast<uint8_t>(WHITE | reset),
         static_cast<uint8_t>(BLACK | light)},                                                  // BW: light
        {static_cast<uint8_t>(BLACK | (reset - dark)), static_cast<uint8_t>(WHITE | reset),
         static_cast<uint8_t>(BLACK | dark)},                                                   // WB: dark
        {static_cast<uint8_t>(WHITE | reset), static_cast<uint8_t>(BLACK | reset), 0},          // BB: black
    };
    for (int t = 0; t < 5; t++) {
      uint8_t *shaking = bytes + t * TABLE, *push = shaking + 7;
      shaking[0] = shakes;
      shaking[1] = (t == 0 ? 0 : BLACK) | shake;
      shaking[2] = (t == 0 ? 0 : WHITE) | shake;
      shaking[5] = 1;
      shaking[6] = 1;
      push[0] = 1;
      for (int p = 0; p < 3; p++)
        push[1 + p] = phases[t][p];
      push[5] = 1;
      push[6] = 1;
    }
  }
};
static constexpr uint8_t FAST_SHAKE = 20, FAST_SHAKES = 2, FAST_FOUR_RESET = 50, FAST_EVEN_RESET = 40,
                         FAST_LIGHT = 5, FAST_DARK_THIRD = 9, FAST_DARK_SIXTH = 11;
// Four greys, with a 50-frame push: black, dark grey at 0.28 to 0.37, light at 0.66 to 0.74, white.
// The seven-grey middle pass, a 40-frame push and dark grey at a sixth, with its half steps of 3, 5
// and 3 frames at 150 Hz: 0.02, 0.17, 0.31, 0.49, 0.67, 0.81 and 1.02.
static constexpr FastGreyWaveform FAST_FOUR{FAST_SHAKE, FAST_SHAKES, FAST_FOUR_RESET, FAST_LIGHT, FAST_DARK_THIRD};
static constexpr FastGreyWaveform FAST_EVEN{FAST_SHAKE, FAST_SHAKES, FAST_EVEN_RESET, FAST_LIGHT, FAST_DARK_SIXTH};

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
static constexpr uint8_t FAST_HALF_LENGTHS[3] = {3, 5, 3};  // after the fast middle pass
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

// Glides. A pixel pushed one way for a few frames moves part of the way, and how far hangs on where
// it starts. At 12 °C, from a settled white, 0, 2, 4, 6, 8, 12, 16, 20, 28 and 40 frames of black at
// 200 Hz, given as refreshes of 1, 2, 4, 8, 16 and 32 frames, left 0.99, 0.98, 0.90, 0.80, 0.64,
// 0.47, 0.31, 0.24, 0.12 and 0.05; from a settled black, as much white left 0.02, 0.04, 0.06, 0.11,
// 0.17, 0.31, 0.48, 0.67, 0.87 and 0.96. Forty one-frame refreshes managed only 0.42 and 0.82: a
// pixel loses its way between refreshes, so the frames go in as few as their binary digits. Read
// backwards, each level sits so far along each curve, and a glide from one level to another starts
// as the difference, the ends 48, past where both curves flatten; then tuned on the camera against
// an exact refresh of the same picture. Frames at 200 Hz, from level (row) to level (column), +
// black and - white.
static constexpr int8_t GLIDE[7][7] = {
    {0, -5, -11, -17, -20, -25, -48},
    {23, 0, -3, -6, -10, -17, -40},
    {33, 13, 0, -2, -5, -12, -36},
    {37, 17, 8, 0, -3, -9, -32},
    {40, 21, 13, 7, 0, -5, -28},
    {43, 23, 15, 10, 5, 0, -22},
    {48, 24, 16, 12, 8, 5, 0},
};
static constexpr uint8_t FRAMES_200_HZ = 0x1F;
// A glide's drive record holds up to this many frames either way; a glide that would take a pixel
// further settles its window in greys instead.
static constexpr int MAX_DRIVE = 127;

// One of a glide's scans, or a scan cancelling glides: with OLD and NEW as masks, black on BW and
// white on WB for `frames` frames (two phases past the 63 a phase holds), WW and BB holding.
static SimpleWaveform scan_waveform(uint8_t frames) {
  const uint8_t a = frames > 63 ? frames / 2 : frames, b = static_cast<uint8_t>(frames - a);
  return SimpleWaveform{{a, b, 0}, {{0}, {0}, {1, 1}, {2, 2}, {0}}};
}

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
static_assert(balanced({FAST_FOUR.bytes}) && balanced({FAST_EVEN.bytes}) && vcom_lasts(FAST_FOUR.bytes) &&
                  vcom_lasts(FAST_EVEN.bytes),
              "the fast greys must leave no net drive on a pixel, and VCOM last as long");
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

// With grey partials, a hash of each row's levels as last drawn, so that after a wake, when the
// copy of the last frame has gone with the PSRAM, a grey partial still knows which rows changed.
// Trusted when shown_rows_valid says the last update finished.
static RTC_DATA_ATTR uint32_t shown_rows[CrowPanel42::HEIGHT];
static RTC_DATA_ATTR bool shown_rows_valid;
// What black and white partials had changed since the last frame drawn in greys, kept through
// deep sleep for the next grey partial.
static RTC_DATA_ATTR CrowPanel42::Window slept_dirty;

// A window is empty when its first row is past its last.
static constexpr CrowPanel42::Window EMPTY{CrowPanel42::PLANE_ROW, 0, CrowPanel42::HEIGHT, 0};
static bool is_empty(const CrowPanel42::Window &window) { return window.first_row > window.last_row; }
static void merge(CrowPanel42::Window &into, const CrowPanel42::Window &window) {
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

// The reader for the panel's temperature, and with grey, the copy of the last frame drawn in greys.
void CrowPanel42::setup_controllers_() {
  this->add_reader_();
  if (this->grey_ && this->pixels_ != nullptr) {
    RAMAllocator<uint8_t> allocator;
    this->shown_ = allocator.allocate(this->buffer_length_);
    if (this->glide_) {
      this->drive_ = reinterpret_cast<int8_t *>(allocator.allocate(WIDTH * HEIGHT));
      this->push_ = reinterpret_cast<int8_t *>(allocator.allocate(WIDTH * HEIGHT));
      this->masks_ = allocator.allocate(2 * BIT_PLANE);
      if (this->drive_ != nullptr)
        memset(this->drive_, 0, WIDTH * HEIGHT);
    }
  }
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    shown_rows_valid = false;
  } else {
    this->dirty_ = slept_dirty;
  }
}

// Before a deep sleep the supplies go off, after any refresh still running; the controller stays
// in standby, keeping its RAM. An update cut short leaves the row record untrusted.
void CrowPanel42::prepare_for_sleep_(bool settled) {
  // Glides' drive lives in PSRAM, which the sleep loses, so it is cancelled now, by a grey refresh
  // of everything glides have touched.
  if (settled && this->driven_count_ != 0) {
    ESP_LOGD(TAG, "Settling %u glided pixels before sleeping", (unsigned) this->driven_count_);
    this->request_grey_partial();
    this->update();
    const uint32_t start = millis();
    while ((this->state_ != epaper_spi::EPaperState::IDLE || this->copying_) && millis() - start < 20000) {
      this->loop();
      App.feed_wdt();
      delay(1);
    }
  }
  shown_rows_valid = shown_rows_valid && settled;
  slept_dirty = this->dirty_;
  if (this->supplies_on_) {
    this->wait_idle_(3000);
    this->switch_supplies_off_();
    this->wait_idle_(500);
  }
}

// With grey partials, or when asked, a partial is a grey refresh of the window around what changed
// since the last frame drawn in greys: exactly, with that frame, or after a wake, the band of rows
// whose levels changed; and around whatever black and white refreshes have touched since, which is
// all of it when the last full refresh was one.
bool CrowPanel42::plan_own_update_() {
  // A full refresh cancels every glide's drive first, and the base plans the rest.
  if (this->update_count_ == 0) {
    if (this->driven_count_ != 0) {
      this->window_ = {0, static_cast<uint8_t>(PLANE_ROW - 1), 0, static_cast<uint16_t>(HEIGHT - 1)};
      this->plan_scans_(Pass::UNDOSE, this->drive_bits_());
    }
    return false;
  }
  if (this->plan_glide_())
    return true;
  const bool grey_partial = this->grey_ && this->pixels_ != nullptr &&
                            (this->grey_partials_ || this->glide_ || this->grey_requested_) &&
                            (this->shown_valid_ || shown_rows_valid || !is_empty(this->dirty_));
  this->grey_requested_ = false;
  if (!grey_partial)
    return false;
  const bool changed =
      this->shown_valid_ ? this->find_changed_window_() : shown_rows_valid && this->find_changed_band_();
  if (!changed && is_empty(this->dirty_)) {
    this->unchanged_ = true;
    return true;
  }
  if (!changed)
    this->window_ = this->dirty_;
  merge(this->window_, this->dirty_);
  this->windowed_ = true;
  if (this->driven_count_ != 0)
    this->plan_scans_(Pass::UNDOSE, this->drive_bits_());
  if (this->unsettled_in_window_())
    this->passes_[this->pass_count_++] = Pass::CORRECT;
  this->plan_grey_passes_();
  ESP_LOGV(TAG, "Grey refresh of x %u to %u, y %u to %u", (unsigned) this->window_.first_bank * 8,
           (unsigned) this->window_.last_bank * 8 + 7, (unsigned) this->window_.first_row,
           (unsigned) this->window_.last_row);
  return true;
}

// A partial sends only NEW: OLD already holds what is on the glass. Every other pass sends both,
// and leaves the row record untrusted until the update has finished; a black and white partial
// leaves the last frame drawn in greys as it was, and so the record.
void CrowPanel42::start_pass_(Pass pass) {
  const bool partial = pass == Pass::FAST || pass == Pass::PARTIAL;
  this->plane_ = partial ? 1 : 0;
  if (!partial)
    shown_rows_valid = false;
  // A scan sends only its window, in partial mode, where writes fill the window.
  if (pass == Pass::DOSE || pass == Pass::UNDOSE) {
    this->build_masks_(pass, this->scan_frames_[this->pass_index_]);
    this->send_window_();
  }
}

// Out of partial mode before any plane is written: in it, writes wrap within the window.
void CrowPanel42::pass_finished_() {
  const Pass pass = this->passes_[this->pass_index_];
  if (this->windowed_ || pass == Pass::DOSE || pass == Pass::UNDOSE)
    this->command(0x92);
}

// The last frame drawn in greys, and the hash of each of its rows for after a wake, stay as they
// were through black and white refreshes, which instead grow the window the next grey partial
// covers: a fast partial by the bytes whose reading changed, a balanced one, from cold or after a
// wake without the glass's reading, by the rows it sent, and a full refresh by the whole panel.
void CrowPanel42::update_finished_(Pass last) {
  this->record_drive_(last);
  if (last == Pass::DOSE) {
    // The glide drew the frame near enough: the next glide starts from it, and the next grey
    // partial settles its window.
    memcpy(this->shown_, this->pixels_, this->buffer_length_);
    for (size_t y = 0; y != HEIGHT; y++)
      shown_rows[y] = this->level_row_hash_(y);
    shown_rows_valid = true;
    merge(this->dirty_, this->window_);
  } else if (last == Pass::GREY || last == Pass::HALF_TO_WHITE) {
    this->dirty_ = EMPTY;
    if (this->shown_ != nullptr) {
      memcpy(this->shown_, this->pixels_, this->buffer_length_);
      this->shown_valid_ = true;
    }
    for (size_t y = 0; y != HEIGHT; y++)
      shown_rows[y] = this->level_row_hash_(y);
    shown_rows_valid = true;
  } else if (last == Pass::BLACK_WHITE) {
    this->dirty_ = {0, static_cast<uint8_t>(PLANE_ROW - 1), 0, static_cast<uint16_t>(HEIGHT - 1)};
  } else if (last == Pass::PARTIAL) {
    merge(this->dirty_, {0, static_cast<uint8_t>(PLANE_ROW - 1), this->first_row_,
                         static_cast<uint16_t>(this->end_row_ - 1)});
  }
}

void CrowPanel42::reading_changed_(size_t y, size_t i) {
  merge(this->dirty_, {static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint16_t>(y), static_cast<uint16_t>(y)});
}

void CrowPanel42::forget_() {
  this->shown_valid_ = false;
  this->windowed_ = false;
  this->dirty_ = EMPTY;
  shown_rows_valid = false;
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
        this->plane_bytes_(source, this->row_ + r, 0, PLANE_ROW, data + r * PLANE_ROW);
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
  const float speed = frame_rate_hz(this->frame_rate_) / TUNED_HZ;
  // The fast greys likewise; past 200 Hz they run at 200 Hz with their frames scaled to match, built
  // here in the same balanced shape.
  if (this->fast_greys_) {
    const float fast_hz = TUNED_FAST_HZ * speed;
    const float fast_scale = fast_hz > MAX_HZ ? MAX_HZ / fast_hz : 1;
    this->fast_rate_ = frame_rate_code(fast_hz * fast_scale);
    const auto frames = [fast_scale](uint8_t n) { return static_cast<uint8_t>(std::max<long>(1, std::lround(n * fast_scale))); };
    memcpy(this->fast_four_,
           FastGreyWaveform(frames(FAST_SHAKE), FAST_SHAKES, frames(FAST_FOUR_RESET), frames(FAST_LIGHT),
                            frames(FAST_DARK_THIRD))
               .bytes,
           WAVEFORM);
    memcpy(this->fast_even_,
           FastGreyWaveform(frames(FAST_SHAKE), FAST_SHAKES, frames(FAST_EVEN_RESET), frames(FAST_LIGHT),
                            frames(FAST_DARK_SIXTH))
               .bytes,
           WAVEFORM);
  }
  // The half steps keep their length against the middle pass's frames: at its rate, faster, or
  // past 200 Hz at 200 Hz with fewer frames. Both are built from the same lengths, so they cancel.
  const uint8_t *half_lengths = this->fast_greys_ ? FAST_HALF_LENGTHS : HALF_LENGTHS;
  const float half_hz = TUNED_HALF_HZ * speed;
  const float scale = half_hz > MAX_HZ ? MAX_HZ / half_hz : 1;
  this->half_rate_ = frame_rate_code(half_hz * scale);
  uint8_t lengths[3];
  for (int i = 0; i < 3; i++)
    lengths[i] = std::max<long>(1, std::lround(half_lengths[i] * scale));
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

// FNV-1a over row y's levels.
uint32_t CrowPanel42::level_row_hash_(size_t y) const {
  const uint8_t *row = this->pixels_ + y * this->row_width_;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i != this->row_width_; i++)
    hash = (hash ^ row[i]) * 16777619u;
  return hash;
}

// After a wake, without the last frame drawn: the band of rows whose levels differ from those last
// drawn, across the whole width. False when no row differs.
bool CrowPanel42::find_changed_band_() {
  int first_row = -1, last_row = -1;
  for (size_t y = 0; y != HEIGHT; y++) {
    if (this->level_row_hash_(y) != shown_rows[y]) {
      if (first_row < 0)
        first_row = y;
      last_row = y;
    }
  }
  if (first_row < 0)
    return false;
  this->window_ = {0, static_cast<uint8_t>(PLANE_ROW - 1), static_cast<uint16_t>(first_row),
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
      if (this->fast_greys_) {
        this->load_waveform_(this->fast_four_, this->fast_rate_);
      } else {
        this->load_waveform_(FOUR_GREY.bytes, this->frame_rate_);
      }
      break;
    case Pass::HALF_TO_BLACK:
      this->load_waveform_(this->half_to_black_, this->half_rate_);
      break;
    case Pass::EVEN_GREY:
      if (this->fast_greys_) {
        this->load_waveform_(this->fast_even_, this->fast_rate_);
      } else {
        this->load_waveform_(EVEN_GREY.bytes, this->frame_rate_);
      }
      break;
    case Pass::HALF_TO_WHITE:
      this->load_waveform_(this->half_to_white_, this->half_rate_);
      break;
    // A glide's scans and their cancelling, always at 200 Hz, so a frame's drive matches whenever
    // it is cancelled. They are already in partial mode.
    case Pass::DOSE:
    case Pass::UNDOSE:
      this->load_waveform_(scan_waveform(this->scan_frames_[this->pass_index_]).bytes, FRAMES_200_HZ);
      this->command(0x12);
      this->next_delay_ = 2;
      return;
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

// A hardware reset, which switches the supplies off and the registers back to their defaults but
// keeps the RAM.
void CrowPanel42::reset_controllers_() {
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
    this->reset_controllers_();
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

// A pixel's level in a buffer of 4 bits a pixel, the first of each pair in the high nibble.
static uint8_t level_at(const uint8_t *buffer, size_t x, size_t y) {
  const uint8_t pair = buffer[y * (CrowPanel42::WIDTH / 2) + x / 2];
  return (x & 1) ? pair & 0x0F : pair >> 4;
}

// This update's glide from each level to each other, in frames at 200 Hz: the curves' frames,
// fewer in the warm and more in the cold, as the other waveforms' frame rates go.
void CrowPanel42::set_doses_() {
  float celsius = this->panel_celsius_;
#ifdef USE_SENSOR
  if (this->temperature_ != nullptr && !std::isnan(this->temperature_->state))
    celsius = this->temperature_->state;
#endif
  const float speed = std::isnan(celsius) ? 1 : refresh_seconds(TUNED_CELSIUS) / refresh_seconds(celsius);
  for (int from = 0; from < 7; from++) {
    for (int to = 0; to < 7; to++)
      this->doses_[from][to] = static_cast<int8_t>(std::clamp<long>(std::lround(GLIDE[from][to] / speed), -63, 63));
  }
}

// A glide, when the driver knows every pixel's level and nobody asked for exact greys: a scan for
// each binary digit the doses in the window around what changed need. Not when a pixel's drive
// would pass what its record holds; the grey partial settles the window instead.
bool CrowPanel42::plan_glide_() {
  if (!this->glide_ || this->grey_requested_ || !this->shown_valid_ || this->drive_ == nullptr ||
      this->push_ == nullptr || this->masks_ == nullptr)
    return false;
  if (!this->find_changed_window_()) {
    this->unchanged_ = true;
    return true;
  }
  this->set_doses_();
  uint8_t bits = 0;
  for (size_t y = this->window_.first_row; y <= this->window_.last_row; y++) {
    for (size_t x = this->window_.first_bank * 8; x != (this->window_.last_bank + 1) * 8u; x++) {
      const int dose = this->doses_[level_at(this->shown_, x, y)][level_at(this->pixels_, x, y)];
      if (std::abs(this->drive_[y * WIDTH + x] + dose) > MAX_DRIVE)
        return false;
      this->push_[y * WIDTH + x] = dose;
      bits |= std::abs(dose);
    }
  }
  if (bits == 0)
    return false;
  this->windowed_ = true;
  this->plan_scans_(Pass::DOSE, bits);
  ESP_LOGV(TAG, "Glide of x %u to %u, y %u to %u", (unsigned) this->window_.first_bank * 8,
           (unsigned) this->window_.last_bank * 8 + 7, (unsigned) this->window_.first_row,
           (unsigned) this->window_.last_row);
  return true;
}

// A pass for each binary digit of `bits`, the shortest first.
void CrowPanel42::plan_scans_(Pass pass, uint8_t bits) {
  for (int k = 0; k < 8; k++) {
    if ((bits >> k) & 1) {
      this->passes_[this->pass_count_] = pass;
      this->scan_frames_[this->pass_count_] = 1 << k;
      this->pass_count_++;
    }
  }
}

// The binary digits of the drive on the window's pixels.
uint8_t CrowPanel42::drive_bits_() const {
  uint8_t bits = 0;
  for (size_t y = this->window_.first_row; y <= this->window_.last_row; y++) {
    for (size_t x = this->window_.first_bank * 8; x != (this->window_.last_bank + 1) * 8u; x++)
      bits |= std::abs(this->drive_[y * WIDTH + x]);
  }
  return bits;
}

// A scan's masks over the window: OLD white and NEW black (BW) pushes a pixel black, OLD black and
// NEW white (WB) pushes it white, and both white (WW) holds. A glide pushes each pixel by the bits
// of its dose; cancelling pushes it back by the bits of its drive.
void HOT CrowPanel42::build_masks_(Pass pass, uint8_t frames) {
  uint8_t *old_plane = this->masks_, *new_plane = this->masks_ + BIT_PLANE;
  const int8_t *pushes = pass == Pass::DOSE ? this->push_ : this->drive_;
  const int sign = pass == Pass::DOSE ? 1 : -1;
  for (size_t y = this->window_.first_row; y <= this->window_.last_row; y++) {
    const int8_t *row = pushes + y * WIDTH;
    for (size_t bank = this->window_.first_bank; bank <= this->window_.last_bank; bank++) {
      uint8_t old_bits = 0xFF, new_bits = 0xFF;
      for (size_t j = 0; j != 8; j++) {
        const int push = sign * row[bank * 8 + j];  // + black, - white
        if ((std::abs(push) & frames) == 0)
          continue;
        const uint8_t bit = 0x80 >> j;
        if (push > 0) {
          new_bits &= ~bit;
        } else {
          old_bits &= ~bit;
        }
      }
      old_plane[y * PLANE_ROW + bank] = old_bits;
      new_plane[y * PLANE_ROW + bank] = new_bits;
    }
  }
}

// Into partial mode on the window, gates scanning the whole panel as for grey partials.
void CrowPanel42::send_window_() {
  const uint16_t x0 = this->window_.first_bank * 8, x1 = this->window_.last_bank * 8 + 7;
  const uint16_t y0 = this->window_.first_row, y1 = this->window_.last_row;
  this->command(0x91);
  this->cmd_data(0x90, {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0 & 0xF8), static_cast<uint8_t>(x1 >> 8),
                        static_cast<uint8_t>(x1 & 0xFF), static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0 & 0xFF),
                        static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1 & 0xFF), 0x01});
}

// A scan's masks, the window's bytes of each, OLD then NEW, a 10 ms slice at a time.
bool HOT CrowPanel42::send_own_planes_(Pass pass) {
  const uint32_t start = millis();
  const size_t first = this->window_.first_bank, count = this->window_.last_bank - first + 1;
  while (this->plane_ != 2) {
    const bool new_plane = this->plane_ == 1;
    if (!this->sending_) {
      this->command(new_plane ? 0x13 : 0x10);
      this->row_ = this->window_.first_row;
      this->sending_ = true;
    }
    const uint8_t *plane = this->masks_ + (new_plane ? BIT_PLANE : 0);
    this->start_data_();
    while (this->row_ <= this->window_.last_row) {
      this->write_array(plane + this->row_ * PLANE_ROW + first, count);
      this->row_++;
      if (this->row_ <= this->window_.last_row && millis() - start > epaper_spi::MAX_TRANSFER_TIME) {
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

// After the last pass: a glide adds its doses to the window's drive, and cancelling clears it.
void CrowPanel42::record_drive_(Pass last) {
  if (this->drive_ == nullptr)
    return;
  const bool glide = last == Pass::DOSE, cancelled = this->passes_[0] == Pass::UNDOSE;
  if (!glide && !cancelled)
    return;
  for (size_t y = this->window_.first_row; y <= this->window_.last_row; y++) {
    for (size_t x = this->window_.first_bank * 8; x != (this->window_.last_bank + 1) * 8u; x++) {
      int8_t &drive = this->drive_[y * WIDTH + x];
      drive = glide ? drive + this->push_[y * WIDTH + x] : 0;
    }
  }
  this->driven_count_ = 0;
  for (size_t i = 0; i != WIDTH * HEIGHT; i++)
    this->driven_count_ += this->drive_[i] != 0;
  ESP_LOGV(TAG, "%u pixels driven by glides", (unsigned) this->driven_count_);
}

}  // namespace esphome::crowpanel_42
