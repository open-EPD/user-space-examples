/*
 * Author: LC Wang <zaq14760@gmail.com>
 * Date: 2026-06-17
 *
 * Open-EP Pet for frdm-imx93: libgpiod v1 GPIO backend.
 * Shared SPI/EPD/pet logic lives in pet_app.h.
 */
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>

#define EPD_GPIO_CHIP "gpiochip0"

#define EPD_DC_PIN 0
#define EPD_RST_PIN 5
#define EPD_BUSY_PIN 26

struct gpiod_chip *chip;
struct gpiod_line *epd_dc_line, *epd_rst_line, *epd_busy_line;

#define epd_dc_set(v)    gpiod_line_set_value(epd_dc_line,  (v))
#define epd_rst_set(v)   gpiod_line_set_value(epd_rst_line, (v))
#define epd_busy_get()   gpiod_line_get_value(epd_busy_line)

static int epd_gpio_init(void) {
	chip = gpiod_chip_open_by_name(EPD_GPIO_CHIP);
	if (!chip) {
		perror("Error opening GPIO chip");
		return -1;
	}

	epd_dc_line   = gpiod_chip_get_line(chip, EPD_DC_PIN);
	epd_rst_line  = gpiod_chip_get_line(chip, EPD_RST_PIN);
	epd_busy_line = gpiod_chip_get_line(chip, EPD_BUSY_PIN);

	if (!epd_dc_line || !epd_rst_line || !epd_busy_line) {
		perror("Error getting GPIO lines");
		gpiod_chip_close(chip);
		return -1;
	}

	if (gpiod_line_request_output(epd_dc_line,  "epd_dc",  0) < 0 ||
	    gpiod_line_request_output(epd_rst_line, "epd_rst", 0) < 0 ||
	    gpiod_line_request_input(epd_busy_line, "epd_busy")   < 0) {
		perror("Error requesting GPIO lines");
		gpiod_chip_close(chip);
		return -1;
	}

	return 0;
}

static void epd_gpio_release(void) {
	gpiod_line_release(epd_dc_line);
	gpiod_line_release(epd_rst_line);
	gpiod_line_release(epd_busy_line);
	gpiod_chip_close(chip);
}

#include "pet_app.h"
