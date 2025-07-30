/*
 * Author: Wig Cheng <onlywig@gmail.com>
 * Date: 2025-07-30
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "image_output.h"

#define EPD_SPI_DEVICE "/dev/spidev0.0"
#define EPD_GPIO_CHIP "gpiochip0"

#define EPD_DC_PIN 0
#define EPD_RST_PIN 5
#define EPD_BUSY_PIN 26

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

void epd_writeCommand(uint8_t command) {
	gpiod_line_set_value(epd_dc_line, 0);
	sleep_us(1);
	spi_write(&command, 1);
}

void epd_writeData(uint8_t data) {
	gpiod_line_set_value(epd_dc_line, 1);
	sleep_us(1);
	spi_write(&data, 1);
}

void epd_waitUntilIdle() {
	sleep_ms(2);
	while (true) {
		if (gpiod_line_get_value(epd_busy_line) == 0) {
			break;
		}
	}
}

void epd_HWreset() {
	sleep_ms(50);
	gpiod_line_set_value(epd_rst_line, 0);
	sleep_ms(50);
	gpiod_line_set_value(epd_rst_line, 1);
	sleep_ms(50);
}

void epd_init() {

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

	epd_writeCommand(0x18);
    	epd_writeData(0x80);

    	epd_writeCommand(0x0C);
    	epd_writeData(0xAE);
    	epd_writeData(0xC7);
	epd_writeData(0xC3);
    	epd_writeData(0xC0);
    	epd_writeData(0x80);

        epd_writeCommand(0x01);
    	epd_writeData((480 - 1) & 0xFF);
    	epd_writeData((480 - 1) >> 8);
    	epd_writeData(0x02);

 	epd_writeCommand(0x3C);
    	epd_writeData(0x01);

    	epd_writeCommand(0x44);
    	epd_writeData(0x00);
    	epd_writeData(0x00);
    	epd_writeData((800 - 1) & 0xFF);
    	epd_writeData((800 - 1) >> 8);

    	epd_writeCommand(0x45);
    	epd_writeData(0x00);
    	epd_writeData(0x00);
    	epd_writeData((480 - 1) & 0xFF);
    	epd_writeData((480 - 1) >> 8);

    	epd_writeCommand(0x4E);
    	epd_writeData(0x00);
    	epd_writeData(0x00);
    	epd_writeCommand(0x4F);
    	epd_writeData(0x00);
    	epd_writeData(0x00);

    	epd_writeCommand(0x24);

    	for (int i = 0; i < 48000; i++) {
            epd_writeData(epd_image[i]);
    	}

 	epd_writeCommand(0x21);
    	epd_writeData(0x40);
    	epd_writeData(0x00);

    	epd_writeCommand(0x22);
    	epd_writeData(0xF7);
    	epd_writeCommand(0x20);
    	epd_waitUntilIdle();
}

void epd_write_img() {
    	epd_writeCommand(0x24);

    	for (int i = 0; i < 48000; i++) {
            epd_writeData(epd_image[i]);
    	}

 	epd_writeCommand(0x21);
    	epd_writeData(0x40);
    	epd_writeData(0x00);

    	epd_writeCommand(0x22);
    	epd_writeData(0xF7);
    	epd_writeCommand(0x20);
    	epd_waitUntilIdle();
}

int main() {
	while (true) {
		epd_init();
		epd_write_img();
		sleep_ms(30 * 1000);
	}

	close(spi_fd);
	gpiod_line_release(epd_dc_line);
	gpiod_line_release(epd_rst_line);
	gpiod_line_release(epd_busy_line);
	gpiod_chip_close(chip);

	return 0;
}
