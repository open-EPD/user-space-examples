/*
 * Author: LC Wang <zaq14760@gmail.com>
 * Date: 2026-08-11
 *
 * Video player for the 2.13" mono PixPaper (250x122 landscape).
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

#define DISP_W 250
#define DISP_H 128
#define DISP_VIS 122
#define DISP_STRIDE (DISP_H / 8)
#define DISP_BUF_SIZE (DISP_W * DISP_STRIDE)
#define DISP_GATE_MAX (DISP_W - 1)

int spi_fd;

static uint32_t g_spi_speed = 5000000;

/* LUT byte 60 (phase-0 frame count) and the frame-rate code that fills
 * bytes 144..149; both overridable from the command line. */
static int g_phase0_tp = 0x10;
static int g_fr = 3;

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

void epd_waitUntilIdle() {
	sleep_ms(2);
	while (epd_busy_get() != 0)
		;
}

void epd_HWreset() {
	sleep_ms(50);
	epd_rst_set(0);
	sleep_ms(50);
	epd_rst_set(1);
	sleep_ms(50);
}

void epd_reg_init(void) {
	epd_waitUntilIdle();
	epd_writeCommand(0x12);		/* SW reset */
	epd_waitUntilIdle();

	epd_writeCommand(0x01);		/* driver output: 250 gate lines */
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x11);		/* data entry: Y decrement, X increment */
	epd_writeData(0x01);

	epd_writeCommand(0x44);		/* RAM X window: 0..15 (16 bytes) */
	epd_writeData(0x00);
	epd_writeData(0x0F);

	epd_writeCommand(0x45);		/* RAM Y window: 249..0 */
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x3C);		/* border waveform (full update) */
	epd_writeData(0x05);

	epd_writeCommand(0x21);		/* display update control 1 */
	epd_writeData(0x00);
	epd_writeData(0x80);

	epd_writeCommand(0x18);		/* internal temperature sensor */
	epd_writeData(0x80);

	epd_writeCommand(0x4E);		/* RAM X counter = 0 */
	epd_writeData(0x00);
	epd_writeCommand(0x4F);		/* RAM Y counter = 249 */
	epd_writeData(0xF9);
	epd_writeData(0x00);

	epd_waitUntilIdle();
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

	epd_HWreset();
	sleep_ms(1000);
	epd_waitUntilIdle();
	epd_reg_init();
}

/* xb* are RAM-X byte addresses (0x44/0x4E); g_* are 9-bit gate lines, counting
 * down (0x45/0x4F, data-entry mode 0x01). */
static void epd_set_window(int xb_start, int xb_end, int g_start, int g_end) {
	epd_writeCommand(0x44);
	epd_writeData(xb_start & 0xFF);
	epd_writeData(xb_end & 0xFF);

	epd_writeCommand(0x45);
	epd_writeData(g_start & 0xFF);
	epd_writeData((g_start >> 8) & 0xFF);
	epd_writeData(g_end & 0xFF);
	epd_writeData((g_end >> 8) & 0xFF);
}

static void epd_set_cursor(int xb, int g) {
	epd_writeCommand(0x4E);
	epd_writeData(xb & 0xFF);

	epd_writeCommand(0x4F);
	epd_writeData(g & 0xFF);
	epd_writeData((g >> 8) & 0xFF);
}

static void epd_set_full_window(void) {
	epd_set_window(0x00, DISP_STRIDE - 1, DISP_GATE_MAX, 0x00);
}

static void epd_set_full_cursor(void) {
	epd_set_cursor(0x00, DISP_GATE_MAX);
}

/* Seed both RAM banks (0x24 new, 0x26 old/diff) and full-refresh (0xF7). */
void epd_set_base_map(const uint8_t *buf) {
	epd_set_full_window();

	epd_set_full_cursor();
	epd_writeCommand(0x24);
	epd_writeData_bulk(buf, DISP_BUF_SIZE);

	epd_set_full_cursor();
	epd_writeCommand(0x26);
	epd_writeData_bulk(buf, DISP_BUF_SIZE);

	epd_writeCommand(0x22);
	epd_writeData(0xF7);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

/*
 * Partial-refresh waveform.  The panel's built-in OTP Mode-2 waveform leaves
 * static areas grey, so drive our own: 153 LUT bytes + 6 voltage bytes.
 */
static const uint8_t WF_PARTIAL[159] = {
	0x0, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x80, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x40, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x14, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,	/* end of 153 LUT bytes */
	0x22, 0x17, 0x41, 0x0, 0x32, 0x36,			/* EOPT, VGH, VSH1, VSH2, VSL, VCOM */
};

/* Upload the partial waveform + voltages (0x37 enables RAM ping-pong). */
void epd_load_partial_lut(void) {
	uint8_t fr_byte = ((g_fr & 7) << 4) | (g_fr & 7);

	epd_writeCommand(0x32);		/* LUT (153 bytes) */
	for (int i = 0; i < 153; i++) {
		uint8_t b = WF_PARTIAL[i];
		if (i == 60)
			b = (uint8_t)g_phase0_tp;
		else if (i >= 144 && i <= 149)
			b = fr_byte;
		epd_writeData(b);
	}
	epd_waitUntilIdle();

	epd_writeCommand(0x3F);		/* EOPT */
	epd_writeData(WF_PARTIAL[153]);
	epd_writeCommand(0x03);		/* gate voltage */
	epd_writeData(WF_PARTIAL[154]);
	epd_writeCommand(0x04);		/* source voltage */
	epd_writeData(WF_PARTIAL[155]);
	epd_writeData(WF_PARTIAL[156]);
	epd_writeData(WF_PARTIAL[157]);
	epd_writeCommand(0x2C);		/* VCOM */
	epd_writeData(WF_PARTIAL[158]);

	epd_writeCommand(0x37);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x40);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x3C);		/* partial border */
	epd_writeData(0x80);
}

/* Re-assert orientation/source registers after the reset pulse (no 0x12, so
 * the RAM banks survive for the ping-pong differential). */
static void epd_partial_regs(void) {
	epd_writeCommand(0x01);		/* 250 gate lines */
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x11);		/* data entry: Y decrement, X increment */
	epd_writeData(0x01);

	epd_writeCommand(0x21);		/* display update control 1 */
	epd_writeData(0x00);
	epd_writeData(0x80);

	epd_writeCommand(0x18);		/* internal temperature sensor */
	epd_writeData(0x80);
}

#define DISPLAY_PART_KEEP_ON 0x0C	/* display only, leave clock+analog on */

/* Enter partial mode once: reset pulse, registers, LUT, power up. */
void epd_partial_begin(void) {
	epd_rst_set(0);	/* reset pulse (RAM is preserved) */
	sleep_ms(2);
	epd_rst_set(1);
	sleep_ms(2);

	epd_partial_regs();
	epd_load_partial_lut();

	epd_writeCommand(0x22);		/* warm up: enable clock + analog */
	epd_writeData(0xC0);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();

	epd_set_full_window();
}

/*
 * One frame.  Write the whole frame to the new bank (0x24) ONLY: hardware
 * ping-pong keeps the last frame in the old bank and diffs against it, so only
 * changed pixels are driven.  Writing 0x26 here would fight the ping-pong and
 * scramble the image.
 */
void epd_partial_frame(const uint8_t *image) {
	epd_set_full_cursor();
	epd_writeCommand(0x24);
	epd_writeData_bulk(image, DISP_BUF_SIZE);

	epd_writeCommand(0x22);
	epd_writeData(DISPLAY_PART_KEEP_ON);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

/* Leave partial mode: power the analog back down. */
void epd_partial_end(void) {
	epd_writeCommand(0x22);
	epd_writeData(0x03);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

/* ---------------------------------------------------------------- player */

/*
 * .epdv frame pack, written by video2epd.py.  Little-endian.  Frames follow
 * the 64-byte header back to back, each already in controller RAM order
 * (1 = white, 0 = black).
 */
#define EPDV_MAGIC "EPDV"
#define EPDV_VERSION 1
#define EPDV_FLAG_PORTRAIT 0x0001

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

static volatile sig_atomic_t g_stop;

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
		fprintf(stderr, "%s: frame is %u bytes, this panel needs %d\n",
			path, c->hdr->frame_bytes, DISP_BUF_SIZE);
		goto fail;
	}

	c->count = c->hdr->frame_count;
	c->frame_us = c->hdr->frame_us ? c->hdr->frame_us : 200000;
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

/* Full white refresh; also re-seeds both RAM banks for the ping-pong. */
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
"Play a packed frame file on the 2.13\" mono PixPaper.  Build the pack with\n"
"video2epd.py (see README.md).\n"
"\n"
"  --loop           repeat the clip until Ctrl-C\n"
"  --free           show every frame as fast as the panel allows\n"
"  --fps N          play at N fps instead of the clip's own rate\n"
"  --start N        start at frame N\n"
"  --refresh N      full white refresh every N frames (0 = never, default)\n"
"  --tp N           LUT phase-0 frames, default %d; lower = faster, lighter\n"
"  --fr N           LUT frame-rate code 0..7, default %d\n"
"  --spi HZ         SPI clock, default %u\n"
"  --keep           leave the last frame on screen instead of clearing\n"
"  --quiet          no per-frame progress line\n"
"\n"
"Unless --free is given the player follows the wall clock and drops frames it\n"
"cannot keep up with, so the clip always runs for its own length.\n",
		prog, g_phase0_tp, g_fr, g_spi_speed);
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
		else if (!strcmp(a, "--tp"))
			g_phase0_tp = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "--fr"))
			g_fr = need_arg(argc, argv, &i, prog);
		else if (!strcmp(a, "--spi"))
			g_spi_speed = need_arg(argc, argv, &i, prog);
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

	printf("%s: %ux%u, %u frames, %.2f fps nominal (%.1f s)%s\n",
	       o.path, clip.hdr->width, clip.hdr->height, clip.count,
	       1000000.0 / clip.frame_us,
	       clip.count * clip.frame_us / 1000000.0,
	       (clip.hdr->flags & EPDV_FLAG_PORTRAIT) ? ", portrait" : "");
	printf("waveform: tp=%d fr=%d, spi=%u Hz, mode=%s\n",
	       g_phase0_tp, g_fr, g_spi_speed, o.free_run ? "free-run" : "clocked");

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
				/* whatever the tail overshot is dropped too */
				dropped += clip.count - 1 - last_idx;
				break;
			}
			t_start = now_us();
			o.start = 0;
			last_idx = -1;
			continue;
		}

		/* count the skipped frames only once we know they were real */
		dropped += idx - last_idx - 1;

		t0 = now_us();
		epd_partial_frame(clip_frame(&clip, (uint32_t)idx));
		t1 = now_us();

		t_render += t1 - t0;
		last_idx = idx;
		shown++;

		if (!o.quiet && (shown & 0x0F) == 0) {
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
