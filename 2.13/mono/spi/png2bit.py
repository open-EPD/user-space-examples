import cv2
import numpy as np
import os
import argparse

# --- 設定參數解析器 ---
parser = argparse.ArgumentParser(
    description="Convert a single PNG image into a minimal C const uint32_t array.",
    epilog="example: python image_converter.py test.png"
)
parser.add_argument("input_png", type=str, help="The name of the PNG file to convert.")
parser.add_argument("-o", "--output", type=str, help="Optional name of the output C file")

args = parser.parse_args()
target_png_file = args.input_png

try:
    p = os.path.dirname(os.path.abspath(__file__))
except NameError:
    p = os.getcwd()

imgpath = p + '/'

if not os.path.exists(os.path.join(imgpath, target_png_file)):
    print(f"Error：Cannot find the specific file '{target_png_file}'。")
    exit()

if args.output:
    output_c_file = args.output
else:
    base_name = os.path.splitext(target_png_file)[0]
    output_c_file = f"{base_name}_img.c"

path = os.path.join(p, output_c_file)

with open(path, 'w', encoding='utf-8') as f:
    img_name = target_png_file
    array_name = "img0"

    print(f"--- file is processing: {img_name} ---")

    full_img_path = os.path.join(imgpath, img_name)
    im = cv2.imread(full_img_path)

    if im is None:
        print(f"Error：Cannot read the picture '{img_name}'。")
        exit()

    im = cv2.cvtColor(im, cv2.COLOR_BGR2RGB)
    img_height = im.shape[0]
    img_width = im.shape[1]

    print(f"Image size (HxW): {img_height}x{img_width}")
    print(f"Will generated array: const uint32_t {array_name}[]")

    f.write(f"// Image data for: {img_name}\n")
    f.write(f"// Dimensions: {img_width} x {img_height}\n")
    f.write(f"const uint32_t {array_name}[] = {{\n")

    for x in range(img_height):
        f.write("    ")
        for y_chunk in range((img_width + 31) // 32):
            data = 0
            for y1 in range(32):
                y = y_chunk * 32 + y1
                if y >= img_width:
                    continue

                r, g, b = im[x, y]
                bit = 0 if (r, g, b) == (0, 0, 0) else 1

                data |= (bit << (31 - y1))

            f.write(f"0x{data:08X}, ")
        f.write("\n")
    f.write("};")

print(f"\n--- Finish！Already generated file: {output_c_file} ---")
