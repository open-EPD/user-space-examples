import cv2
import math

im = cv2.imread("test.png")
if im is None:
    print("Error: Could not read image 'test.png'")
    exit()

im = cv2.cvtColor(im, cv2.COLOR_BGR2RGB)
filename = "png_HEX.h"

f = open(filename, 'w')
f.write("#ifndef PNG_HEX_H\n")
f.write("#define PNG_HEX_H\n\n")
f.write("#include <stdint.h>\n\n")

width_words = int(im.shape[1]/16)+(1,0)[im.shape[1]%16 == 0]
f.write("static uint32_t img_hex[{:d}][{:d}] = {{\n".format(im.shape[0], width_words))

for x in range(im.shape[0]):
    f.write("    {")
    for y_word in range(width_words):
        dcd = 0b00
        for j in range(16):
            pixel_index_col = im.shape[1] - 1 - (y_word * 16 + j)
            if(pixel_index_col < 0):
                continue
            if(pixel_index_col >= im.shape[1]):
                continue

            r, g, b = im[x, pixel_index_col]
            dxd = 0b01  # default
            if((r,g,b) == (0xFF,0xFF,0x00)):  # Yellow
                dxd=0b10
            elif((r,g,b) == (0x00,0x00,0x00)):  # Black
                dxd=0b00
            elif((r,g,b) == (0xFF,0x00,0x00)):  # Red
                dxd=0b11
            dcd = dxd<<((15-j)*2) | dcd
        f.write("0x{:08x}, ".format(dcd))
    f.write("}, \n")
f.write("};\n\n")

f.write("#define IMG_HEIGHT {:d}\n".format(im.shape[0]))
f.write("#define IMG_WIDTH {:d}\n".format(im.shape[1]))
f.write("#define IMG_WIDTH_WORDS {:d}\n".format(width_words))

f.write("#endif /* PNG_HEX_H */\n")
f.close()

print(f"Successfully created {filename}")
