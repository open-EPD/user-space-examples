# Video playback on the 2.13" mono PixPaper

Plays a video on a 250x122 e-paper panel, using the same partial-refresh path
as the Open-EP Pet demo (`../pet/`).

    your video (mp4/gif/frames)  ->  video2epd.py  ->  clip.epdv  ->  player on the board

All the work happens on the host. `video2epd.py` decodes, scales, thresholds and
**packs every frame into controller RAM order**, so at play time the board only
does `mmap` + one 4000-byte SPI burst per frame. Nothing is decoded or
bit-twiddled on the target — that matters on a slow SoC.

Flat black-and-white silhouette animation is what a 1-bit panel renders best;
live-action footage works but needs dithering and still reads as mush at
250x122.

## Build

```sh
make
```

Needs libgpiod **v2** (`libgpiod-dev` on Trixie) — the Pi 5 backend uses the v2
API and the Makefile stops with a clear message if it finds v1.

The board `.c` is a thin GPIO backend; the SPI/EPD/player core lives in
`video_app.h`. If `gpiodetect` shows the `pinctrl-rp1` chip somewhere other than
`gpiochip15`, point the build at it — on Trixie / kernel 6.12+ it is `gpiochip0`:

```sh
make EXTRA_CFLAGS='-DEPD_GPIO_CHIP=\"gpiochip0\"'
```

Getting this wrong fails at startup with "Error opening GPIO chip", which looks
like a wiring fault but is not.

Only the Raspberry Pi 5 target exists here, because that is the only board this
has been run on. To add another, copy the board `.c`, change the gpiochip and
the three pins, and keep `#include "video_app.h"` last — the backend contract is
the five items listed at the top of that header.

## Make a clip

Needs `opencv-python` and `numpy` on the host (same deps as `../png2bit.py`).

```sh
python3 video2epd.py your_video.mp4 -o clip.epdv --fps 6

# see what the panel will show, without a panel
python3 video2epd.py your_video.mp4 -o clip.epdv --fps 6 --preview sheet.png
```

`--preview` writes a 9-frame contact sheet sampled across the clip, unpacked
back out of the file just written — so it checks the packing, not just the
source.

| flag | what it does |
| --- | --- |
| `--fps N` | playback rate (default 6). Sets the file size and the pacing — match it to what your `--tp`/`--fr` actually sustain. |
| `--fit pad\|crop\|stretch` | letterbox (default), fill-and-cut, or squash. A 4:3 source letterboxes to 162x122; `crop` fills the screen but cuts heads off 4:3 footage. |
| `--thresh N` | black/white cut point, default 128. Lower = more black. |
| `--dither none\|bayer\|fs` | `none` suits flat animation. `bayer` is fast and fine for real footage; `fs` looks best but is slow (pure Python). |
| `--invert`, `--gamma`, `--start`, `--duration` | the obvious things. |
| `--portrait` | pack for a 122x250 portrait mounting. |

The source can also be a directory of numbered PNGs (`--src-fps` then matters).

At 6 fps a 3:40 clip is about 1300 frames, ~5 MB.

## Play

```sh
./pixpaper-213-m-test-rpi5-video clip.epdv
./pixpaper-213-m-test-rpi5-video clip.epdv --loop --tp 10
./pixpaper-213-m-test-rpi5-video clip.epdv --free --quiet
```

| flag | what it does |
| --- | --- |
| `--loop` | repeat until Ctrl-C |
| `--free` | show every frame as fast as the panel manages, ignoring the clip's rate |
| `--fps N` | override the clip's rate |
| `--tp N` / `--fr N` | partial-waveform tuning, see below |
| `--refresh N` | full white refresh every N frames, to clear ghosting |
| `--start N`, `--keep`, `--quiet`, `--spi HZ` | |

By default the player follows the wall clock: it works out which frame *should*
be on screen and skips any it could not keep up with, so the clip always runs
for its own length. `--free` turns that off and shows every frame, which is what
you want when measuring the panel. Either way it prints its own `ms/frame` and
sustained fps on exit.

Ctrl-C (or SIGTERM) leaves partial mode cleanly and clears to white; `--keep`
leaves the last frame up.

## Speed: --tp and --fr

Per-frame time is set by the waveform, not by SPI — 4000 bytes at 5 MHz is
~6 ms.

### Two things are called a "frame"

Keep these apart or none of the numbers below make sense:

| | what it is | how long |
| --- | --- | --- |
| **LUT frame** (a *tick*) | the waveform engine's time unit — one step at which the controller can change the voltage it applies | `period(fr)`, e.g. 13.3 ms at `--fr 3` |
| **displayed picture** | one image you actually see | `47.3 + (tp + 2) * period` ms |

One picture = one whole run of the LUT = `tp + 2` ticks. A tick is not a
picture; `--tp` is not "pictures per second".

### What one tick does

Each tick the controller applies a voltage to every pixel, chosen by which
old-value → new-value group that pixel falls in. Phase 0 of `WF_PARTIAL`:

| LUT group | transition | phase-0 byte | voltage |
| --- | --- | --- | --- |
| LUT0 | 0 → 0 (unchanged) | 0x00 | VSS — not driven |
| LUT1 | 0 → 1 (changing) | 0x80 | VSL (negative) |
| LUT2 | 1 → 0 (changing) | 0x40 | VSH1 (positive) |
| LUT3 | 1 → 1 (unchanged) | 0x00 | VSS — not driven |

That is the ping-pong differential in hardware: unchanged pixels get VSS for the
whole picture and never move.

E-paper pixels are physical — charged pigment particles migrating through fluid
— and one 13.3 ms tick nowhere near finishes the trip. `--tp` is therefore
closer to an **exposure time** than to a frame rate: `(tp + 2) * period` is how
long the field is held on the pixels that need to flip. Hold it too briefly and
the particles only get part way across, which reads as grey instead of black.

The default `--tp 16 --fr 3` breaks down as:

    287 ms per picture
    ├─   4 ms   SPI: 4000 bytes into RAM
    ├─  43 ms   0x22/0x20 update sequence + BUSY wait      <- fixed overhead
    └─ 239 ms   the LUT ─┬─ phase 0: 16 ticks x 13.3 ms = 213 ms   <- the drive
                         ├─ phase 1:  1 tick  x 13.3 ms =  13 ms
                         └─ phase 2:  1 tick  x 13.3 ms =  13 ms

Phases 3..11 are all zero in this waveform, which is where the `+ 2` comes
from — it is structural, not a fudge factor.

### The model

Measured on a Raspberry Pi 5 at 5 MHz SPI:

    frame_ms  ~=  47.3  +  (tp + 2) * period(fr)

`--fr` is a *code*, not a duration: the 3-bit value goes into LUT bytes
144..149 (one nibble per phase, both nibbles set the same here) and the
controller looks the period up in its own table.

| `--fr` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| tick (ms) | 66.6 | 39.9 | 20.0 | **13.3** | 10.0 | 8.0 | 6.65 | 5.70 |
| ≈ Hz | 15 | 25 | 50 | **75** | 100 | 125 | 150 | 175 |

`--tp` is LUT byte 60 (`TP[A]` of phase 0). That byte reads `0x14` (20) in the
`WF_PARTIAL` array but is always overwritten at upload, so the 20 is never
used — do not be misled by it when reading the table.

The ~47 ms is fixed cost — the SPI burst, the update sequence and the BUSY
wait — so it is the hard floor: about 21 fps with zero drive time, which of
course draws nothing. Some real points:

    --tp 16 --fr 3   287 ms   3.5 fps   (default; 239 ms of drive, solid black)
    --tp  8 --fr 3   180 ms   5.6 fps
    --tp 16 --fr 7   150 ms   6.7 fps
    --tp  8 --fr 7   104 ms   9.6 fps   (57 ms of drive -- expect it light)
    --tp  1 --fr 7    64 ms  15.6 fps   (17 ms of drive -- probably blank)

The catch: what actually moves a pixel is the total drive time,
`(tp + 2) * period`, and every bit of speed above comes out of it. At matched
drive time the two knobs are interchangeable — `--tp 21 --fr 7` and
`--tp 8 --fr 3` are both ~132 ms of drive and both land at ~180 ms per frame.
So `--fr` buys no free speed; it buys **finer granularity** (5.7 ms steps
instead of 13.3 ms), which is what you want when tuning near the edge.

No number here is right for your panel: contrast at a given drive time depends
on the unit and on room temperature. Sweep it and look at the screen:

```sh
for tp in 16 12 10 8; do
    ./pixpaper-213-m-test-rpi5-video clip.epdv --free --tp $tp --fr 7
done
```

Then rebuild the pack at whatever fps you settled on, so playback is paced
instead of permanently dropping frames.

## Ghosting

Partial refresh never fully clears the previous image, and over thousands of
frames a faint residue builds up. `--refresh N` does a full white refresh every
N frames; it costs about two seconds and a visible flash, and the player
re-syncs the playhead afterwards. High-contrast silhouette footage tolerates a
lot of ghosting, so the default is off.

## Why 0x24 only

`epd_partial_frame()` writes the new frame to RAM bank 0x24 and never touches
0x26. The 0x37 setting loaded with the partial LUT puts the controller in
ping-pong mode: it keeps the previous frame in the other bank and drives only
the pixels that differ. Writing 0x26 by hand fights that and scrambles the
image. Same rule as the pet demo and the `partial`/`clock` modes in
`../pixpaper-213-m-test-rpi5.c`.

## .epdv format

64-byte little-endian header, then `frame_count` frames of `frame_bytes` each,
back to back:

```
0   char     magic[4]      "EPDV"
4   uint16   version       1
6   uint16   flags         bit0 = portrait
8   uint16   width         logical, informational
10  uint16   height
12  uint32   frame_us      nominal display time per frame
16  uint32   frame_count
20  uint32   frame_bytes   4000 for this panel
24  uint32   reserved[10]
64  frames...
```

Each frame is controller RAM order, bit 1 = white:

```
landscape   byte = frame[x * 16 + (y >> 3)],  bit 0x80 >> (y & 7)
portrait    same, with gate = y and bit = 121 - x
```

which is exactly the layout `fb_set()` writes in the other examples, so anything
that can build one of those framebuffers can write a `.epdv` frame.
