#!/usr/bin/env python3
"""Pack a video (or a directory of images) into an .epdv frame file for the
2.13" mono PixPaper.

Every frame is stored exactly as the controller wants it in RAM, so the on-board
player only has to memcpy it out over SPI:

    landscape  RAM byte = frame[x * 16 + (y >> 3)], bit 0x80 >> (y & 7)
    portrait   same, with gate = y and bit = 121 - x

Bit value 1 = white, 0 = black -- same convention as fb_set() in the C code.

examples:
  python3 video2epd.py clip.mp4 -o clip.epdv --fps 6
  python3 video2epd.py clip.mp4 -o clip.epdv --fps 8 --preview sheet.png
  python3 video2epd.py frames/ -o clip.epdv --fps 4 --src-fps 30 --dither bayer
"""

import argparse
import os
import struct
import sys

import cv2
import numpy as np

DISP_W = 250          # gate axis (landscape width)
DISP_VIS = 122        # visible rows
DISP_H = 128          # RAM rows per gate line (122 visible + 6 padding)
DISP_STRIDE = DISP_H // 8
FRAME_BYTES = DISP_W * DISP_STRIDE      # 4000

EPDV_MAGIC = b"EPDV"
EPDV_VERSION = 1
EPDV_FLAG_PORTRAIT = 0x0001
HDR_FMT = "<4sHHHHIII40x"               # 64 bytes, see struct epdv_hdr

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".pgm", ".tif", ".tiff")


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("input", help="video file, or a directory of numbered images")
    p.add_argument("-o", "--output", default="clip.epdv", help="output .epdv file")
    p.add_argument("--fps", type=float, default=6.0,
                   help="playback frame rate (default 6; the panel does roughly "
                        "3-8 fps depending on the player's --tp)")
    p.add_argument("--src-fps", type=float,
                   help="source frame rate; needed only for image directories "
                        "or when the container lies (default 30)")
    p.add_argument("--portrait", action="store_true",
                   help="pack for a portrait (122x250) panel orientation")
    p.add_argument("--fit", choices=["pad", "crop", "stretch"], default="pad",
                   help="pad = letterbox and keep the whole frame (default), "
                        "crop = fill the screen and cut the overflow, "
                        "stretch = ignore the aspect ratio")
    p.add_argument("--pad-color", choices=["white", "black"], default="white",
                   help="letterbox colour for --fit pad (default white)")
    p.add_argument("--thresh", type=int, default=128,
                   help="black/white threshold 0..255 (default 128)")
    p.add_argument("--dither", choices=["none", "bayer", "fs"], default="none",
                   help="none = hard threshold, right for flat silhouette "
                        "animation (default); bayer = fast 8x8 ordered dither "
                        "for real footage; fs = Floyd-Steinberg, best looking "
                        "but slow (seconds per frame in pure Python)")
    p.add_argument("--invert", action="store_true", help="invert black and white")
    p.add_argument("--gamma", type=float, default=1.0,
                   help="gamma applied before thresholding (>1 brightens)")
    p.add_argument("--start", type=float, default=0.0,
                   help="skip the first N seconds of the source")
    p.add_argument("--duration", type=float,
                   help="encode only N seconds")
    p.add_argument("--preview", metavar="PNG",
                   help="write a 9-frame contact sheet sampled across the clip, "
                        "unpacked from the file just written (so it checks the "
                        "packing, not just the source)")
    return p.parse_args()


def to_gray(frame):
    if frame.ndim == 3:
        return cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    return frame


def fit_frame(gray, dst_w, dst_h, mode, pad_value):
    """Resize gray into dst_w x dst_h under the chosen fit rule."""
    if mode == "stretch":
        return cv2.resize(gray, (dst_w, dst_h), interpolation=cv2.INTER_AREA)

    sh, sw = gray.shape[:2]
    scale = min(dst_w / sw, dst_h / sh) if mode == "pad" else max(dst_w / sw, dst_h / sh)
    nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
    interp = cv2.INTER_AREA if scale < 1 else cv2.INTER_LINEAR
    small = cv2.resize(gray, (nw, nh), interpolation=interp)

    if mode == "crop":
        x0, y0 = (nw - dst_w) // 2, (nh - dst_h) // 2
        return small[y0:y0 + dst_h, x0:x0 + dst_w]

    canvas = np.full((dst_h, dst_w), pad_value, dtype=np.uint8)
    x0, y0 = (dst_w - nw) // 2, (dst_h - nh) // 2
    canvas[y0:y0 + nh, x0:x0 + nw] = small
    return canvas


BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.float32)


def dither_bayer(gray):
    """8x8 ordered dither to 1 bit; returns a bool array, True = white."""
    h, w = gray.shape
    tile = np.tile((BAYER8 + 0.5) * (255.0 / 64.0),
                   (h // 8 + 1, w // 8 + 1))[:h, :w]
    return gray.astype(np.float32) >= tile


def dither_fs(gray):
    """Floyd-Steinberg to 1 bit; returns a bool array, True = white."""
    buf = gray.astype(np.float32)
    h, w = buf.shape
    for y in range(h):
        row = buf[y]
        for x in range(w):
            old = row[x]
            new = 255.0 if old >= 128.0 else 0.0
            row[x] = new
            err = old - new
            if x + 1 < w:
                row[x + 1] += err * 7 / 16
            if y + 1 < h:
                nxt = buf[y + 1]
                if x:
                    nxt[x - 1] += err * 3 / 16
                nxt[x] += err * 5 / 16
                if x + 1 < w:
                    nxt[x + 1] += err * 1 / 16
    return buf >= 128.0


def pack_frame(bw, portrait):
    """bw: bool array indexed [y][x], True = white -- (122,250) landscape,
    (250,122) portrait.  Returns FRAME_BYTES of controller-RAM-order data."""
    if portrait:
        # logical (x,y) -> gate = y, bit = 121 - x
        canvas = np.ones((DISP_W, DISP_H), dtype=bool)     # 250 gates x 128 bits
        canvas[:, :DISP_VIS] = bw[:, ::-1]
    else:
        # logical (x,y) -> gate = x, bit = y
        canvas = np.ones((DISP_W, DISP_H), dtype=bool)
        canvas[:, :DISP_VIS] = bw.T

    # packbits is MSB-first along axis 1, which is exactly 0x80 >> (bit & 7)
    return np.packbits(canvas, axis=1).tobytes()


def unpack_frame(data, portrait):
    """Inverse of pack_frame, for --preview."""
    canvas = np.unpackbits(
        np.frombuffer(data, dtype=np.uint8).reshape(DISP_W, DISP_STRIDE), axis=1
    ).astype(bool)
    vis = canvas[:, :DISP_VIS]
    return vis[:, ::-1] if portrait else vis.T


def write_preview(pack_path, png_path, portrait, tiles=9, scale=2):
    """Contact sheet of frames read back out of the finished pack."""
    with open(pack_path, "rb") as f:
        count = struct.unpack(HDR_FMT, f.read(64))[6]
        n = min(tiles, count)
        idx = ([round(i * (count - 1) / (n - 1)) for i in range(n)]
               if n > 1 else [0])
        shots = []
        for i in idx:
            f.seek(64 + i * FRAME_BYTES)
            vis = unpack_frame(f.read(FRAME_BYTES), portrait)
            shots.append(np.where(vis, 255, 0).astype(np.uint8))

    th, tw = shots[0].shape
    cols = min(3, n)
    rows = (n + cols - 1) // cols
    gap = 6
    sheet = np.full((rows * (th + gap) + gap, cols * (tw + gap) + gap),
                    128, dtype=np.uint8)      # grey, so white frames show up
    for k, shot in enumerate(shots):
        r, c = divmod(k, cols)
        y0, x0 = gap + r * (th + gap), gap + c * (tw + gap)
        sheet[y0:y0 + th, x0:x0 + tw] = shot

    if scale > 1:
        sheet = cv2.resize(sheet, (sheet.shape[1] * scale, sheet.shape[0] * scale),
                           interpolation=cv2.INTER_NEAREST)
    cv2.imwrite(png_path, sheet)
    return idx


def iter_source(args):
    """Yield grayscale source frames along with the source frame rate."""
    if os.path.isdir(args.input):
        names = sorted(
            n for n in os.listdir(args.input) if n.lower().endswith(IMAGE_EXTS)
        )
        if not names:
            sys.exit(f"{args.input}: no images found")
        src_fps = args.src_fps or 30.0

        def gen():
            for n in names:
                img = cv2.imread(os.path.join(args.input, n), cv2.IMREAD_GRAYSCALE)
                if img is None:
                    sys.exit(f"{n}: cannot read")
                yield img

        return gen(), src_fps, len(names)

    cap = cv2.VideoCapture(args.input)
    if not cap.isOpened():
        sys.exit(f"{args.input}: cannot open")
    src_fps = args.src_fps or cap.get(cv2.CAP_PROP_FPS) or 30.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)

    def gen():
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            yield to_gray(frame)
        cap.release()

    return gen(), src_fps, total


def main():
    args = parse_args()

    if args.fps <= 0:
        sys.exit("--fps must be positive")

    source, src_fps, src_total = iter_source(args)
    step = src_fps / args.fps          # source frames per output frame
    first_src = int(round(args.start * src_fps))
    last_src = (
        first_src + int(round(args.duration * src_fps)) if args.duration else None
    )

    dst_w, dst_h = (DISP_VIS, DISP_W) if args.portrait else (DISP_W, DISP_VIS)
    pad_value = 255 if args.pad_color == "white" else 0

    lut = None
    if args.gamma != 1.0:
        lut = np.array(
            [min(255, int(round(255.0 * (i / 255.0) ** (1.0 / args.gamma))))
             for i in range(256)], dtype=np.uint8
        )

    print(f"source: {args.input}  {src_fps:.2f} fps"
          f"{f', {src_total} frames' if src_total else ''}")
    print(f"output: {args.output}  {dst_w}x{dst_h}"
          f"{' portrait' if args.portrait else ''} @ {args.fps} fps"
          f"  ({FRAME_BYTES} bytes/frame)")

    frame_us = int(round(1_000_000 / args.fps))
    written = 0
    next_src = float(first_src)

    with open(args.output, "wb") as out:
        out.write(struct.pack(HDR_FMT, EPDV_MAGIC, EPDV_VERSION, 0, 0, 0, 0, 0, 0))

        for i, gray in enumerate(source):
            if i < first_src:
                continue
            if last_src is not None and i >= last_src:
                break
            if i < next_src:
                continue
            next_src += step

            if lut is not None:
                gray = cv2.LUT(gray, lut)
            gray = fit_frame(gray, dst_w, dst_h, args.fit, pad_value)
            if args.dither == "fs":
                bw = dither_fs(gray)
            elif args.dither == "bayer":
                bw = dither_bayer(gray)
            else:
                bw = gray >= args.thresh
            if args.invert:
                bw = ~bw
            data = pack_frame(bw, args.portrait)

            out.write(data)
            written += 1
            if written % 100 == 0:
                print(f"\r  {written} frames packed", end="", flush=True)

        out.seek(0)
        out.write(struct.pack(HDR_FMT, EPDV_MAGIC, EPDV_VERSION,
                              EPDV_FLAG_PORTRAIT if args.portrait else 0,
                              dst_w, dst_h, frame_us, written, FRAME_BYTES))

    if not written:
        sys.exit("no frames written -- check --start/--duration")

    size_mb = (64 + written * FRAME_BYTES) / (1024 * 1024)
    print(f"\rpacked {written} frames, {size_mb:.1f} MB, "
          f"{written / args.fps:.1f} s of playback")

    if args.preview:
        idx = write_preview(args.output, args.preview, args.portrait)
        print(f"preview (unpacked from the file): {args.preview}  "
              f"frames {', '.join(str(i) for i in idx)}")


if __name__ == "__main__":
    main()
