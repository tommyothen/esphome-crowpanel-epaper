# CrowPanel 5.79" panel notes

From Elecrow's schematics and demos, GxEPD2's GDEY0579T93 driver, the other
community drivers listed in [Prior art](../README.md#prior-art), and testing on
the board.

- **Pins.** SPI clock GPIO12, data GPIO11, CS 45, DC 46, reset 47, BUSY 48.
  GPIO7 switches the panel's ground through Q9 and must be high before
  anything answers.
- **Two SSD1683s behind one CS.** The left controller takes the same commands
  with 0x80 set: 0x91, 0xC4, 0xC5, 0xCE, 0xCF, 0xA4, 0xA6. Commands without a
  RAM or address argument (0x12, 0x18, 0x22, 0x20, 0x3C, 0x10) go to both.
- **The split.** Each controller has 400 columns of RAM and shows 396. A
  99-byte row splits at byte 49, which both receive: the left controller
  shows its high nibble and the right one its low nibble. The right
  controller is mounted the other way round, so its X counts down from the
  seam (entry mode 0x02) while the left one counts up (0x03). The seam at
  395/396 neither repeats nor loses a column.
- **BUSY** is high while busy. The board pulls it low, so it reads idle while
  the controllers sleep; never wait for it to rise after a reset.
- **Init** is a software reset (0x12) and the internal temperature sensor
  (0x18 0x80).
- **Full refresh** is 0x22 0xF7, with the OTP waveform for the temperature.
  About 3.7 s.
- **The cascade bit.** Partial and grey refreshes need 0x21 0x00 0x10; GxEPD2
  calls the 0x10 the cascade bit. Without it, every changed pixel stops half
  way, at grey, whatever the waveform, so the advice in crowtides never to
  send 0x21 does not hold on this board.
- **Loaded waveforms.** 0x32 takes 227 bytes in the black and white mode's
  format: five tables of six groups, VCOM's and then WW, BW, WB and BB, named
  by the pixel's 0x24 bit then its 0x26 bit. 0x3F, 0x03, 0x04 and 0x2C take
  the end option and the voltages, then 0x22 0xCF runs it without the OTP.
  On this panel VSH1 (+15 V) drives a pixel black and VSL (-15 V) white.
- **The OTP partial waveform** (0x22 0xFF) runs about ten frames and also
  drives pixels that do not change: two of them took a light grey from 52 to
  65 on the camera, and eleven took it nearly to white. The driver loads its
  own instead: a pixel turning black is pushed towards white for 15 frames
  and then towards black for 15, one turning white the other way round, and
  the rest get nothing. It clears a moved box a little better than the OTP's
  and leaves greys untouched. Pushing one way only, for 20 frames, clears a
  little better still but balances only over many changes. Fewer frames are
  faster but leave more behind, measured against flat frames (1 is white):

  | Frames a half | Black a partial drew, cleared | Black a grey refresh drew, cleared | Black drawn |
  |---|---|---|---|
  | 8 | 0.75 | 0.40 | 0.13 |
  | 10 | 0.83 | 0.60 | 0.11 |
  | 12 | 0.86 | 0.72 | 0.09 |
  | 15 | 0.89 | 0.83 | 0.07 |

- **The cold partial.** The first partial after a boot or a wake runs the
  balanced waveform, since the driver does not yet know what the glass
  shows. Nobody is usually watching it, so it can afford more frames:

  | Frames a half | Time | Black a grey refresh drew, cleared | Black a partial drew, cleared |
  |---|---|---|---|
  | 15 | 0.89 s | 0.80 | 0.93 |
  | 20 | 1.10 s | 0.89 | 0.96 |
  | 25, the driver's | 1.29 s | 0.93 | 0.97 |

- **What a partial compares.** RAM 0x26 should hold what is on the glass and
  0x24 the new frame. A refresh with a loaded waveform swaps the banks 0x24
  and 0x26 address until the next reset, which is what RAM ping-pong (0x37,
  set from the OTP) does: after a partial, writing the new frame to 0x24
  alone makes the next partial right and 0x26 alone does not. The driver
  writes the frame's black and white reading to both after every such
  refresh, as GxEPD2's `writeImageAgain` does, so the next partial is right
  with or without a reset in between.
- **Where the time goes.** A balanced 15-frame partial drove the panel for
  0.85 s, switching the supplies on and off included, and the update took
  1.17 s: the rest was resetting and initialising the controllers, sending
  the frame, writing it back to both planes, and waiting for ESPHome's next
  loop (about 16 ms) between each of a dozen steps. The driver runs the
  steps back to back within one loop call and leaves the controllers awake
  for a while after an update, which took that update to 1.0 s; keeping
  the supplies on and pushing one way took a partial to 0.4 to 0.5 s.
- **One SPI write a row was slow.** Each write costs more than its 50
  bytes, so rows now go out 16 at a time.
- **Only what changed.** The driver keeps a hash of each row's black and
  white reading as the glass shows it, 1 KB in RTC memory so it survives
  deep sleep. A partial sends, and afterwards copies back, only the band of
  rows that changed: a clock tick sends a sixth of the frame and takes
  0.92 s. A redraw that changes nothing skips the refresh and takes only
  the drawing, about 50 ms.
- **Grey.** The OTP has no grey waveform; claiming 80 to 127 °C with 0x1A in
  display mode 1 gives only black and white. Waveshare's four-grey waveform
  for their 4.2" V2, another SSD1683, works. Each pixel's high bit goes to
  0x26 and its low bit to 0x24, 11 white and 00 black. Its end option is 0x07,
  which leaves the pixels charged to the last phase (+15 V on black); the
  datasheet asks for 0x22, which discharges them, and the greys look the same.
- **Seven greys.** Each pixel's RAM bits pick one of four waveforms, so one
  refresh gives four greys at most. A seven-grey refresh runs three: a half
  step towards black for levels 2, 3 and 5, a four-grey refresh that draws
  black, dark grey (levels 1 to 3), light grey (levels 4 and 5) and white, and
  the same half step towards white. The two half steps cancel, and the middle
  pass balances on its own. Where a table lands hangs on its last few frames:
  the dark table with 15 frames of black drive lands at 0.46, and with 2 more
  frames of black at the very end at 0.12, so the middle pass makes dark grey
  level 1 directly, at 0.20, and the half steps carry levels 2 and 3 up from
  it. They run at 125 Hz, 8 ms a frame, since one 20 ms frame moves a grey too
  far: 2, 4 and 3 frames for levels 2, 3 and 5. Pixels that get no half step
  get a one-frame trim instead, towards white before and black after, which
  takes level 1 and light grey down a touch and leaves black black; white
  shares level 2's step, which a push and a pull leave white. Measured
  against flat white and black frames, the seven read 0.02, 0.17, 0.30, 0.50,
  0.63, 0.81 and 0.99, within 0.04 of even steps (the first try was 0.01,
  0.18, 0.47, 0.61, 0.71, 0.87 and 1.00), and partials over them changed none
  by more than
  0.01. The camera sees a strong lamp gradient across the panel, so every
  reading here is corrected against flat frames.
- **Keeping the supplies on.** 0x22 0xCC switches the supplies on and runs
  the loaded waveform, leaving them on; 0x0C runs it with them already on;
  0x03 switches them off. Each switch takes about 0.1 s. GxEPD2 leaves them
  on between partials too. Whether an idle controller with its supplies on
  puts a voltage across the pixels was the worry: over two minutes, a grey
  frame drifted no more with them on than off (light grey 0.45 to 0.44
  against 0.40 to 0.38, the ink settling either way).
- **Settling.** The pass that cancels a fast partial without a grey refresh
  to hide it must add one push's worth of drive and still leave each pixel
  where it was, so it pushes past the far colour and comes back. Measured
  against flat frames: 30 frames over and 15 back left whites at 0.96, 40
  and 25 at 1.02, 45 and 30 at 1.03.
- **VCOM.** Every loaded waveform sets VCOM, the offset that cancels the
  TFTs' kickback; if it is off for a panel, every frame of every refresh
  leaves a little drive on every pixel, always the same way. A test: three
  minutes of frames driving nothing over a grey frame, so only VCOM acts. At
  anything from 0x20 to Waveshare's 0x30 the greys drifted no more than the
  ink settles with no frames at all (light grey -0.02). The panel's own OTP
  value, loaded with 0x22 0xB9, lightened dark grey by 0.06 and every grey by
  about 0.1, so the driver keeps 0x30. The booster's soft start (0x0C) is left
  at its reset value, as Elecrow's firmware leaves it.
- **Temperature.** Faking the temperature with 0x1A and timing the OTP's
  own waveforms shows the maker's bands: the full refresh runs 4.50 s at
  10 °C and below, 3.28 s from 13 to 20 °C, 2.27 s from 22 to 30 °C and
  1.66 s from 35 °C, while the partial runs 0.43 s at every temperature. The
  four-grey refresh at 37.5, 50, 75 and 100 Hz runs 4.78, 3.53, 2.35 and
  1.77 s, the same progression, so the driver switches frame rate at 12, 21
  and 33 °C. (The deeper white since adds 27 frames: 4.2 s at 50 Hz.)
- **Read back.** The controllers can report their temperature, RAM and display
  options, but only over the data line, which the board wires to one GPIO as
  an output. Reading would mean bit-banging it.
- **Deep sleep.** 0x10 0x01 keeps RAM. With GPIO7 and reset held high through
  an ESP32 deep sleep, the panel keeps its picture and both planes, and they
  survive the hardware reset and 0x12 that start the next update.
- **The board.** 8 MB of octal PSRAM in the S3's package. The USB-C port is a
  CH340 on UART0, which also resets the board into the bootloader, so the
  logger needs `hardware_uart: UART0` and flashing needs no BOOT button.

## The grey waveform's timing

Waveshare's waveform has four groups that do something, each run a set number
of times. Group 1 only shakes the particles: the same drive in every pixel's
table, balanced positive and negative. Group 2 is where the greys form. Busy
time on this panel, and the greys photographed against each other after a
grey frame and after eight partials:

| Group 1 | Group 2 | Busy | Greys |
|---|---|---|---|
| 5 | 5 | 5.47 s | Waveshare's |
| 3 | 5 | 4.57 s | the same |
| 2 | 5 | 4.12 s | the same |
| 1 | 5 | 3.68 s | the same, and the driver's choice |
| 1 | 3 | 3.00 s | dark and light grey a shade lighter |
| 1 | 2 | 2.65 s | dark and light grey a shade lighter |

Each frame is about 20 ms. None of them left a ghost of the frame before. The deeper white
since adds 27 frames, 0.5 s at 50 Hz.

## Measuring greys

Every grey here was tuned on a laptop camera's JPEG codes at a fixed exposure, normalised
between flat white and flat black frames. That reading depends on the exposure: on 2026-11-27
the same seven-grey ramp read 0.18, 0.36, 0.54, 0.70 and 0.81 at an exposure of 200, 0.24,
0.43, 0.64, 0.76 and 0.84 at 400, and 0.29, 0.48, 0.64, 0.73 and 0.81 at 700, the whites
running into the camera's highlight knee. The camera is no power law either: it adds a pedestal
of about 19 codes and lifts its shadows unless `backlight_compensation` is off.

A reading that does not depend on the camera: photograph each frame at exposures of 50, 100,
200 and 400 (exposure time is linear in light), fit the camera's inverse response, a cubic in
the log of the code above the pedestal, jointly with every patch's light (a linear least
squares; it fits to 3%), then place each grey's CIE L* between the black's and the white's.
Read that way, the seven greys are 0.31, 0.49, 0.64, 0.75 and 0.83 against 0.17, 0.33, 0.50,
0.67 and 0.83, up to 0.16 light in the darks, and the four greys 0.26 and 0.62 against 0.33
and 0.67. The panel's black reflects 0.08 of its white. The waveforms are the ones tuned on
2026-11-25, byte for byte bar the deeper white, and the old white read the same, so the panel
has not moved: the yardstick has.

The four greys stay as they are: one frame of white at 50 Hz, anywhere in group 3, moves dark
grey 0.13 and light grey 0.08 (to 0.39 and 0.70), more than they are out.

The re-base, the same day, on both panels. Here the seven greys' middle pass ends dark grey on a
frame of black more (group 3's phase D) and the half steps trim with 2 frames, not 1: the seven
read 0.01, 0.18, 0.37, 0.54, 0.69, 0.83 and 1.00. The ticks' curves were measured afresh in L* at
the scans' VCOM, their stops moved to 20 and 245 to keep 64 frames each way, and the glide table
took three rounds against exact refreshes (0.088 out on average at first, then 0.057, then 0.041;
0.035 gliding back). A box taken black and back by ticks once, and five times, came out 0.009 and
0.019 whiter than the white beside it. The 4.2"'s are in its notes. A faked 11 °C (37.5
Hz) took the seven greys lighter still, to 0.29, 0.57, 0.73, 0.83 and 0.90 in codes, so the
room's 12 °C is not the cause.

## Still open

- Thirteen greys: two more half-step passes, quarter steps, each band with its
  own length. One length for every band crosses the levels over.
- A faster grey: shortening group 2 and nudging its phases to win the greys
  back.

## Glides and live ticks

- **How fast the controllers go.** Timed with BUSY on SPI at 20 MHz, supplies on: a refresh
  costs 10.3 ms beyond its frames at 0x08, the fastest frame rate, 5.1 ms each (15.4 ms for
  one frame). Byte 224's codes 0x00 to 0x08 run at about 15, 25, 49, 74, 99, 124, 148, 173 and
  195 Hz, and 0x09 to 0x0F at 37, 62, 86, 111, 135, 161 and 186. The overhead is about two
  frames at any rate. All four planes (both controllers, OLD and NEW) take 22.8 ms; 40 rows of
  them 4.3 ms.
- **No partial window.** The SSD1683 refreshes every pixel, so a scan sends the rows its masks
  are in, and gives the rows earlier scans left masks in the glass's reading back, in both
  planes, which holds them (WW and BB drive nothing). Writing both planes before every scan
  also keeps the RAM ping-pong out of it.
- **Doses.** At 195 Hz from a settled white (the panel's own full refresh), 6, 12, 18, 24, 30,
  36, 48, 60 and 72 frames of black given as binary digits left 0.89, 0.67, 0.53, 0.37, 0.30,
  0.24, 0.12, 0.08 and 0.05; from a settled black, as much white left 0.08, 0.23, 0.39, 0.57,
  0.72, 0.76, 0.84, 0.89 and 0.92: the ink moves at about two thirds of the 4.2"'s pace.
  Streams of 4, 6 and 8-frame refreshes went further per frame, 8-frame ones reaching 0.02 and
  1.00 after 72. The first guess, the 4.2"'s tuning with a quarter more frames, left old digits
  and a moving ball's trail ghosting: the model had them white long before they were.
- **Glides.** The table read from those curves landed 0.084 off on average against an exact
  refresh of the same grid; two rounds of tuning on the camera, weighing the glide back from
  each glided level too, brought it to 0.056 out and 0.055 back on the camera's codes (0.041
  and 0.035 in L* after the re-base below). A full refresh afterwards
  left every cell within 0.035. Black takes 66 frames and white 90, so a glide may push up to
  96 frames, in up to seven scans.
- **Ticks.** 8 frames, not 4: this panel's ticks wait on drawing and planning (about 60 ms)
  rather than on the 51 ms an 8-frame refresh takes, so the longer push is free. The live
  demo, a clock, a ball and a ticker, went from 11 to 14 to 16 ticks a second with it, and
  its ghosting cleared. Setting 0x21 to 00 10, which loaded waveforms need, before the panel's
  own full refresh turned an all-white RAM into a black panel in the test rig; the driver does
  not set it there.
- **White ghosting in live mode.** A box taken black and back by ticks, once and three times,
  then compared with the untouched white beside it, came out 0.014 and 0.047 whiter. A traced
  pixel showed every cycle balanced, 64 frames of black and 64 of white, so it was the ink: a
  swing with equal drive leaves a deeper white than the grey refresh draws, deeper each time.
  Ending each white tick in 2 or 5 frames of black only moved the offset, not the build-up.
  Three changes fixed most of it. The grey refresh's white went 27 frames deeper (it had been
  0.018 shallower than the panel's own refresh's white; five crossings then left 0.014, not
  0.040). Ticks stop black and white at matched depths (13 and 245 of 255 in the model), so the
  way back pays the drive back and none is left over. And scans and ticks run VCOM at its null,
  0x28: held for 800 frames, a black half and a white half moved +0.004 and -0.001, as much as
  they settle on their own, where 0x30 darkened the white 0.004, 0x38 0.012, 0x40 0.032 and
  0x50 0.14; the untouched white had drifted 0.016 darker over 45 s of ticking and now holds.
  With all three, one crossing left +0.004 and three +0.017.

## Turbo

One light run, the same test as the 4.2"'s (see its notes), at the default 96 frames from the
far end, 48 for a white already there and 36 for a black. Against the balanced driver's white
and black:

| | Balanced | Turbo |
|---|---|---|
| Full refresh, black and white | 4.4 to 4.6 s | 0.67 to 0.87 s |
| Full refresh, seven greys | 4.5 s | 1.4 s |
| White ghost, 12 s later | -0.004, -0.005 | +0.010, +0.012 |
| Black ghost | -0.015 | -0.008 |
| Greys | 0.01, 0.24 to 0.27, 0.43 to 0.50, 0.64 to 0.68, 0.77 to 0.79, 0.84 to 0.86, 1.00 | 0.00 to 0.02, 0.02 to 0.06, 0.30 to 0.33, 0.42 to 0.44, 0.67 to 0.69, 0.80 to 0.84, 0.94 to 0.98 |

The darkest grey lands near black: a freshly reset white takes black faster than the settled
white the glides were measured from. A second run with its own doses from white (31, 22, 15, 11
and 7 frames rather than the glides' 38, 23, 16, 11 and 7) took level 3 to 0.50 but left levels
1 and 2 at 0.04 and 0.08 to 0.15, one frame fewer making level 2 darker: on this panel the
binary scans' order and the holds after them count for more than the frames, which a light run
cannot untangle. The driver keeps the glides' doses.
