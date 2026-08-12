# Video playback on the 4.26" mono PixPaper

Plays a video on an 800x480 e-paper panel.

    your video (mp4/gif/frames)  ->  video2epd.py  ->  clip.epdv  ->  player on the board

All the work happens on the host. `video2epd.py` decodes, scales, thresholds and
**packs every frame into controller RAM order**, so at play time the board only
does `mmap` + one SPI burst per frame.

Flat black-and-white silhouette animation is what a 1-bit panel renders best,
though 800x480 is enough resolution for dithered live-action to be legible.

The panel manages 30 fps at full rate, and about 16 fps with the default
settings, because the controller will diff frames in hardware and only drive
the pixels that changed.

## Build

```sh
make
```

Needs libgpiod **v2**. If `gpiodetect` shows the `pinctrl-rp1` chip somewhere
other than `gpiochip15` — on Trixie / kernel 6.12+ it is `gpiochip0`:

```sh
make EXTRA_CFLAGS='-DEPD_GPIO_CHIP=\"gpiochip0\"'
```

The 4.26" panel has no other Raspberry Pi 5 example in this repo; the pins in
`pixpaper-426-m-test-rpi5-video.c` (DC 5, RST 6, BUSY 26, `/dev/spidev0.0`) are
the ones this was measured on.

## Make a clip

Needs `opencv-python` and `numpy` on the host.

```sh
python3 video2epd.py your_video.mp4 -o clip.epdv --fps 15
python3 video2epd.py your_video.mp4 -o clip.epdv --fps 15 --preview sheet.png
```

`--preview` writes a 9-frame contact sheet sampled across the clip, unpacked
back out of the file just written — so it checks the packing, not just the
source.

| flag | what it does |
| --- | --- |
| `--fps N` | playback rate, default 15 — roughly what the player's defaults sustain. Never higher than the source's own rate. |
| `--fit pad\|crop\|stretch` | letterbox (default), fill-and-cut, or squash. A 4:3 source letterboxes to 640x480 on this 5:3 panel. |
| `--thresh N` | black/white cut point, default 128. |
| `--dither none\|bayer\|fs` | `none` suits flat animation; `bayer` is fast and worth trying here, since 800x480 has the resolution to carry a dither. |
| `--invert`, `--gamma`, `--start`, `--duration` | the obvious things. |

**Frames are 48000 bytes**, so clips get large fast: a 3:40 video is ~160 MB at
15 fps, 300 MB at 30 fps. Keep `.epdv` files out of the repo —
`.gitignore` here already does.

## Play

```sh
./pixpaper-426-m-test-rpi5-video clip.epdv
./pixpaper-426-m-test-rpi5-video clip.epdv --loop
./pixpaper-426-m-test-rpi5-video clip.epdv --free --quiet     # measure
```

| flag | what it does |
| --- | --- |
| `--loop` | repeat until Ctrl-C |
| `--free` | show every frame as fast as the panel manages, ignoring the clip's rate |
| `--fps N` | override the clip's rate |
| `--spi HZ` | SPI clock, default 20 MHz. It is the floor on frame time here, see below. |
| `--update N` | override the panel's per-frame update sequence |
| `--tp N` | drive frames per update, default 4; `--tp 0` uses the OTP waveform |
| `--fr N` | frame-rate code, i.e. how long one drive frame lasts; 4 by default, 6 is the practical floor |
| `--no-pingpong` | repaint every pixel instead of only the changed ones |
| `--refresh N` | full white refresh every N frames, see Ghosting |
| `--start N`, `--keep`, `--quiet` | |

By default the player follows the wall clock and skips frames it could not keep
up with, so a clip always runs for its own length. `--free` shows every frame
and reports sustained fps — that is how you measure.

Ctrl-C (or SIGTERM) leaves partial mode cleanly and clears to white.

## Speed: --tp and --fr

Measured on a Raspberry Pi 5, every setting below checked on the panel rather
than inferred from timing:

    frame_ms  ~=  SPI burst  +  tp * period(fr)

| setting | frame time | sustained | note |
| --- | --- | --- | --- |
| `--tp 0` | 670 ms | 1.5 fps | the controller's OTP waveform |
| `--tp 8 --fr 4` | 101 ms | 9.9 fps | |
| `--tp 4 --fr 4` | 61 ms | 16.5 fps | **default** |
| `--tp 3 --fr 4 --spi 32000000` | 47 ms | 21.3 fps | plays 20 fps content with margin |
| `--tp 2 --fr 6 --spi 32000000` | 30 ms | 33.2 fps | plays 30 fps content at full rate |
| `--tp 1 --fr 6 --spi 32000000` | 23 ms | 42.7 fps | near the floor |

**The SPI burst is the hard floor.** 48000 bytes is 384000 bits: 77 ms at
5 MHz, 20 ms at 20 MHz, 17 ms at the ~23 MHz the Pi actually produces when
asked for 32. Drive time can go to zero, that cannot, so ~58 fps is the
ceiling — raising the clock is the first thing worth doing here.

**`--tp` is drive frames, `--fr` is how long a frame lasts.** Total drive is
`tp * period(fr)`, and that is what moves pigment. Measured periods:

| `--fr` | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- |
| period | 20.1 ms | 13.4 ms | 10.1 ms | 8.1 ms | 6.7 ms |

`--fr` has a floor: one frame has to be long enough for the controller to scan
all 480 gate lines. At `--fr 8` (5 ms) it cannot, and a whole band of the panel
is left undrawn. `--fr 6` works; 7 is untested.

## Four things that have to be right

Every one of these was wrong at some point here. They produce different
symptoms, and each is easy to misread as one of the others:

| what you see | what it usually is |
| --- | --- |
| smeared, static areas degrading | frame differencing not enabled (1) |
| pale, washed out | source voltage too low (2) |
| present but unclear, never quite black | gate voltage too low, or VCOM unset (2) |
| a band of the panel never drawn | `--fr` too low to scan every gate line (3) |
| frozen image, but timings perfect and linear | supply (4) |

**1. Frame differencing is off until you enable it.** The controller can carry
the displayed frame forward and drive only the pixels that differ, but it does
not do so at power-on. `epd_partial_begin()` turns it on, and that is what
makes a short waveform viable: static areas are never repainted, so they never
accumulate smear. Without it every frame repaints all 800x480 and a short drive
looks filthy.

**2. Drive voltages have to be the panel's, not the stock example's.** The
voltages in the stock `../pixpaper-426-m-test-*.c` waveform belong to its
greyscale passes, which drive deliberately weakly to land on intermediate
tones. Reused for a black/white refresh they leave the image pale, and the gate
voltage in particular is low enough that the pixel TFTs never fully open --
which reads as an unclear image rather than merely a pale one. `video_app.h`
sets all of them to the panel's rated levels instead; the datasheet for your
panel has the figures.

**3. `--fr` has a floor.** A drive frame must last long enough for the
controller to scan every gate line. Below that a whole band of the panel is
left undrawn. `--fr 6` works on this panel and 8 does not.

**4. Supply current.** An underpowered Pi produces the most misleading failure
here: the controller answers normally, BUSY is punctual and frame times are
exact and perfectly linear in `--tp` -- and nothing moves on the glass, because
the booster cannot reach drive voltage. If the image freezes, check
`vcgencmd get_throttled` before suspecting the waveform. A full refresh
finishing in well under a second means it is not really driving.

## Ghosting

Ping-pong is what keeps a short waveform clean, and it works by never
repainting a pixel that has not changed. The flip side is that a static area is
also never refreshed, so any residue it picks up stays. `--refresh N` does a
full white refresh every N frames, which clears it at the cost of about four
seconds and a visible flash; the player re-syncs the playhead afterwards so the
clip keeps its own length. The default is off.

**How much residue actually builds up over a long run has not been measured
here** -- the runs behind the figures above were minutes, not hours. If you are
leaving a clip playing unattended, watch the background for a while before
trusting it, and set `--refresh` if it drifts.

## .epdv format

A pack is panel-specific and the player checks it: `frame_bytes` must be 48000
here, so a file built for another panel is rejected with a message rather than
displayed as noise.

```
0   char     magic[4]      "EPDV"
4   uint16   version       1
6   uint16   flags
8   uint16   width         logical, informational
10  uint16   height
12  uint32   frame_us      nominal display time per frame
16  uint32   frame_count
20  uint32   frame_bytes   48000 for this panel
24  uint32   reserved[10]
64  frames...
```

Each frame is in the order the panel wants it: bit 1 = white, row-major, 100
bytes per row, the top bit of each byte being the leftmost of its eight pixels.
No horizontal flip — `../png2bit_426.py` mirrors its input, but on this wiring
that comes out backwards.
