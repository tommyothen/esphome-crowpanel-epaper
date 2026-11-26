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
  and 33 °C.
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

Each frame is about 20 ms. None of them left a ghost of the frame before.

## Still open

- Thirteen greys: two more half-step passes, quarter steps, each band with its
  own length. One length for every band crosses the levels over.
- A faster grey: shortening group 2 and nudging its phases to win the greys
  back.
