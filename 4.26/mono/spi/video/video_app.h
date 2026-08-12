/*
 * Author: LC Wang <zaq14760@gmail.com>
 * Date: 2026-08-11
 *
 * Video player for the 4.26" mono PixPaper (800x480).
 * Board-independent core: SPI, EPD command sequence, partial-refresh LUT and
 * the frame-pack player.
 *
 * The GPIO backend is board-specific and lives in the per-board .c file,
 * which must, before including this header, provide:
 *
 *   epd_dc_set(v)            set the DC line
 *   epd_rst_set(v)           set the RST line
 *   epd_busy_get()           read the BUSY line (1 = busy)
 *   int  epd_gpio_init(void)    acquire DC/RST/BUSY; return <0 on error
 *   void epd_gpio_release(void) release the lines and close the chip
 */
#ifndef VIDEO_APP_H
#define VIDEO_APP_H

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef EPD_SPI_DEVICE		/* a board .c may pick a different spidev node */
#define EPD_SPI_DEVICE "/dev/spidev0.0"
#endif

#define DISP_W 800
#define DISP_H 480
#define DISP_STRIDE (DISP_W / 8)		/* 100 bytes per row */
#define DISP_BUF_SIZE (DISP_STRIDE * DISP_H)	/* 48000 */

int spi_fd;

static uint32_t g_spi_speed = 20000000;
static volatile sig_atomic_t g_stop;

/* A wrong BUSY pin, or an unpowered panel, otherwise spins here forever --
 * and a tight spin cannot even be interrupted. */
#define EPD_BUSY_TIMEOUT_MS 5000

/* Named so a stall says which step stalled, not just that one did. */
static const char *g_wait_what = "startup";

/* 0x22 sequence run for each frame.  0x0C displays without cycling the analog
 * supply, which epd_partial_begin() has already powered up; the OTP path uses
 * 0xFF instead, which reloads that waveform on every frame. */
static int g_update_seq = 0x0C;

/*
 * Drive length in frames, and the frame period the LUT counts them in.  With
 * ping-pong on, only the pixels that change are driven, so a short drive still
 * looks clean: 4 frames at fr 4 runs about 16 fps with margin to spare.
 * --tp 0 falls back to the controller's OTP waveform, five times slower.
 */
static int g_phase0_tp = 4;
static int g_fr = 4;		/* -1 leaves the frame rate the OTP chose */

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

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

void spi_write(uint8_t *data, int len) {
	struct spi_ioc_transfer tr = {
	    .tx_buf = (unsigned long)data,
	    .len = len,
	    .speed_hz = g_spi_speed,
	    .bits_per_word = 8,
	};
	ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

void epd_writeCommand(uint8_t command) {
	epd_dc_set(0);
	sleep_us(1);
	spi_write(&command, 1);
}

void epd_writeData(uint8_t data) {
	epd_dc_set(1);
	sleep_us(1);
	spi_write(&data, 1);
}

void epd_writeData_bulk(const uint8_t *data, int len) {
	epd_dc_set(1);
	sleep_us(1);
	while (len > 0) {
		int chunk = len > 4096 ? 4096 : len;
		spi_write((uint8_t *)data, chunk);
		data += chunk;
		len -= chunk;
	}
}

void epd_waitUntilIdle(void) {
	uint64_t deadline = now_us() + EPD_BUSY_TIMEOUT_MS * 1000ull;

	sleep_ms(2);
	while (epd_busy_get() != 0) {
		if (g_stop)
			return;
		if (now_us() > deadline) {
			fprintf(stderr, "\nBUSY still asserted %d ms into '%s' "
				"-- check the BUSY pin, and that the panel is "
				"connected and powered\n",
				EPD_BUSY_TIMEOUT_MS, g_wait_what);
			exit(1);
		}
		sleep_us(200);
	}
}

void epd_HWreset() {
	sleep_ms(50);
	epd_rst_set(0);
	sleep_ms(50);
	epd_rst_set(1);
	sleep_ms(50);
}

void epd_reg_init(void) {
	g_wait_what = "reg init";
	epd_waitUntilIdle();

	epd_writeCommand(0x18);		/* internal temperature sensor */
	epd_writeData(0x80);

	epd_writeCommand(0x0C);		/* booster soft start */
	epd_writeData(0xAE);
	epd_writeData(0xC7);
	epd_writeData(0xC3);
	epd_writeData(0xC0);
	epd_writeData(0x80);

	epd_writeCommand(0x01);		/* driver output: 480 gate lines */
	epd_writeData((DISP_H - 1) & 0xFF);
	epd_writeData((DISP_H - 1) >> 8);
	epd_writeData(0x02);

	epd_writeCommand(0x3C);		/* border waveform (full update) */
	epd_writeData(0x01);

	epd_writeCommand(0x44);		/* RAM X window: 0..799 */
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData((DISP_W - 1) & 0xFF);
	epd_writeData((DISP_W - 1) >> 8);

	epd_writeCommand(0x45);		/* RAM Y window: 0..479 */
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData((DISP_H - 1) & 0xFF);
	epd_writeData((DISP_H - 1) >> 8);

	epd_waitUntilIdle();
}

static void epd_reset_ram_counters(void) {
	epd_writeCommand(0x4E);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x4F);
	epd_writeData(0x00);
	epd_writeData(0x00);
}

void epd_init(void) {
	spi_fd = open(EPD_SPI_DEVICE, O_RDWR);
	if (spi_fd < 0) {
		perror("Error opening SPI device");
		exit(1);
	}

	uint8_t spi_mode = SPI_MODE_0;
	ioctl(spi_fd, SPI_IOC_WR_MODE, &spi_mode);
	ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &g_spi_speed);

	if (epd_gpio_init() < 0)
		exit(1);

	g_wait_what = "power-on reset";
	epd_HWreset();
	sleep_ms(1000);
	epd_waitUntilIdle();
	epd_reg_init();
	epd_reset_ram_counters();
}

/* Seed both RAM banks and full-refresh, so the differential has a known base. */
void epd_set_base_map(const uint8_t *buf) {
	g_wait_what = "base map";
	epd_reset_ram_counters();
	epd_writeCommand(0x24);
	epd_writeData_bulk(buf, DISP_BUF_SIZE);

	epd_reset_ram_counters();
	epd_writeCommand(0x26);
	epd_writeData_bulk(buf, DISP_BUF_SIZE);

	epd_writeCommand(0x22);
	epd_writeData(0xF7);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

/*
 * Partial waveform.  Layout: bytes 0..49 are LUT0..LUT4, ten bytes each, one
 * byte per group holding four phases at two bits (A in the top bits;
 * 00 VSS, 01 VSH1, 10 VSL, 11 VSH2).  Bytes 50..99 are ten groups of
 * TP[A] TP[B] TP[C] TP[D] RP.  Bytes 100..104 set the frame rate.
 *
 * Which of LUT0..LUT3 a pixel uses depends on ping-pong: without it the LUT
 * follows the target colour alone, with it the pair (what is on the glass,
 * what we want).  wf_build() writes the second form, since epd_partial_begin()
 * always turns ping-pong on.  VSH1 drives toward black and VSL toward white --
 * that polarity is from the panel, which showed a negative with the other one,
 * not from the datasheet.
 */
#define WF_LUT_BYTES 100	/* 105 when a frame rate is set */
#define WF_TIMING_AT 50
#define WF_FRAMERATE_AT 100

static uint8_t wf_partial[WF_FRAMERATE_AT + 5];
static int wf_len = WF_LUT_BYTES;

/*
 * Panel-rated levels: 15 V / -15 V source swing, 20 V gate, -2.0 V VCOM.
 * The stock driver's LUT carries 12 V / -12 V and a 13 V gate instead, but
 * that belongs to its greyscale passes -- borrowing those for a black/white
 * refresh leaves the gate unable to switch the pixel TFTs fully, which reads
 * as an unclear image rather than merely a pale one.
 */
static uint8_t WF_VGH = 0x17;	/* 20 V, what the panel is rated for */
static uint8_t WF_VSH1 = 0x41;	/* 15 V */
static uint8_t WF_VSH2 = 0xA8;	/* 5 V, power-on default; unused here */
static uint8_t WF_VSL = 0x32;	/* -15 V */
static uint8_t WF_VCOM = 0x50;	/* -2.0 V */

static int wf_build(int tp, int fr) {
	memset(wf_partial, 0, sizeof(wf_partial));

	/*
	 * With ping-pong on, the LUT is picked by the pair (what is on the
	 * glass, what we want), so the two no-change combinations stay at VSS
	 * and static areas are never touched.  That is what lets a drive this
	 * short stay clean.  Phase byte is [A:7-6][B:5-4][C:3-2][D:1-0], and
	 * only phase A runs.
	 */
	wf_partial[0]  = 0x00;		/* black -> black: idle */
	wf_partial[10] = 0x80;		/* black -> white: VSL  */
	wf_partial[20] = 0x40;		/* white -> black: VSH1 */
	wf_partial[30] = 0x00;		/* white -> white: idle */

	wf_partial[WF_TIMING_AT] = (uint8_t)tp;		/* TP[0A] */

	if (fr < 0)			/* leave the frame rate the OTP set */
		return WF_LUT_BYTES;

	memset(wf_partial + WF_FRAMERATE_AT, ((fr & 0xF) << 4) | (fr & 0xF), 5);
	return WF_FRAMERATE_AT + 5;
}

void epd_load_partial_lut(void) {
	epd_writeCommand(0x03);		/* gate voltage */
	epd_writeData(WF_VGH);

	epd_writeCommand(0x04);		/* source voltage */
	epd_writeData(WF_VSH1);
	epd_writeData(WF_VSH2);
	epd_writeData(WF_VSL);

	epd_writeCommand(0x2C);		/* VCOM */
	epd_writeData(WF_VCOM);

	epd_writeCommand(0x32);
	epd_writeData_bulk(wf_partial, wf_len);
	epd_waitUntilIdle();
}

/*
 * Enter partial mode once.  The stock driver resets the panel and reprograms
 * these registers for every partial frame; hoisting that out of the loop is
 * most of what makes this a video player rather than a slideshow.
 */
void epd_partial_begin(void) {
	g_wait_what = "partial begin";
	epd_rst_set(0);
	sleep_ms(10);
	epd_rst_set(1);
	sleep_ms(10);
	epd_waitUntilIdle();

	epd_writeCommand(0x18);
	epd_writeData(0x80);

	epd_writeCommand(0x3C);		/* partial border */
	epd_writeData(0x80);

	{
		/*
		 * 0x37 byte F bit 6 turns on RAM ping-pong for display mode 2:
		 * the controller carries the displayed frame into the old bank
		 * itself and drives only the pixels that differ.  It is off at
		 * power-on, and without it every frame repaints the whole panel,
		 * which is what smears.  Mode 1 does not support it.
		 */
		static const uint8_t opt[10] = {
			0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0
		};
		epd_writeCommand(0x37);
		epd_writeData_bulk(opt, sizeof(opt));
	}

	if (g_phase0_tp > 0) {
		wf_len = wf_build(g_phase0_tp, g_fr);
		epd_load_partial_lut();
	}

	epd_writeCommand(0x44);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData((DISP_W - 1) & 0xFF);
	epd_writeData((DISP_W - 1) >> 8);

	epd_writeCommand(0x45);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData((DISP_H - 1) & 0xFF);
	epd_writeData((DISP_H - 1) >> 8);

	/* Power the analog up once here so a per-frame sequence does not have
	 * to.  A display-only sequence such as 0x0C needs this: with the
	 * analog off it would wait on a frame that can never finish. */
	epd_writeCommand(0x22);
	epd_writeData(0xC0);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

/*
 * Only 0x24.  On this controller the LUT is chosen by the pixel's target
 * colour, not by an old-to-new transition: 0x24 = 0 picks LUT0 (drive black)
 * and 0x24 = 1 picks LUT1 (drive white).  The 0x26 RAM is the red channel,
 * and on a black/white panel LUT2 and LUT3 are aliases of LUT0 and LUT1, so
 * its content cannot change what is displayed -- writing it per frame would
 * just double the SPI burst for nothing.
 */
void epd_partial_frame(const uint8_t *image) {
	g_wait_what = "frame";
	epd_reset_ram_counters();
	epd_writeCommand(0x24);
	epd_writeData_bulk(image, DISP_BUF_SIZE);

	epd_writeCommand(0x22);
	epd_writeData((uint8_t)g_update_seq);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

/*
 * A sequence ending in "disable analog, disable clock" -- 0xFF and friends --
 * has already powered the panel down, so there is nothing to do.  Only a
 * keep-on sequence leaves it running, and issuing 0x22 = 0x03 once the analog
 * is already off leaves BUSY asserted, so use deep sleep instead; the next
 * epd_HWreset() undoes it.
 */
void epd_partial_end(void) {
	if (g_update_seq & 0x03)
		return;

	epd_writeCommand(0x10);
	epd_writeData(0x01);
	sleep_ms(10);
}

/* ---------------------------------------------------------------- player */

/*
 * .epdv frame pack, written by video2epd.py.  Little-endian.  Frames follow
 * the 64-byte header back to back, each already in controller RAM order
 * (1 = white, 0 = black).
 */
#define EPDV_MAGIC "EPDV"
#define EPDV_VERSION 1

struct epdv_hdr {
	char     magic[4];
	uint16_t version;
	uint16_t flags;
	uint16_t width;		/* logical, informational */
	uint16_t height;
	uint32_t frame_us;	/* nominal display time per frame */
	uint32_t frame_count;
	uint32_t frame_bytes;
	uint32_t reserved[10];
};

_Static_assert(sizeof(struct epdv_hdr) == 64, "epdv header must be 64 bytes");

struct clip {
	const struct epdv_hdr *hdr;
	const uint8_t *frames;
	void *map;
	size_t map_len;
	uint32_t count;
	uint32_t frame_us;
};

static void on_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static int clip_open(struct clip *c, const char *path) {
	struct stat st;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(struct epdv_hdr)) {
		fprintf(stderr, "%s: too small to be an .epdv pack\n", path);
		close(fd);
		return -1;
	}

	c->map_len = st.st_size;
	c->map = mmap(NULL, c->map_len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (c->map == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	c->hdr = c->map;
	if (memcmp(c->hdr->magic, EPDV_MAGIC, 4)) {
		fprintf(stderr, "%s: not an .epdv pack\n", path);
		goto fail;
	}
	if (c->hdr->version != EPDV_VERSION) {
		fprintf(stderr, "%s: unsupported version %u\n", path,
			c->hdr->version);
		goto fail;
	}
	if (c->hdr->frame_bytes != DISP_BUF_SIZE) {
		fprintf(stderr, "%s: frame is %u bytes, this panel needs %d "
			"(is this a pack for a different panel?)\n",
			path, c->hdr->frame_bytes, DISP_BUF_SIZE);
		goto fail;
	}

	c->count = c->hdr->frame_count;
	c->frame_us = c->hdr->frame_us ? c->hdr->frame_us : 500000;
	if (!c->count ||
	    c->map_len < sizeof(*c->hdr) + (size_t)c->count * DISP_BUF_SIZE) {
		fprintf(stderr, "%s: truncated (%u frames declared)\n", path,
			c->count);
		goto fail;
	}

	c->frames = (const uint8_t *)c->map + sizeof(struct epdv_hdr);
	return 0;

fail:
	munmap(c->map, c->map_len);
	return -1;
}

static void clip_close(struct clip *c) {
	if (c->map)
		munmap(c->map, c->map_len);
	c->map = NULL;
}

static inline const uint8_t *clip_frame(const struct clip *c, uint32_t i) {
	return c->frames + (size_t)i * DISP_BUF_SIZE;
}

static uint8_t white_buf[DISP_BUF_SIZE];

/* Full white refresh; also re-seeds both RAM banks for the differential. */
static void epd_clear_white(void) {
	memset(white_buf, 0xFF, sizeof(white_buf));
	epd_HWreset();
	epd_reg_init();
	epd_set_base_map(white_buf);
}

struct opts {
	const char *path;
	int loop;
	int free_run;	/* ignore the clip's rate: show every frame, flat out */
	int fps;	/* 0 = use the clip's own rate */
	uint32_t start;
	uint32_t refresh;	/* full white refresh every N frames, 0 = never */
	int keep;	/* leave the last frame on screen at exit */
	int quiet;
};

static void usage(const char *prog) {
	fprintf(stderr,
"usage: %s [options] <clip.epdv>\n"
"\n"
"Play a packed frame file on the 4.26\" mono PixPaper.  Build the pack with\n"
"video2epd.py (see README.md).\n"
"\n"
"  --loop           repeat the clip until Ctrl-C\n"
"  --free           show every frame as fast as the panel allows\n"
"  --fps N          play at N fps instead of the clip's own rate\n"
"  --start N        start at frame N\n"
"  --refresh N      full white refresh every N frames (0 = never, default)\n"
"  --spi HZ         SPI clock, default %u; 48000 bytes a frame makes this\n"
"                   the first thing worth raising\n"
"  --update N       0x22 sequence per frame, default 0x%02X\n"
"  --tp N           drive frames per update, default %d; --tp 0 uses the\n"
"                   controller's OTP waveform instead (much slower)\n"
"  --fr N           LUT frame-rate code, how long one drive frame lasts,\n"
"                   default %d; too low and the gate scan cannot finish\n"
"  --keep           leave the last frame on screen instead of clearing\n"
"  --quiet          no per-frame progress line\n"
"\n"
"Unless --free is given the player follows the wall clock and drops frames it\n"
"cannot keep up with, so the clip always runs for its own length.\n",
		prog, g_spi_speed, g_update_seq, g_phase0_tp, g_fr);
}

/* --opt N: returns the value, or exits with the usage message. */
static long need_arg(int argc, char **argv, int *i, const char *prog) {
	char *end;
	long v;

	if (*i + 1 >= argc) {
		fprintf(stderr, "%s: missing value\n", argv[*i]);
		usage(prog);
		exit(2);
	}
	v = strtol(argv[++*i], &end, 0);
	if (*end) {
		fprintf(stderr, "%s: bad value '%s'\n", argv[*i - 1], argv[*i]);
		exit(2);
	}
	return v;
}

static int parse_opts(int argc, char **argv, struct opts *o) {
	const char *prog = argv[0];
	int update_given = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--loop"))
			o->loop = 1;
		else if (!strcmp(a, "--free"))
			o->free_run = 1;
		else if (!strcmp(a, "--keep"))
			o->keep = 1;
		else if (!strcmp(a, "--quiet"))
			o->quiet = 1;
		else if (!strcmp(a, "--fps"))
			o->fps = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "--start"))
			o->start = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "--refresh"))
			o->refresh = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "--spi"))
			g_spi_speed = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "--update")) {
			g_update_seq = need_arg(argc, argv, &i, prog);
			update_given = 1;
		}
		else if (!strcmp(a, "--tp"))
			g_phase0_tp = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "--fr"))
			g_fr = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(prog);
			exit(0);
		} else if (a[0] == '-') {
			fprintf(stderr, "unknown option: %s\n", a);
			usage(prog);
			return -1;
		} else if (!o->path) {
			o->path = a;
		} else {
			fprintf(stderr, "extra argument: %s\n", a);
			return -1;
		}
	}

	if (!o->path) {
		usage(prog);
		return -1;
	}
	if (o->fps < 0 || o->fps > 1000) {
		fprintf(stderr, "--fps out of range\n");
		return -1;
	}
	if (g_phase0_tp > 0xFF) {
		fprintf(stderr, "--tp must fit in a byte\n");
		return -1;
	}
	/* 0x10 in the update sequence loads the OTP waveform, and it does so
	 * before displaying -- so it would discard a custom LUT every frame. */
	if (g_phase0_tp <= 0) {
		if (!update_given)
			g_update_seq = 0xFF;
	} else if (update_given && (g_update_seq & 0x10)) {
		fprintf(stderr, "warning: --update 0x%02X reloads the OTP "
			"waveform, so --tp will have no effect\n", g_update_seq);
	}
	return 0;
}

int main(int argc, char **argv) {
	struct opts o = { 0 };
	struct clip clip = { 0 };
	uint64_t t_start, t_render = 0;
	uint32_t shown = 0, dropped = 0, since_refresh = 0;
	int64_t idx, last_idx;

	if (parse_opts(argc, argv, &o) < 0)
		return 2;
	if (clip_open(&clip, o.path) < 0)
		return 1;

	if (o.fps)
		clip.frame_us = 1000000u / (uint32_t)o.fps;
	if (o.start >= clip.count) {
		fprintf(stderr, "--start %u past the end (%u frames)\n",
			o.start, clip.count);
		clip_close(&clip);
		return 2;
	}

	printf("%s: %ux%u, %u frames, %.2f fps nominal (%.1f s)\n",
	       o.path, clip.hdr->width, clip.hdr->height, clip.count,
	       1000000.0 / clip.frame_us,
	       clip.count * clip.frame_us / 1000000.0);
	printf("spi=%u Hz (%.0f ms/frame of burst), update=0x%02X, waveform=%s,"
	       " mode=%s\n", g_spi_speed,
	       DISP_BUF_SIZE * 8.0 * 1000.0 / g_spi_speed, g_update_seq,
	       g_phase0_tp > 0 ? "custom" : "OTP",
	       o.free_run ? "free-run" : "clocked");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	epd_init();
	epd_clear_white();
	epd_partial_begin();

	t_start = now_us();
	last_idx = (int64_t)o.start - 1;

	while (!g_stop) {
		uint64_t t0, t1;

		if (o.free_run) {
			idx = last_idx + 1;
		} else {
			/* wall-clock playhead: which frame should be up now */
			idx = o.start + (int64_t)((now_us() - t_start) /
						  clip.frame_us);
			if (idx == last_idx) {
				/* ahead of schedule: wait out the frame */
				uint64_t due = t_start +
					(uint64_t)(idx + 1 - o.start) * clip.frame_us;
				uint64_t n = now_us();
				if (due > n)
					sleep_us(due - n);
				continue;
			}
		}

		if (idx >= (int64_t)clip.count) {
			if (!o.loop) {
				dropped += clip.count - 1 - last_idx;
				break;
			}
			t_start = now_us();
			o.start = 0;
			last_idx = -1;
			continue;
		}

		dropped += idx - last_idx - 1;

		t0 = now_us();
		epd_partial_frame(clip_frame(&clip, (uint32_t)idx));
		t1 = now_us();

		t_render += t1 - t0;
		last_idx = idx;
		shown++;

		if (!o.quiet && (shown & 0x07) == 0) {
			printf("\rframe %6ld/%u  shown %u  dropped %u  "
			       "%.1f ms/frame", (long)idx, clip.count, shown,
			       dropped, t_render / 1000.0 / shown);
			fflush(stdout);
		}

		if (o.refresh && ++since_refresh >= o.refresh) {
			since_refresh = 0;
			epd_partial_end();
			epd_clear_white();
			epd_partial_begin();
			if (!o.free_run)	/* the hiccup is not the clip's fault */
				t_start = now_us() -
					(uint64_t)(idx + 1 - o.start) * clip.frame_us;
		}
	}

	if (!o.quiet)
		printf("\n");

	epd_partial_end();
	if (!o.keep)
		epd_clear_white();

	if (shown)
		printf("done: %u frames shown, %u dropped, %.1f ms/frame "
		       "(%.2f fps sustained)\n", shown, dropped,
		       t_render / 1000.0 / shown, 1000000.0 * shown / t_render);

	clip_close(&clip);
	close(spi_fd);
	epd_gpio_release();
	return 0;
}

#endif /* VIDEO_APP_H */
