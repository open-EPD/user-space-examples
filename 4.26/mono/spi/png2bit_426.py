import cv2
import argparse
import os

parser = argparse.ArgumentParser(description="Convert PNG to .h header for EPD")
parser.add_argument("input_png", type=str, help="Input PNG file")
parser.add_argument("-o", "--output", type=str, help="Output .h file (optional)")
parser.add_argument("-n", "--name", type=str, default="epd_image", help="Array name (default: epd_image)")
args = parser.parse_args()

input_file = args.input_png
output_file = "image_output.h"
array_name = args.name

# 讀取影像
im = cv2.imread(input_file, cv2.IMREAD_GRAYSCALE)
if im is None:
    print("❌ Cannot read image.")
    exit()

im = cv2.resize(im, (800, 480))
_, bw = cv2.threshold(im, 128, 255, cv2.THRESH_BINARY)
bw = cv2.flip(bw, 1)  # 左右翻轉（視需要）

# 轉成 byte array
bytes_per_row = 100
buffer = []

for y in range(480):
    for x_byte in range(bytes_per_row):
        byte = 0
        for bit in range(8):
            x = x_byte * 8 + bit
            pixel = bw[y, x]
            bit_val = 1 if pixel > 0 else 0
            byte = (byte << 1) | bit_val
        buffer.append(byte)

# 輸出成 .h 檔
with open(output_file, "w") as f:
    f.write(f"// Auto-generated from {os.path.basename(input_file)}\n")
    f.write(f"#ifndef {array_name.upper()}_H\n")
    f.write(f"#define {array_name.upper()}_H\n\n")
    f.write(f"#include <stdint.h>\n\n")
    f.write(f"const uint8_t {array_name}[48000] = {{\n")

    for i, byte in enumerate(buffer):
        if i % 12 == 0:
            f.write("    ")
        f.write(f"0x{byte:02X}, ")
        if (i + 1) % 12 == 0:
            f.write("\n")
    f.write("\n};\n\n")
    f.write(f"#endif // {array_name.upper()}_H\n")

print(f"✅ Generated: {output_file}")
