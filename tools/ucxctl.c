// SPDX-License-Identifier: MIT
/*
 * ucxctl - talk to an RME Fireface UCX in RME driver mode from userspace.
 *
 * A safe place to test protocol guesses before they go into the kernel
 * module: a crash here costs a process, not the machine.
 */
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb.h>

#include "ucx_proto.h"

#define TIMEOUT_MS 1000
/* Mixer sources of one kind: indices 0x00-0x1f or 0x20-0x3f */
#define UCX_MIX_SOURCES_MAX 32

static const uint16_t ucx_vids[] = { UCX_VID_RME, UCX_VID_MICROCHIP };

static void hexdump(const unsigned char *buf, int len)
{
	for (int i = 0; i < len; i += 16) {
		printf("%04x:", i);
		for (int j = i; j < i + 16 && j < len; j++)
			printf(" %02x", buf[j]);
		putchar('\n');
	}
}

static const char *xfer_type_name(uint8_t attributes)
{
	static const char *const names[] = { "control", "isoc", "bulk", "interrupt" };

	return names[attributes & LIBUSB_TRANSFER_TYPE_MASK];
}

static int is_ucx_vid(uint16_t vid)
{
	for (size_t i = 0; i < sizeof(ucx_vids) / sizeof(ucx_vids[0]); i++)
		if (vid == ucx_vids[i])
			return 1;
	return 0;
}

/* Descriptors are readable without permissions, so find the mode before opening */
static libusb_device_handle *open_ucx(libusb_context *ctx)
{
	libusb_device **list;
	libusb_device_handle *h = NULL;
	int cc_mode = 0, found = 0;
	ssize_t n;

	n = libusb_get_device_list(ctx, &list);
	for (ssize_t i = 0; i < n && !found; i++) {
		struct libusb_device_descriptor dd;
		int err;

		if (libusb_get_device_descriptor(list[i], &dd) || !is_ucx_vid(dd.idVendor))
			continue;
		if (dd.idProduct == UCX_PID_CC_MODE)
			cc_mode = 1;
		if (dd.idProduct != UCX_PID_RME_MODE)
			continue;
		found = 1;
		err = libusb_open(list[i], &h);
		if (err)
			fprintf(stderr, "Cannot open the UCX: %s%s\n", libusb_strerror(err),
				err == LIBUSB_ERROR_ACCESS ? " (run as root or add a udev rule)" : "");
	}
	if (n >= 0)
		libusb_free_device_list(list, 1);

	if (!found)
		fprintf(stderr, cc_mode ?
			"The UCX is in class-compliant mode (PID 3fb9); switch it to RME driver mode first.\n" :
			"No Fireface UCX found.\n");
	return h;
}

/* Interface number that owns endpoint @ep in the active configuration, or -1 */
static int interface_for_ep(libusb_device_handle *h, uint8_t ep)
{
	struct libusb_config_descriptor *cfg;
	int found = -1;

	if (libusb_get_active_config_descriptor(libusb_get_device(h), &cfg))
		return -1;
	for (int i = 0; i < cfg->bNumInterfaces && found < 0; i++) {
		const struct libusb_interface *intf = &cfg->interface[i];

		for (int a = 0; a < intf->num_altsetting && found < 0; a++) {
			const struct libusb_interface_descriptor *alt = &intf->altsetting[a];

			for (int e = 0; e < alt->bNumEndpoints; e++)
				if (alt->endpoint[e].bEndpointAddress == ep)
					found = alt->bInterfaceNumber;
		}
	}
	libusb_free_config_descriptor(cfg);
	return found;
}

static int cmd_info(libusb_device_handle *h)
{
	libusb_device *dev = libusb_get_device(h);
	struct libusb_device_descriptor dd;
	struct libusb_config_descriptor *cfg;
	int err;

	err = libusb_get_device_descriptor(dev, &dd);
	if (err)
		return err;
	printf("device %04x:%04x bcdDevice %04x, %u configuration(s)\n",
	       dd.idVendor, dd.idProduct, dd.bcdDevice, dd.bNumConfigurations);

	err = libusb_get_active_config_descriptor(dev, &cfg);
	if (err)
		return err;
	for (int i = 0; i < cfg->bNumInterfaces; i++) {
		const struct libusb_interface *intf = &cfg->interface[i];

		for (int a = 0; a < intf->num_altsetting; a++) {
			const struct libusb_interface_descriptor *alt = &intf->altsetting[a];

			printf("  interface %u alt %u class %02x/%02x/%02x\n",
			       alt->bInterfaceNumber, alt->bAlternateSetting,
			       alt->bInterfaceClass, alt->bInterfaceSubClass,
			       alt->bInterfaceProtocol);
			for (int e = 0; e < alt->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];

				printf("    ep 0x%02x %-9s maxpacket %u\n", ep->bEndpointAddress,
				       xfer_type_name(ep->bmAttributes), ep->wMaxPacketSize);
			}
		}
	}
	libusb_free_config_descriptor(cfg);
	return 0;
}

static int cmd_vendor(libusb_device_handle *h, int in, int argc, char **argv)
{
	unsigned char buf[4096];
	uint8_t type = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE;
	unsigned long req, value, index, len = 0;
	int ret;

	if (argc < 3 + in) {
		fprintf(stderr, "usage: vendor-%s <request> <wValue> <wIndex>%s\n",
			in ? "in" : "out", in ? " <length>" : "");
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	req = strtoul(argv[0], NULL, 0);
	value = strtoul(argv[1], NULL, 0);
	index = strtoul(argv[2], NULL, 0);
	if (in) {
		len = strtoul(argv[3], NULL, 0);
		if (len > sizeof(buf))
			len = sizeof(buf);
	}

	if (req == UCX_REQ_FLASH_WRITE || req == UCX_REQ_FLASH_STATUS) {
		fprintf(stderr, "refusing request 0x%02lx: it belongs to the flash programming\n",
			req);
		return LIBUSB_ERROR_ACCESS;
	}

	type |= in ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT;
	ret = libusb_control_transfer(h, type, req, value, index, buf, len, TIMEOUT_MS);
	if (ret < 0)
		return ret;
	if (in)
		hexdump(buf, ret);
	return 0;
}

static int cmd_ctrl_read(libusb_device_handle *h)
{
	unsigned char buf[UCX_CTRL_BLOCK_SIZE];
	int intf, got, err;

	intf = interface_for_ep(h, UCX_EP_CTRL_IN);
	if (intf < 0) {
		fprintf(stderr, "endpoint 0x%02x not found\n", UCX_EP_CTRL_IN);
		return LIBUSB_ERROR_NOT_FOUND;
	}
	libusb_set_auto_detach_kernel_driver(h, 1);
	err = libusb_claim_interface(h, intf);
	if (err)
		return err;

	err = libusb_bulk_transfer(h, UCX_EP_CTRL_IN, buf, sizeof(buf), &got, TIMEOUT_MS);
	if (!err || err == LIBUSB_ERROR_TIMEOUT)
		hexdump(buf, got);
	libusb_release_interface(h, intf);
	return err;
}

/*
 * Streaming the way the Windows driver does it (0x14000ccb0, 0x140010a00):
 * select the speed-class alt setting, set the REG1 buffer-size bit, send
 * START_AT (now), then keep
 * UCX_URBS_PER_EP transfers queued per endpoint. OUT sends silence.
 */
#define STREAM_BLOCKS_PER_XFER	32	/* 256 frames at single speed */

struct stream {
	FILE *raw;
	int running;
	int pending;
	int dumped;
	unsigned int block_bytes;
	unsigned int channels;
	/* Test tone on OUT; tone_slot < 0 sends silence */
	int tone_slot;
	double amp, phase, phase_inc;
	unsigned long long tone_frames, tone_total, fade_frames;
	unsigned long long in_bytes, out_bytes;
	unsigned long in_xfers, out_xfers, short_in, errors;
	struct timespec first_in, last_in;
};

static volatile sig_atomic_t stop_requested;

static void on_sigint(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static double ts_diff(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) + (b->tv_nsec - a->tv_nsec) / 1e9;
}

static const char *xfer_status_name(enum libusb_transfer_status status)
{
	switch (status) {
	case LIBUSB_TRANSFER_TIMED_OUT:
		return "timed out";
	case LIBUSB_TRANSFER_STALL:
		return "stall";
	case LIBUSB_TRANSFER_NO_DEVICE:
		return "no device";
	case LIBUSB_TRANSFER_OVERFLOW:
		return "overflow";
	default:
		return "error";
	}
}

/* -1.0..1.0 as the device wants it: int32 LE with 24 valid bits at the top */
static void put_sample(unsigned char *p, double v)
{
	uint32_t u = (uint32_t)(int32_t)lrint(v * 8388607.0) << 8;

	p[0] = u;
	p[1] = u >> 8;
	p[2] = u >> 16;
	p[3] = u >> 24;
}

static void fill_out(struct stream *s, unsigned char *buf, int len)
{
	int frame_bytes = s->channels * UCX_SAMPLE_BYTES;

	memset(buf, 0, len);
	if (s->tone_slot < 0)
		return;
	for (int off = 0; off + frame_bytes <= len; off += frame_bytes) {
		unsigned long long n = s->tone_frames++;
		double gain = 1.0;

		if (n >= s->tone_total)
			continue;
		if (n < s->fade_frames)
			gain = (double)n / s->fade_frames;
		else if (s->tone_total - n < s->fade_frames)
			gain = (double)(s->tone_total - n) / s->fade_frames;
		put_sample(buf + off + s->tone_slot * UCX_SAMPLE_BYTES, s->amp * gain * sin(s->phase));
		s->phase = fmod(s->phase + s->phase_inc, 2 * M_PI);
	}
}

static void LIBUSB_CALL stream_cb(struct libusb_transfer *t)
{
	struct stream *s = t->user_data;

	if (t->status == LIBUSB_TRANSFER_COMPLETED && (t->endpoint & LIBUSB_ENDPOINT_IN)) {
		clock_gettime(CLOCK_MONOTONIC, &s->last_in);
		if (!s->in_xfers)
			s->first_in = s->last_in;
		s->in_bytes += t->actual_length;
		s->in_xfers++;
		if (t->actual_length < t->length)
			s->short_in++;
		if (!s->dumped && t->actual_length) {
			int n = t->actual_length < 2 * (int)s->block_bytes ?
				t->actual_length : 2 * (int)s->block_bytes;

			printf("first IN transfer: %d bytes, first %d:\n", t->actual_length, n);
			hexdump(t->buffer, n);
			s->dumped = 1;
		}
		if (s->raw)
			fwrite(t->buffer, 1, t->actual_length, s->raw);
	} else if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		s->out_bytes += t->actual_length;
		s->out_xfers++;
	} else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
		if (++s->errors <= 5)
			fprintf(stderr, "ep 0x%02x: %s\n", t->endpoint, xfer_status_name(t->status));
	}

	/* Like 0x140010520: resubmit straight away, stop the slot on hard errors */
	if (s->running && !(t->endpoint & LIBUSB_ENDPOINT_IN))
		fill_out(s, t->buffer, t->length);
	if (s->running &&
	    (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) &&
	    libusb_submit_transfer(t) == 0)
		return;
	s->pending--;
}

static int read_meters(libusb_device_handle *h, int rounds);
static double meter_peak(libusb_device_handle *h, uint32_t bank, int ch);
static int send_dsp(libusb_device_handle *h, const uint32_t *words, int n);

/*
 * `mixprobe`: while the tone plays, send gain-0 mixer words one per second
 * and watch one output's level, to find which index removes the route.
 */
struct probe {
	int out;
	int n;
	const uint32_t *words;
	int batch;		/* >0: step 0 sends words[0..batch) in one transfer */
	int muted_by;		/* index into words, or -1 */
};

#define PROBE_BANK	0x33333333	/* outputs before volume */

static void probe_step(libusb_device_handle *h, struct probe *p, int step)
{
	static double before;
	double db = meter_peak(h, PROBE_BANK, p->out);

	if (step == 0) {
		before = db;
		printf("output %d before: %.1f dBFS\n", p->out, db);
	} else {
		printf("  after %08x: %.1f dBFS\n", p->words[step - 1], db);
		if (db < before - 20)
			p->muted_by = step - 1;
	}
	if (p->muted_by < 0 && step < p->n)
		send_dsp(h, &p->words[step], p->batch ? p->batch : 1);
}

/*
 * Run a duplex stream for @seconds. IN goes to @raw_path if given; OUT
 * carries a sine on @tone_slot (silence if negative).
 */
static int stream_run(libusb_context *ctx, libusb_device_handle *h, unsigned int rate,
		      double seconds, const char *raw_path, int tone_slot, double freq, double dbfs,
		      struct probe *probe)
{
	int probe_step_next = 0;
	struct libusb_transfer *xfers[2 * UCX_URBS_PER_EP] = { 0 };
	struct stream s = { .tone_slot = tone_slot, .dumped = tone_slot >= 0 };
	struct timeval tick = { .tv_usec = 100000 };
	struct timespec start, now;
	unsigned int alt, xfer_len, value;
	/* Let the queued OUT transfers play out the tone's fade */
	double run_seconds = tone_slot >= 0 ? seconds + 0.25 : seconds;
	int err, n = 0, meters_done = 0;

	if (ucx_stream_pairs(rate) != 1) {
		fprintf(stderr, "quad speed needs both endpoint pairs; not supported yet\n");
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}
	alt = ucx_speed_class(rate);
	s.block_bytes = ucx_bytes_per_block(rate);
	s.channels = ucx_channels(rate);
	if (tone_slot >= (int)s.channels) {
		fprintf(stderr, "output slot must be 0-%u at %u Hz\n", s.channels - 1, rate);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	s.amp = pow(10.0, dbfs / 20.0);
	s.phase_inc = 2 * M_PI * freq / rate;
	s.tone_total = seconds * rate;
	s.fade_frames = rate / 20;
	xfer_len = STREAM_BLOCKS_PER_XFER * s.block_bytes;
	value = ucx_reg1_large_buffer(rate, STREAM_BLOCKS_PER_XFER * ucx_frames_per_block(rate));

	if (raw_path) {
		s.raw = fopen(raw_path, "wb");
		if (!s.raw) {
			perror(raw_path);
			return LIBUSB_ERROR_IO;
		}
	}

	libusb_set_auto_detach_kernel_driver(h, 1);
	err = libusb_claim_interface(h, UCX_INTF_PCM);
	if (err)
		goto out_close;
	err = libusb_set_interface_alt_setting(h, UCX_INTF_PCM, alt);
	if (err)
		goto out_release;

	printf("rate %u: interface %d alt %u, %u-byte blocks, %u bytes per transfer, "
	       "REG1 bit 15 = %u\n", rate, UCX_INTF_PCM, alt, s.block_bytes, xfer_len, !!value);
	err = libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
				      LIBUSB_RECIPIENT_DEVICE, UCX_REQ_REG1,
				      value, UCX_REG1_LARGE_BUFFER, NULL, 0, TIMEOUT_MS);
	if (err < 0)
		goto out_alt;
	err = libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
				      LIBUSB_RECIPIENT_DEVICE, UCX_REQ_START_AT,
				      0, 0, NULL, 0, TIMEOUT_MS);
	if (err < 0)
		goto out_alt;

	/* OUT first, as the Windows driver starts 0x01 before 0x82 */
	s.running = 1;
	for (int i = 0; i < 2 * UCX_URBS_PER_EP; i++) {
		unsigned char ep = i < UCX_URBS_PER_EP ? UCX_EP_PCM_OUT : UCX_EP_PCM_IN;
		unsigned char *buf = calloc(1, xfer_len);

		xfers[i] = libusb_alloc_transfer(0);
		if (!buf || !xfers[i]) {
			free(buf);
			err = LIBUSB_ERROR_NO_MEM;
			break;
		}
		if (ep == UCX_EP_PCM_OUT)
			fill_out(&s, buf, xfer_len);
		libusb_fill_interrupt_transfer(xfers[i], h, ep, buf, xfer_len, stream_cb, &s, TIMEOUT_MS);
		xfers[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		err = libusb_submit_transfer(xfers[i]);
		if (err)
			break;
		s.pending++;
		n++;
	}
	if (err)
		fprintf(stderr, "submitting transfer %d: %s\n", n, libusb_strerror(err));

	/* Like the ASIO start (0x140015df0): enable channels once streams run */
	if (!err) {
		unsigned int mask_value, mask_index;

		ucx_channels_req((1u << s.channels) - 1, &mask_value, &mask_index);
		err = libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
					      LIBUSB_RECIPIENT_DEVICE, UCX_REQ_CHANNELS,
					      mask_value, mask_index, NULL, 0, TIMEOUT_MS);
		if (err < 0)
			fprintf(stderr, "CHANNELS request: %s\n", libusb_strerror(err));
		else
			err = 0;
	}

	signal(SIGINT, on_sigint);
	clock_gettime(CLOCK_MONOTONIC, &start);
	now = start;
	while (s.pending > 0 && !stop_requested && ts_diff(&start, &now) < run_seconds) {
		libusb_handle_events_timeout_completed(ctx, &tick, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (probe) {
			if (probe_step_next <= probe->n && probe->muted_by < 0 &&
			    ts_diff(&start, &now) > 2.0 + probe_step_next)
				probe_step(h, probe, probe_step_next++);
		} else if (tone_slot >= 0 && !meters_done && ts_diff(&start, &now) > 2.0) {
			printf("meters 2 s into the tone:\n");
			read_meters(h, 1);
			meters_done = 1;
		}
	}

	s.running = 0;
	for (int i = 0; i < n; i++)
		libusb_cancel_transfer(xfers[i]);
	while (s.pending > 0)
		libusb_handle_events_timeout_completed(ctx, &tick, NULL);
	for (int i = 0; i < 2 * UCX_URBS_PER_EP; i++)
		libusb_free_transfer(xfers[i]);
	signal(SIGINT, SIG_DFL);

	printf("IN:  %llu bytes in %lu transfers (%lu short)\n", s.in_bytes, s.in_xfers, s.short_in);
	printf("OUT: %llu bytes in %lu transfers, %lu error(s)\n", s.out_bytes, s.out_xfers, s.errors);
	if (s.in_xfers > 1) {
		/* Rate over complete transfers: the first one only marks the start */
		double dt = ts_diff(&s.first_in, &s.last_in);
		double bps = (s.in_bytes - s.in_bytes / s.in_xfers) / dt;

		printf("IN rate: %.0f bytes/s = %.0f blocks/s = %.0f frames/s (asked for %u)\n",
		       bps, bps / s.block_bytes, bps / s.block_bytes * ucx_frames_per_block(rate), rate);
	}

out_alt:
	libusb_set_interface_alt_setting(h, UCX_INTF_PCM, 0);
out_release:
	libusb_release_interface(h, UCX_INTF_PCM);
out_close:
	if (s.raw)
		fclose(s.raw);
	return err < 0 ? err : 0;
}

static int cmd_stream_in(libusb_context *ctx, libusb_device_handle *h, int argc, char **argv)
{
	unsigned int rate = argc > 0 ? strtoul(argv[0], NULL, 0) : 48000;
	double seconds = argc > 1 ? strtod(argv[1], NULL) : 2.0;

	return stream_run(ctx, h, rate, seconds, argc > 2 ? argv[2] : NULL, -1, 0, 0, NULL);
}

/* A wrong guess about the output format must not blow speakers or ears */
#define TONE_MAX_DBFS	-20.0

static int cmd_stream_out(libusb_context *ctx, libusb_device_handle *h, int argc, char **argv)
{
	int slot = argc > 0 ? atoi(argv[0]) : 0;
	double seconds = argc > 1 ? strtod(argv[1], NULL) : 3.0;
	double freq = argc > 2 ? strtod(argv[2], NULL) : 440.0;
	double dbfs = argc > 3 ? strtod(argv[3], NULL) : -30.0;
	unsigned int rate = argc > 4 ? strtoul(argv[4], NULL, 0) : 48000;

	if (slot < 0 || freq <= 0 || freq >= rate / 2.0 || seconds <= 0) {
		fprintf(stderr, "usage: stream-out [slot] [seconds] [freq] [dBFS] [rate]\n");
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	if (dbfs > TONE_MAX_DBFS) {
		fprintf(stderr, "level capped at %.0f dBFS\n", TONE_MAX_DBFS);
		dbfs = TONE_MAX_DBFS;
	}
	printf("tone: %.1f Hz at %.1f dBFS on output slot %d for %.1f s\n", freq, dbfs, slot, seconds);
	return stream_run(ctx, h, rate, seconds, NULL, slot, freq, dbfs, NULL);
}

/*
 * Find the mixer index of playback 1 -> @out. Candidates: TotalMix layout
 * (playback at 0x20), FireWire serialization order (playback after 18 inputs
 * and 2 FX returns, at 20), and both with output and source swapped.
 */
static int cmd_mixprobe(libusb_context *ctx, libusb_device_handle *h, int argc, char **argv)
{
	int out = argc > 0 ? atoi(argv[0]) : 8;
	uint32_t idx[] = {
		UCX_DSP_MIX_STEP * out + UCX_DSP_PLAYBACK,
		UCX_DSP_MIX_STEP * out + 20,
		UCX_DSP_PLAYBACK + out,
		20 + out,
	};
	uint32_t words[sizeof(idx) / sizeof(idx[0])];
	struct probe p = { .out = out, .n = sizeof(idx) / sizeof(idx[0]), .words = words,
			   .muted_by = -1 };
	int err;

	if (out < 0 || out >= UCX_DSP_OUTPUTS)
		return LIBUSB_ERROR_INVALID_PARAM;
	for (int i = 0; i < p.n; i++)
		words[i] = ucx_dsp_word(UCX_DSP_MIXER | idx[i] << 16);

	err = stream_run(ctx, h, 48000, 3.0 + p.n, NULL, 0, 440.0, -30.0, &p);
	if (p.muted_by < 0) {
		printf("no candidate removed the route\n");
		return err;
	}
	printf("index 0x%03x routes playback 1 to output %d; restoring it to 0 dB\n",
	       idx[p.muted_by], out);
	words[0] = ucx_dsp_word(UCX_DSP_MIXER | idx[p.muted_by] << 16 | ucx_dsp_mix_coef(UCX_DSP_MIX_UNITY));
	return send_dsp(h, words, 1);
}

/*
 * While the tone plays, send gain 0 for playback 1..@count -> @out in ONE
 * transfer and check whether the playback 1 route went away. With "rev" the
 * playback 1 word is sent last instead of first.
 */
static int cmd_mixbatch(libusb_context *ctx, libusb_device_handle *h, int argc, char **argv)
{
	int out = argc > 0 ? atoi(argv[0]) : 8;
	int count = argc > 1 ? atoi(argv[1]) : 18;
	int rev = argc > 2 && !strcmp(argv[2], "rev");
	uint32_t words[UCX_MIX_SOURCES_MAX];
	struct probe p = { .out = out, .n = 1, .words = words, .batch = count, .muted_by = -1 };
	uint32_t restore;
	int err;

	if (out < 0 || out >= UCX_DSP_OUTPUTS || count < 1 || count > UCX_MIX_SOURCES_MAX)
		return LIBUSB_ERROR_INVALID_PARAM;
	for (int i = 0; i < count; i++) {
		int ch = rev ? count - 1 - i : i;

		words[i] = ucx_dsp_word(UCX_DSP_MIXER |
					(UCX_DSP_MIX_STEP * out + UCX_DSP_PLAYBACK + ch) << 16);
	}

	err = stream_run(ctx, h, 48000, 4.0, NULL, 0, 440.0, -30.0, &p);
	printf("%d-word transfer%s while streaming: playback 1 route %s\n", count,
	       rev ? " (playback 1 last)" : "", p.muted_by >= 0 ? "REMOVED" : "still there");
	if (p.muted_by < 0)
		return err;
	restore = ucx_dsp_word(UCX_DSP_MIXER | (UCX_DSP_MIX_STEP * out + UCX_DSP_PLAYBACK) << 16 |
			       ucx_dsp_mix_coef(UCX_DSP_MIX_UNITY));
	printf("restoring playback 1 -> output %d to 0 dB\n", out);
	return send_dsp(h, &restore, 1);
}

/* Send DSP command words the way TotalMix does: one bulk OUT on 0x0c */
static int send_dsp(libusb_device_handle *h, const uint32_t *words, int n)
{
	unsigned char buf[64 * 4];
	int sent, err;

	for (int i = 0; i < n; i++) {
		printf("dsp word %08x\n", words[i]);
		for (int b = 0; b < 4; b++)
			buf[i * 4 + b] = words[i] >> (8 * b);
	}
	libusb_set_auto_detach_kernel_driver(h, 1);
	err = libusb_claim_interface(h, UCX_INTF_AUX);
	if (err)
		return err;
	err = libusb_bulk_transfer(h, UCX_EP_AUX_OUT, buf, n * 4, &sent, TIMEOUT_MS);
	libusb_release_interface(h, UCX_INTF_AUX);
	return err;
}

/* Gains are capped at 0 dB: these are for testing, not for mixing */
static int parse_db(const char *s, double *db)
{
	if (!strcmp(s, "off")) {
		*db = -INFINITY;
		return 0;
	}
	*db = strtod(s, NULL);
	if (*db > 0.0) {
		fprintf(stderr, "gain capped at 0 dB\n");
		*db = 0.0;
	}
	return 0;
}

static int cmd_mix(libusb_device_handle *h, int argc, char **argv)
{
	unsigned int channels = ucx_channels(48000);
	int out, play;
	double db;
	uint32_t w;

	if (argc < 3) {
		fprintf(stderr, "usage: mix <output 0-%u> <playback 0-%u> <dB|off>\n",
			channels - 1, channels - 1);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	out = atoi(argv[0]);
	play = atoi(argv[1]);
	if (out < 0 || out >= (int)channels || play < 0 || play >= (int)channels)
		return LIBUSB_ERROR_INVALID_PARAM;
	parse_db(argv[2], &db);

	w = ucx_dsp_mix_cmd(out, UCX_DSP_PLAYBACK + play, (int)lrint(pow(10.0, db / 20.0) * 32768.0));
	return send_dsp(h, &w, 1);
}

static int cmd_outvol(libusb_device_handle *h, int argc, char **argv)
{
	unsigned int channels = ucx_channels(48000);
	int out;
	double db;
	uint32_t w;

	if (argc < 2) {
		fprintf(stderr, "usage: outvol <output 0-%u> <dB>  (-65 is the minimum)\n", channels - 1);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	out = atoi(argv[0]);
	if (out < 0 || out >= (int)channels)
		return LIBUSB_ERROR_INVALID_PARAM;
	parse_db(argv[1], &db);
	if (db < -65.0)
		db = -65.0;

	w = ucx_dsp_output_vol_cmd(out, (int)lrint(db * 10.0));
	return send_dsp(h, &w, 1);
}

static int cmd_dsp(libusb_device_handle *h, int argc, char **argv)
{
	uint32_t words[64];
	int n = argc < 64 ? argc : 64;

	if (!n) {
		fprintf(stderr, "usage: dsp <word> [word...]  (parity bit is added)\n");
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	for (int i = 0; i < n; i++) {
		words[i] = ucx_dsp_word(strtoul(argv[i], NULL, 16));
		/* Checked before anything is sent: none of the words goes out */
		if (UCX_DSP_WORD_REG(words[i]) == UCX_DSP_REG_STORE ||
		    UCX_DSP_WORD_REG(words[i]) == UCX_DSP_REG_STORE_NEAR) {
			fprintf(stderr, "refusing DSP word %s: register %04x stores the unit's "
				"state to flash\n", argv[i], UCX_DSP_WORD_REG(words[i]));
			return LIBUSB_ERROR_ACCESS;
		}
	}
	return send_dsp(h, words, n);
}

static uint32_t get_le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Print peak levels of each meter bank, @rounds times, the way TotalMix polls them */
static int read_meters(libusb_device_handle *h, int rounds)
{
	unsigned char buf[512];
	int got, err;

	libusb_set_auto_detach_kernel_driver(h, 1);
	err = libusb_claim_interface(h, UCX_INTF_AUX);
	if (err)
		return err;

	for (int r = 0; r < rounds * 5 && !err; r++) {
		err = libusb_bulk_transfer(h, UCX_EP_METERS, buf, sizeof(buf), &got, TIMEOUT_MS);
		if (err)
			break;
		if (got < UCX_METER_MIN_BYTES) {
			printf("short meter read: %d bytes\n", got);
			hexdump(buf, got);
			continue;
		}
		printf("bank %08x peak dBFS:", get_le32(buf + UCX_METER_TAG_OFFSET));
		for (int c = 0; c < UCX_METER_CHANNELS; c++) {
			uint32_t peak = get_le32(buf + UCX_METER_PEAK_OFFSET + 4 * c) & ~0xfu;

			if (peak)
				printf(" %d:%.0f", c, 20 * log10(peak / 134217728.0));
		}
		putchar('\n');
	}
	libusb_release_interface(h, UCX_INTF_AUX);
	return err;
}

/* Peak of channel @ch in meter bank @bank, in dBFS (-INFINITY when silent or unread) */
static double meter_peak(libusb_device_handle *h, uint32_t bank, int ch)
{
	unsigned char buf[512];
	double db = -INFINITY;
	int got;

	libusb_set_auto_detach_kernel_driver(h, 1);
	if (libusb_claim_interface(h, UCX_INTF_AUX))
		return db;
	/* The device cycles through 5 banks; 6 reads always cover the one we want */
	for (int r = 0; r < 6; r++) {
		if (libusb_bulk_transfer(h, UCX_EP_METERS, buf, sizeof(buf), &got, TIMEOUT_MS) ||
		    got < UCX_METER_MIN_BYTES)
			break;
		if (get_le32(buf + UCX_METER_TAG_OFFSET) == bank) {
			uint32_t peak = get_le32(buf + UCX_METER_PEAK_OFFSET + 4 * ch) & ~0xfu;

			if (peak)
				db = 20 * log10(peak / 134217728.0);
			break;
		}
	}
	libusb_release_interface(h, UCX_INTF_AUX);
	return db;
}

static int cmd_meters(libusb_device_handle *h, int argc, char **argv)
{
	return read_meters(h, argc > 0 ? atoi(argv[0]) : 1);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ucxctl <command> [args]\n"
		"  info                                  list interfaces and endpoints\n"
		"  vendor-out <req> <wValue> <wIndex>    vendor request, no data\n"
		"  vendor-in <req> <wValue> <wIndex> <len>\n"
		"  ctrl-read                             read one 256-byte flash data block from ep 0x88\n"
		"  stream-in [rate] [seconds] [raw-file] stream (silence out), measure and save input\n"
		"  stream-out [slot] [seconds] [freq] [dBFS] [rate]\n"
		"                                        quiet sine on one output slot (default -30 dBFS)\n"
		"  mix <output> <playback> <dB|off>      route a playback channel to an output (max 0 dB)\n"
		"  outvol <output> <dB>                  output volume (max 0 dB)\n"
		"  dsp <hex word>...                     raw DSP command words\n"
		"  meters [rounds]                       read level meters (5 banks per round)\n"
		"  mixprobe [output]                     find the mixer index of playback 1 -> output\n"
		"  mixbatch [output] [count] [rev]       mute playback 1..count -> output in one transfer\n");
}

int main(int argc, char **argv)
{
	libusb_context *ctx;
	libusb_device_handle *h;
	int err;

	if (argc < 2) {
		usage();
		return 2;
	}
	err = libusb_init_context(&ctx, NULL, 0);
	if (err) {
		fprintf(stderr, "libusb: %s\n", libusb_strerror(err));
		return 1;
	}
	h = open_ucx(ctx);
	if (!h) {
		libusb_exit(ctx);
		return 1;
	}

	if (!strcmp(argv[1], "info")) {
		err = cmd_info(h);
	} else if (!strcmp(argv[1], "vendor-out")) {
		err = cmd_vendor(h, 0, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "vendor-in")) {
		err = cmd_vendor(h, 1, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "ctrl-read")) {
		err = cmd_ctrl_read(h);
	} else if (!strcmp(argv[1], "stream-in")) {
		err = cmd_stream_in(ctx, h, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "stream-out")) {
		err = cmd_stream_out(ctx, h, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "mix")) {
		err = cmd_mix(h, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "outvol")) {
		err = cmd_outvol(h, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "dsp")) {
		err = cmd_dsp(h, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "mixbatch")) {
		err = cmd_mixbatch(ctx, h, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "mixprobe")) {
		err = cmd_mixprobe(ctx, h, argc - 2, argv + 2);
	} else if (!strcmp(argv[1], "meters")) {
		err = cmd_meters(h, argc - 2, argv + 2);
	} else {
		usage();
		err = LIBUSB_ERROR_INVALID_PARAM;
	}

	if (err)
		fprintf(stderr, "%s: %s\n", argv[1], libusb_strerror(err));
	libusb_close(h);
	libusb_exit(ctx);
	return err ? 1 : 0;
}
