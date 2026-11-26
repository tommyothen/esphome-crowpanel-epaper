#include "crowpanel_579.h"

#include <algorithm>
#include <cmath>
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
// The supplies stay on until ACTIVE_MS passes without an update, or for at most MAX_ACTIVE_MS at a
// stretch however often updates come; then they go off, and after CONTROLLER_IDLE_MS more the
// controllers sleep, so an update in between skips their reset.
static constexpr uint32_t ACTIVE_MS = 5000;
static constexpr uint32_t MAX_ACTIVE_MS = 60000;
static constexpr uint32_t CONTROLLER_IDLE_MS = 10000;

// Display update control (0x22) sequences. The full one switches the supplies on, reads the
// panel's temperature sensor, loads the matching OTP waveform, runs it and switches them off. The
// others run a waveform loaded with 0x32, switching the supplies on first if they are off and
// leaving them on; switching them on or off takes about 0.1 s each.
static constexpr uint8_t UPDATE_FULL = 0xF7;
// Frame rates (byte 224 of a waveform). Every waveform here is written for 50 Hz.
static constexpr uint8_t FRAMES_37_5_HZ = 0x09, FRAMES_50_HZ = 0x02, FRAMES_75_HZ = 0x03, FRAMES_100_HZ = 0x04,
                         FRAMES_125_HZ = 0x05;
// The fastest: timed with BUSY, a refresh costs 10.3 ms beyond its frames, 5.1 ms each, at 0x08;
// 0x00 to 0x08 run at about 15, 25, 49, 74, 99, 124, 148, 173 and 195 Hz, 0x09 to 0x0F at 37, 62, 86,
// 111, 135, 161 and 186.
static constexpr uint8_t FRAMES_195_HZ = 0x08;
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

// The cold partial refresh's waveform, for the first update after a boot or a wake, before the
// driver knows what the glass shows. Balanced on every change rather than only over time: a pixel
// turning black is pushed towards white for 25 frames, then towards black for 25, and one turning
// white the other way round. The first push only deepens the colour the pixel already shows, so
// nothing flashes. Pixels that keep their colour get no drive at all, so greys survive, where the
// panel's own partial waveform fades them. 25 frames rather than 15 clears black a grey refresh
// drew to 0.93 rather than 0.80 on the camera, for 0.4 s more on an update nobody is usually
// watching.
struct PartialWaveform {
  uint8_t bytes[233];
  constexpr PartialWaveform() : bytes() {
    constexpr uint8_t FRAMES = 25, NONE = FRAMES, BLACK = 0x40 | FRAMES, WHITE = 0x80 | FRAMES;
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
// driven less. The level a table lands on hangs on its last few frames: on the camera, the dark
// table with 15 frames of black drive lands at 0.46, and the same with 2 more frames of black at
// the very end at 0.12. Here dark grey is level 1 of the seven, 0.20: 15 frames, then 4 frames of
// black and 2 back to white at the end, balanced by a frame of white either side of the shake's
// white. Light grey is level 4, 0.72: 10 frames of black drive, balanced by 10 of white first.
struct EvenGreyWaveform {
  uint8_t bytes[233];
  constexpr EvenGreyWaveform() : bytes() {
    for (int i = 0; i < 233; i++)
      bytes[i] = GREY_LUT[i];
    constexpr int BW = 84, WB = 126;  // the light and dark tables
    // Phases A to D of groups 0 to 3, each table: VSS, VSH1 (0x40) or VSL (0x80), and the length.
    constexpr uint8_t dark[4][4] = {
        {0x0A, 0x1B, 0x8F, 0x03}, {0x4A, 0x81, 0x8A, 0x81}, {0x48, 0x83, 0x82, 0x04}, {0x04, 0x44, 0x82, 0x00}};
    constexpr uint8_t light[4][4] = {
        {0x8A, 0x1B, 0x0F, 0x03}, {0x4A, 0x01, 0x8A, 0x01}, {0x48, 0x03, 0x82, 0x84}, {0x04, 0x04, 0x02, 0x00}};
    for (int g = 0; g < 4; g++) {
      for (int p = 0; p < 4; p++) {
        bytes[WB + g * 7 + 1 + p] = dark[g][p];
        bytes[BW + g * 7 + 1 + p] = light[g][p];
      }
    }
  }
};
static constexpr EvenGreyWaveform EVEN_GREY{};

// The seven-grey refresh's outer passes: a half step, towards black before the middle pass and
// towards white after it, so the two cancel. The pixel's two RAM bits say which: level 2 (and
// white, which a push and a pull leave white) from dark grey (WB), level 3 from dark grey (BW),
// level 5 from light grey (WW), or for levels 0, 1 and 4, a one-frame trim (BB), towards white
// before and towards black after, which takes level 1 from 0.19 to 0.17 and light grey from 0.70
// to 0.64, and leaves black black. They run at 125 Hz, 8 ms a frame, since a single 20 ms frame
// moves a grey too far: at room temperature 2, 4 and 3 frames land levels 2, 3 and 5 at 0.30,
// 0.50 and 0.82. The middle pass runs faster in the warm and slower in the cold, so these scale
// with it, one set of lengths for each band; the trim stays a frame.
struct HalfStepWaveform {
  uint8_t bytes[233];
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
      bytes[t * 42] = 1;
      for (int p = 0; p < 3; p++)
        bytes[t * 42 + 1 + p] = (((phases_of_table[t] >> p) & 1) ? drive : 0) | lengths[p];
      bytes[t * 42 + 4] = TRIM;  // phase D: the trim, driven only in BB
      bytes[t * 42 + 5] = 1;
      bytes[t * 42 + 6] = 1;
    }
    bytes[4 * 42 + 4] = (white ? 0x40 : 0x80) | TRIM;  // BB: towards white before, black after
    bytes[224] = 0x05;  // 125 Hz
    for (int i = 227; i < 233; i++)
      bytes[i] = GREY_LUT[i];
  }
};
// For the temperature bands at 37.5, 50, 75 and 100 Hz: the room-temperature lengths scaled.
static constexpr HalfStepWaveform HALF_TO_BLACK[4] = {
    {false, 3, 5, 4}, {false, 2, 4, 3}, {false, 1, 3, 2}, {false, 1, 2, 2}};
static constexpr HalfStepWaveform HALF_TO_WHITE[4] = {
    {true, 3, 5, 4}, {true, 2, 4, 3}, {true, 1, 3, 2}, {true, 1, 2, 2}};

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
// One of a glide's scans, or a tick, or a scan cancelling them: with the planes as masks, black on
// BW and white on WB for `frames` frames (two phases past the 63 a phase holds), WW and BB holding.
static SimpleWaveform scan_waveform(uint8_t frames) {
  const uint8_t a = frames > 63 ? frames / 2 : frames, b = static_cast<uint8_t>(frames - a);
  return SimpleWaveform{{a, b, 0}, {{0}, {0}, {1, 1}, {2, 2}, {0}}};
}

// Glides and ticks, at 195 Hz. Not yet measured on this panel: the 4.2"'s, with a quarter more
// frames, as this panel's fast partial needs 15 frames at 50 Hz where the 4.2"'s needs 12.
static constexpr CrowPanel579::GlideTuning GLIDES{
    {
        {0, -6, -14, -21, -25, -31, -60},
        {29, 0, -4, -8, -13, -21, -50},
        {41, 16, 0, -3, -6, -15, -45},
        {46, 21, 10, 0, -4, -11, -40},
        {50, 26, 16, 9, 0, -6, -35},
        {54, 29, 19, 13, 6, 0, -28},
        {60, 30, 20, 15, 10, 6, 0},
    },
    {{0, .99f}, {5, .89f}, {10, .70f}, {15, .52f}, {20, .40f}, {25, .31f}, {30, .25f}, {40, .18f}, {50, .14f},
     {60, .11f}, {80, .08f}},
    {{0, .03f}, {5, .11f}, {10, .26f}, {15, .46f}, {20, .64f}, {25, .75f}, {30, .82f}, {40, .90f}, {50, .93f},
     {60, .96f}, {80, .98f}},
};

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
// Whether every table of a waveform runs to the same timing as VCOM's: the same repeat counts,
// phase lengths and state repeats. The datasheet gives each table its own, but every SSD1683
// waveform known keeps them equal, and net_drive counts each table by its own: a table timed
// differently could make the count wrong.
static constexpr bool same_timing(const uint8_t *waveform) {
  for (int t = 1; t < 5; t++) {
    for (int g = 0; g < 6; g++) {
      for (int i = 0; i < 7; i++) {
        const uint8_t mask = (i >= 1 && i <= 4) ? 0x3F : 0xFF;
        if ((waveform[t * 42 + g * 7 + i] & mask) != (waveform[g * 7 + i] & mask))
          return false;
      }
    }
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
static_assert(same_timing(PARTIAL.bytes) && same_timing(GREY_LUT) && same_timing(EVEN_GREY.bytes) &&
                  same_timing(HALF_TO_BLACK[0].bytes) && same_timing(HALF_TO_BLACK[1].bytes) &&
                  same_timing(HALF_TO_BLACK[2].bytes) && same_timing(HALF_TO_BLACK[3].bytes) &&
                  same_timing(HALF_TO_WHITE[0].bytes) && same_timing(HALF_TO_WHITE[1].bytes) &&
                  same_timing(HALF_TO_WHITE[2].bytes) && same_timing(HALF_TO_WHITE[3].bytes),
              "every table of a waveform must run to VCOM's timing");
static_assert(balanced({PARTIAL.bytes}), "the partial waveform must leave no net drive on a pixel");
static_assert(balanced({GREY_LUT}), "the grey waveform must leave no net drive on a pixel");
static_assert(balanced({EVEN_GREY.bytes}), "the even grey waveform must leave no net drive on a pixel");
static_assert(balanced({HALF_TO_BLACK[0].bytes, HALF_TO_WHITE[0].bytes}) &&
                  balanced({HALF_TO_BLACK[1].bytes, HALF_TO_WHITE[1].bytes}) &&
                  balanced({HALF_TO_BLACK[2].bytes, HALF_TO_WHITE[2].bytes}) &&
                  balanced({HALF_TO_BLACK[3].bytes, HALF_TO_WHITE[3].bytes}),
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
static_assert(same_timing(ONE_WAY.bytes) && same_timing(CORRECTION.bytes) && same_timing(SETTLE.bytes),
              "every table of a waveform must run to VCOM's timing");


// Controllers still awake from the last update need no reset or initialisation: every refresh
// loads its own waveform and settings, and every plane it reads is written for it.
// The wind-down stops here, once an update has started.
bool CrowPanel579::reset() {
  this->cancel_timeout("idle");
  this->cancel_timeout("sleep");
  if (this->awake_)
    return true;
  if (!EPaperBase::reset())
    return false;  // reset is held low; the base calls back after reset_duration_
  this->next_delay_ = 10;  // give the controllers time to come out of reset before BUSY is read
  return true;
}

// A software reset, then the internal temperature sensor; every refresh loads its own waveform,
// voltages and VCOM. The reset sets the controllers working, so BUSY is polled again after it.
bool CrowPanel579::initialise(bool partial) {
  if (!this->is_idle_())
    return false;
  if (this->awake_)
    return true;
  if (this->init_step_ == 0) {
    this->command(0x12);
    this->init_step_ = 1;
    this->delay_until_ = millis() + 10;  // BUSY rises a moment after the command
    return false;
  }
  this->cmd_data(0x18, {0x80});
  this->init_step_ = 0;
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

// A partial sends only 0x24, the new frame, and only the rows that changed: 0x26, and 0x24 everywhere
// else, already hold what is on the glass. Cancelling and settling send the rows with a pixel out of
// balance. Every other pass sends both planes whole.
void CrowPanel579::start_pass_(Pass pass) {
  if (pass == Pass::DOSE || pass == Pass::UNDOSE) {
    // The controllers have no partial window and drive every pixel, so a scan sends the rows its
    // masks are in, and those the last scans left masks in, which get the glass's reading back
    // and so hold.
    Window rows{0, static_cast<uint8_t>(PLANE_ROW - 1), this->window_.first_row, this->window_.last_row};
    merge(rows, this->stale_);
    this->first_row_ = rows.first_row;
    this->end_row_ = rows.last_row + 1;
    this->stale_ = {0, static_cast<uint8_t>(PLANE_ROW - 1), this->window_.first_row, this->window_.last_row};
    this->plane_ = 0;
    return;
  }
  if (pass == Pass::CORRECT) {
    this->find_unsettled_rows_();
  } else if (pass != Pass::FAST && pass != Pass::PARTIAL) {
    this->first_row_ = 0;
    this->end_row_ = HEIGHT;
  }
  this->plane_ = pass == Pass::FAST || pass == Pass::PARTIAL ? 2 : 0;
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

// A scan's planes, as send_planes_ sends them: the masks in the window's rows, and the glass's
// reading in both planes in any other row, which holds.
bool HOT CrowPanel579::send_own_planes_(Pass pass) {
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
    const Source masks{nullptr, this->masks_ + (new_plane ? BIT_PLANE : 0)}, reading{nullptr, this->glass_};
    this->start_data_();
    while (this->row_ != this->end_row_) {
      const size_t rows = std::min<size_t>(ROWS_A_WRITE, this->end_row_ - this->row_);
      for (size_t r = 0; r != rows; r++) {
        const size_t y = this->row_ + r;
        const bool masked = y >= this->window_.first_row && y <= this->window_.last_row;
        this->plane_bytes_(masked ? masks : reading, y, offset, HALF, data + r * HALF);
      }
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

// The glass's reading back into both planes of the rows scans left masks in, waking the
// controllers first if they have gone to sleep.
void CrowPanel579::restore_reading_() {
  if (is_empty(this->stale_) || this->glass_ == nullptr)
    return;
  this->wake_controllers_();
  this->wait_idle_(500);
  this->first_row_ = this->stale_.first_row;
  this->end_row_ = this->stale_.last_row + 1;
  this->plane_ = 0;
  while (!this->send_planes_(Source{nullptr, this->glass_}, Source{nullptr, this->glass_})) {
  }
  this->stale_ = EMPTY_WINDOW;
}

const CrowPanel579::GlideTuning *CrowPanel579::glide_tuning_() const { return &GLIDES; }

// The ink's speed against 50 Hz, where the waveforms were tuned, by the temperature's band.
float CrowPanel579::speed_() const {
  switch (this->frame_rate_for_temperature_()) {
    case FRAMES_37_5_HZ:
      return 0.75f;
    case FRAMES_75_HZ:
      return 1.5f;
    case FRAMES_100_HZ:
      return 2;
    default:
      return 1;
  }
}

// Particles move faster in the warm and slower in the cold, so the self-balanced waveforms run at
// a frame rate for the temperature, which stretches or shrinks every frame alike and leaves their
// balance as it is. The bands and ratios are the panel maker's: with 0x1A faking the temperature,
// the OTP's full refresh runs 4.5 s at 10 °C and below, 3.3 s from 13 to 20 °C (where these
// waveforms were tuned), 2.3 s from 22 to 30 °C and 1.7 s from 35 °C, which 37.5, 50, 75 and 100 Hz
// match. Without a temperature, 50 Hz.
uint8_t CrowPanel579::frame_rate_for_temperature_() const {
#ifdef USE_SENSOR
  if (this->temperature_ != nullptr && !std::isnan(this->temperature_->state)) {
    const float celsius = this->temperature_->state;
    if (celsius < 12)
      return FRAMES_37_5_HZ;
    if (celsius >= 33)
      return FRAMES_100_HZ;
    if (celsius >= 21)
      return FRAMES_75_HZ;
  }
#endif
  return FRAMES_50_HZ;
}

// The temperature band of this update's frame rate, coldest first.
int CrowPanel579::band_() const {
  switch (this->frame_rate_) {
    case FRAMES_37_5_HZ:
      return 0;
    case FRAMES_75_HZ:
      return 2;
    case FRAMES_100_HZ:
      return 3;
    default:
      return 1;
  }
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
    case Pass::GREY:
      this->load_waveform_(GREY_LUT, this->frame_rate_);
      break;
    case Pass::HALF_TO_BLACK:
      this->load_waveform_(HALF_TO_BLACK[this->band_()].bytes, FRAMES_125_HZ);
      break;
    case Pass::EVEN_GREY:
      this->load_waveform_(EVEN_GREY.bytes, this->frame_rate_);
      break;
    case Pass::HALF_TO_WHITE:
      this->load_waveform_(HALF_TO_WHITE[this->band_()].bytes, FRAMES_125_HZ);
      break;
    case Pass::DOSE:
    case Pass::UNDOSE:
      this->load_waveform_(scan_waveform(this->scan_frames_[this->pass_index_]).bytes, FRAMES_195_HZ);
      this->command(0x20);
      this->prerender_();
      return;
    case Pass::BLACK_WHITE:  // run above
      break;
  }
  this->command(0x20);
}

// Loads a waveform and runs it, leaving the supplies on afterwards for whatever comes next. VSH1
// and VSL are +15 V and -15 V, which the balance checks count on. VCOM is Waveshare's 0x30: three
// minutes of frames driving nothing over a grey frame drifted the greys no more than the ink
// settles on its own at anything from 0x20 to 0x30, where the panel's own OTP value lightened
// dark greys by 0.06 and every grey by about 0.1. The booster's soft start is left at its reset
// value, as Elecrow's firmware leaves it.
void CrowPanel579::load_waveform_(const uint8_t *waveform, uint8_t frame_rate) {
  uint8_t lut[227];
  memcpy(lut, waveform, sizeof lut);
  lut[224] = frame_rate;
  this->cmd_data(0x32, lut, sizeof lut);
  this->cmd_data(0x3F, {waveform[227]});
  this->cmd_data(0x03, {waveform[228]});
  this->cmd_data(0x04, {waveform[229], waveform[230], waveform[231]});
  this->cmd_data(0x2C, {waveform[232]});
  if (!this->supplies_on_) {
    ESP_LOGV(TAG, "Supplies on");
    this->supplies_since_ = millis();
  }
  this->cmd_data(0x22, {this->supplies_on_ ? UPDATE_DISPLAY : UPDATE_SUPPLIES_DISPLAY});
  this->supplies_on_ = true;
}

// The base sleeps the controllers after every update. Here the supplies stay on until ACTIVE_MS
// passes without another update, so the next one skips switching them on, but no longer than
// MAX_ACTIVE_MS at a stretch.
void CrowPanel579::deep_sleep() {
  if (this->continue_live_())
    return;  // ticking on, the supplies with it
  if (this->supplies_on_ && millis() - this->supplies_since_ >= MAX_ACTIVE_MS) {
    this->switch_supplies_off_();
    this->set_timeout("sleep", CONTROLLER_IDLE_MS, [this]() { this->sleep_controllers_(); });
    return;
  }
  this->set_timeout("idle", ACTIVE_MS, [this]() { this->wind_down_(); });
}

// Supplies off, and the controllers to sleep a while later. Fast partials' pushes stay recorded and
// are cancelled by the next grey refresh, or settled before a deep sleep: a push left for a while
// changes nothing, since what matters is each pixel's drive over its life, and the record keeps
// that within one push.
void CrowPanel579::wind_down_() {
  if (this->state_ != epaper_spi::EPaperState::IDLE)
    return;
  this->switch_supplies_off_();
  this->set_timeout("sleep", CONTROLLER_IDLE_MS, [this]() { this->sleep_controllers_(); });
}

// A hardware reset, which also switches the supplies off, then deep sleep.
void CrowPanel579::reset_controllers_() {
  this->cancel_timeout("idle");
  this->cancel_timeout("sleep");
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->digital_write(false);
    delay(2);
    this->reset_pin_->digital_write(true);
    delay(10);
  }
  this->awake_ = true;
  this->supplies_on_ = false;
  this->wait_idle_(100);
  this->sleep_controllers_();
}

// Settling, waiting on BUSY, for a deep sleep that cannot wait for an update: only the pixels a
// fast partial left a push out blink, and land where they were. The controllers may have gone to
// sleep since, so they are woken as an update would wake them.
void CrowPanel579::settle_now_() {
  this->wake_controllers_();
  this->find_unsettled_rows_();
  this->plane_ = 0;
  while (!this->send_planes_(Source{nullptr, this->unsettled_}, Source{nullptr, this->glass_})) {
  }
  this->cmd_data(0x3C, {BORDER_PARTIAL});
  this->cmd_data(0x21, {0x00, 0x10});
  this->load_waveform_(SETTLE.bytes, FRAMES_50_HZ);
  this->command(0x20);
  this->wait_idle_(3000);
  while (!this->send_planes_(Source{nullptr, this->glass_}, Source{nullptr, this->glass_})) {
  }
  memset(this->unsettled_, 0, BIT_PLANE);
  this->unsettled_count_ = 0;
}

// Switches the ±15 V supplies off, without waiting: the next command waits for BUSY anyway.
void CrowPanel579::switch_supplies_off_() {
  if (!this->supplies_on_)
    return;
  ESP_LOGV(TAG, "Supplies off after %u s", (unsigned) ((millis() - this->supplies_since_) / 1000));
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

// The controllers awake, as an update would wake them, waiting on BUSY.
void CrowPanel579::wake_controllers_() {
  if (this->awake_)
    return;
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->digital_write(false);
    delay(2);
    this->reset_pin_->digital_write(true);
    delay(10);
  }
  this->wait_idle_(100);
  this->command(0x12);
  this->wait_idle_(500);
  this->cmd_data(0x18, {0x80});
  this->awake_ = true;
  this->supplies_on_ = false;
}

}  // namespace esphome::crowpanel_579
