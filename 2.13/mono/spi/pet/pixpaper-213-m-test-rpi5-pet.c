/*
 * Author: LC Wang <zaq14760@gmail.com>
 * Date: 2026-06-17
 *
 * Open-EP Pet for Raspberry Pi 5: libgpiod v2 GPIO backend.
 * Shared SPI/EPD/pet logic lives in pet_app.h.
 */
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>

#define EPD_GPIO_CHIP "gpiochip15"

#define EPD_DC_PIN 5
#define EPD_RST_PIN 6
#define EPD_BUSY_PIN 26

struct gpiod_chip *chip;
struct gpiod_line_request *epd_dc_line, *epd_rst_line, *epd_busy_line;

static const unsigned int EPD_DC_OFFSET   = EPD_DC_PIN;
static const unsigned int EPD_RST_OFFSET  = EPD_RST_PIN;
static const unsigned int EPD_BUSY_OFFSET = EPD_BUSY_PIN;

static struct gpiod_line_request *
gpiod_request_output(struct gpiod_chip *c, unsigned int offset,
		     const char *consumer, int value)
{
	struct gpiod_line_settings *settings;
	struct gpiod_line_config *line_cfg;
	struct gpiod_request_config *req_cfg;
	struct gpiod_line_request *request = NULL;

	settings = gpiod_line_settings_new();
	if (!settings)
		return NULL;
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings,
		value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);

	line_cfg = gpiod_line_config_new();
	if (!line_cfg) {
		gpiod_line_settings_free(settings);
		return NULL;
	}
	if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0)
		goto out;

	req_cfg = gpiod_request_config_new();
	if (req_cfg)
		gpiod_request_config_set_consumer(req_cfg, consumer);

	request = gpiod_chip_request_lines(c, req_cfg, line_cfg);

	if (req_cfg)
		gpiod_request_config_free(req_cfg);
out:
	gpiod_line_config_free(line_cfg);
	gpiod_line_settings_free(settings);
	return request;
}

static struct gpiod_line_request *
gpiod_request_input(struct gpiod_chip *c, unsigned int offset,
		    const char *consumer)
{
	struct gpiod_line_settings *settings;
	struct gpiod_line_config *line_cfg;
	struct gpiod_request_config *req_cfg;
	struct gpiod_line_request *request = NULL;

	settings = gpiod_line_settings_new();
	if (!settings)
		return NULL;
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);

	line_cfg = gpiod_line_config_new();
	if (!line_cfg) {
		gpiod_line_settings_free(settings);
		return NULL;
	}
	if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0)
		goto out;

	req_cfg = gpiod_request_config_new();
	if (req_cfg)
		gpiod_request_config_set_consumer(req_cfg, consumer);

	request = gpiod_chip_request_lines(c, req_cfg, line_cfg);

	if (req_cfg)
		gpiod_request_config_free(req_cfg);
out:
	gpiod_line_config_free(line_cfg);
	gpiod_line_settings_free(settings);
	return request;
}

static inline void
gpiod_set_value(struct gpiod_line_request *req, unsigned int offset, int value)
{
	gpiod_line_request_set_value(req, offset,
		value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

static inline int
gpiod_get_value(struct gpiod_line_request *req, unsigned int offset)
{
	return gpiod_line_request_get_value(req, offset) == GPIOD_LINE_VALUE_ACTIVE
		? 1 : 0;
}

#define epd_dc_set(v)    gpiod_set_value(epd_dc_line,   EPD_DC_OFFSET,   (v))
#define epd_rst_set(v)   gpiod_set_value(epd_rst_line,  EPD_RST_OFFSET,  (v))
#define epd_busy_get()   gpiod_get_value(epd_busy_line, EPD_BUSY_OFFSET)

static int epd_gpio_init(void) {
	chip = gpiod_chip_open("/dev/" EPD_GPIO_CHIP);
	if (!chip) {
		perror("Error opening GPIO chip");
		return -1;
	}

	epd_dc_line   = gpiod_request_output(chip, EPD_DC_OFFSET,   "epd_dc", 0);
	epd_rst_line  = gpiod_request_output(chip, EPD_RST_OFFSET,  "epd_rst", 0);
	epd_busy_line = gpiod_request_input(chip,  EPD_BUSY_OFFSET, "epd_busy");

	if (!epd_dc_line || !epd_rst_line || !epd_busy_line) {
		perror("Error requesting GPIO lines");
		gpiod_chip_close(chip);
		return -1;
	}

	return 0;
}

static void epd_gpio_release(void) {
	gpiod_line_request_release(epd_dc_line);
	gpiod_line_request_release(epd_rst_line);
	gpiod_line_request_release(epd_busy_line);
	gpiod_chip_close(chip);
}

#include "pet_app.h"
