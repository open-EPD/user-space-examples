/*
 * Author: Wig Cheng <onlywig@gmail.com>
 * Date: 2025-02-21
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#include <time.h>
#include <stdint.h>
#include "png_HEX.h"
#define EPD_SPI_DEVICE "/dev/spidev0.0"
#define EPD_GPIO_CHIP "gpiochip0"

#define EPD_DC_PIN 0
#define EPD_RST_PIN 5
#define EPD_BUSY_PIN 26

#define SOURCE_BITS 122
#define GATE_BITS 250

#define SPI_SPEED 5000000

int spi_fd;
struct gpiod_chip *chip;
struct gpiod_line *epd_dc_line, *epd_rst_line, *epd_busy_line;


void sleep_ms(unsigned int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

void sleep_us(unsigned int microseconds) {
    struct timespec ts;
    ts.tv_sec = microseconds / 1000000;
    ts.tv_nsec = (microseconds % 1000000) * 1000;
    nanosleep(&ts, NULL);
}


void spi_write(uint8_t *data, int len) {
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)data,
        .len = len,
        .speed_hz = SPI_SPEED,
        .bits_per_word = 8,
    };
    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

void epd_writeCommand(uint8_t command){
    gpiod_line_set_value(epd_dc_line, 0);
    sleep_us(1);
    spi_write(&command, 1);
}

void epd_writeData(uint8_t data){
    gpiod_line_set_value(epd_dc_line, 1);
    sleep_us(1);
    spi_write(&data, 1);

}

void epd_waitUntilIdle(){
    sleep_ms(2);
    while(true){
        if(gpiod_line_get_value(epd_busy_line) == 1){
            break;
        }
    }
}

void epd_waitUntilIdle_N(){
    sleep_ms(2);
    while(true){
        if(gpiod_line_get_value(epd_busy_line) == 0){
            break;
        }
        sleep_ms(10);
    }
}

void epd_HWreset(){
    sleep_ms(50);
    gpiod_line_set_value(epd_rst_line, 0);
    sleep_ms(50);
    gpiod_line_set_value(epd_rst_line, 1);
    sleep_ms(50);
}

void epd_driverOutputControl_dataF(uint8_t* data, uint16_t A, uint8_t B){
    *(data+0) = (uint8_t)A;
    *(data+1) = (uint8_t)(A>>8);
    *(data+2) = (uint8_t)(B);
}

uint8_t img_fetch_hex_32(int y, int x, uint32_t* img_src){
    int hsize = 250/16 + (250%16==0 ? 0 : 1);
    if(y >=122 || x>=250){
        return 0b11;
    }
    else{
        uint32_t target = *(img_src+hsize*y + (int)(x/16));
        return (target)>>((15 - x%16)*2) & 0b11;
    }
}

void epd_init(){

    spi_fd = open(EPD_SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) {
        perror("Error opening SPI device");
        exit(1);
    }


    uint8_t mode = SPI_MODE_0;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);


    uint32_t speed = SPI_SPEED;
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    chip = gpiod_chip_open_by_name(EPD_GPIO_CHIP);
    if (!chip) {
        perror("Error opening GPIO chip");
        exit(1);
    }

    epd_dc_line = gpiod_chip_get_line(chip, EPD_DC_PIN);
    epd_rst_line = gpiod_chip_get_line(chip, EPD_RST_PIN);
    epd_busy_line = gpiod_chip_get_line(chip, EPD_BUSY_PIN);

    if (!epd_dc_line || !epd_rst_line || !epd_busy_line) {
        perror("Error getting GPIO lines");
        gpiod_chip_close(chip);
        exit(1);
    }

    gpiod_line_request_output(epd_dc_line, "epd_dc", 0);
    gpiod_line_request_output(epd_rst_line, "epd_rst", 0);
    gpiod_line_request_input(epd_busy_line, "epd_busy");

    printf("start\n");
    epd_HWreset();
    sleep_ms(1000);
    epd_waitUntilIdle();
    printf("reset successed\n");

    epd_writeCommand(0x4D);
    epd_writeData(0x78);
    epd_waitUntilIdle();

    epd_writeCommand(0x00); //PSR
    epd_writeData(0b00001111);
    epd_writeData(0b00001001);
    epd_waitUntilIdle();

    epd_writeCommand(0x01); //PWRR
    epd_writeData(0x07);
    epd_writeData(0x00);
    epd_writeData(0x22);
    epd_writeData(0x78);
    epd_writeData(0x0A);
    epd_writeData(0x22);
    epd_waitUntilIdle();

    epd_writeCommand(0x03); //POFS
    epd_writeData(0x10);
    epd_writeData(0x54);
    epd_writeData(0x44);
    epd_waitUntilIdle();

    epd_writeCommand(0x06); //BTST_P
    epd_writeData(0x0F);
    epd_writeData(0x0A);
    epd_writeData(0x2F);
    epd_writeData(0x25);
    epd_writeData(0x22);
    epd_writeData(0x2E);
    epd_writeData(0x21);
    epd_waitUntilIdle();

    epd_writeCommand(0x30); //CDI
    epd_writeData(0x02);
    epd_waitUntilIdle();

    epd_writeCommand(0x41);
    epd_writeData(0x00);
    epd_waitUntilIdle();

    epd_writeCommand(0x50);
    epd_writeData(0x37);
    epd_waitUntilIdle();

    epd_writeCommand(0x60);
    epd_writeData(0x02);
    epd_writeData(0x02);
     epd_waitUntilIdle();

    epd_writeCommand(0x61);
    epd_writeData(0x00);   // Source_BITS_H
    epd_writeData(0x80);   // Source_BITS_L
    epd_writeData(0x00);   // Gate_BITS_H
    epd_writeData(0xFA);
    epd_waitUntilIdle();

    epd_writeCommand(0x65);
    epd_writeData(0x00);
    epd_writeData(0x00);
    epd_writeData(0x00);
    epd_writeData(0x00);
    epd_waitUntilIdle();

    epd_writeCommand(0xE7);
    epd_writeData(0x1C);
    epd_waitUntilIdle();

    epd_writeCommand(0xE3);
    epd_writeData(0x22);
    epd_waitUntilIdle();

    epd_writeCommand(0xE0);
    epd_writeData(0x00);
    epd_waitUntilIdle();

    epd_writeCommand(0xB4);
    epd_writeData(0xD0);
    epd_waitUntilIdle();

    epd_writeCommand(0xB5);
    epd_writeData(0x03);
    epd_waitUntilIdle();

    epd_writeCommand(0xE9);
    epd_writeData(0x01);
    epd_waitUntilIdle();
}

void epd_write_img(uint32_t* img_src){
    epd_writeCommand(0x10);
    sleep_ms(10);

    for(uint i=0;i<250;i++)
    {
        for(uint j=0;j<32;j++){
            uint8_t ty = (img_fetch_hex_32(j*4+0,i, img_src)<<6) | (img_fetch_hex_32(j*4+1,i, img_src)<<4) | (img_fetch_hex_32(j*4+2,i, img_src)<<2) | (img_fetch_hex_32(j*4+3,i, img_src)<<0);
           epd_writeData(ty);
        }
    }

    epd_waitUntilIdle();

    printf("wait\n");
    printf("write image successed, starting update image\n");

    epd_writeCommand(0x04);
    sleep_ms(10);
    epd_writeData(0x00);
    epd_waitUntilIdle();

    epd_writeCommand(0x12);
    sleep_ms(10);
    epd_writeData(0x00);
    epd_waitUntilIdle();
    printf("update image successed\n");

    gpiod_line_set_value(epd_rst_line, 0);
    gpiod_line_set_value(epd_dc_line, 0);
}

int main(){
    while(true){
        epd_init();
        epd_write_img(&img_hex[0][0]);
        sleep_ms(30*1000);
    }


    close(spi_fd);
    gpiod_line_release(epd_dc_line);
    gpiod_line_release(epd_rst_line);
    gpiod_line_release(epd_busy_line);
    gpiod_chip_close(chip);

    return 0;
}
