import cv2
import math

im = cv2.imread("test.png")
im = cv2.cvtColor(im, cv2.COLOR_BGR2RGB)
path = 'test' + "_HEX.txt"
f = open(path, 'w')
f.write("uint32_t img1_hex[{:d}][{:d}] = {{\n".format(im.shape[0], int(im.shape[1]/16+(1,0)[im.shape[1]%16 == 0])))


for x in range(im.shape[0]):
    f.write("{")
    for y in range(int(im.shape[1]/16)+(1,0)[im.shape[1]%16 == 0]):
        dcd = 0b00
        for j in range(16):
            dxd=0b01
            if(y*16+j >= im.shape[1]):
                continue
            r, g, b = im[x, y*16+j]
            if((r,g,b) == (0xFF,0xFF,0x00)):
                dxd=0b10
            if((r,g,b) == (0x00,0x00,0x00)):
                dxd=0b00
            if((r,g,b) == (0xFF,0x00,0x00)):
                dxd=0b11
            dcd = dxd<<((15-j)*2) | dcd
        f.write("0x{:08x},".format(dcd))
    f.write("}, \n")
f.write("};")
f.close()
