// SPDX-License-Identifier: MIT
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ucx_proto.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
typedef int atomic_t;
typedef int wait_queue_head_t;

#define BIT(n) (1u << (n))
#define HZ 1000ul
#define GFP_ATOMIC 0
#define USB_DIR_IN 0x80
#define SNDRV_PCM_STREAM_PLAYBACK 0
#define SNDRV_PCM_TRIGGER_START 1
#define SNDRV_PCM_TRIGGER_STOP 2
#define SNDRV_PCM_TRIGGER_SUSPEND 3
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define time_after(a, b) ((a) > (b))
#define time_before(a, b) ((a) < (b))
#define msecs_to_jiffies(x) ((unsigned long)(x))
#define le32_to_cpu(x) (x)
#define lockdep_assert_held(lock) assert(*(lock))
#define mutex_lock(lock) (++*(lock))
#define mutex_unlock(lock) (--*(lock))
#define spin_lock_irq(lock) mutex_lock(lock)
#define spin_unlock_irq(lock) mutex_unlock(lock)
#define spin_lock_irqsave(lock, flags) ((flags) = 0, mutex_lock(lock))
#define spin_unlock_irqrestore(lock, flags) ((void)(flags), mutex_unlock(lock))
#define atomic_inc(value) (++*(value))
#define atomic_dec_and_test(value) (--*(value) == 0)
#define atomic_read_acquire(value) (*(value))
#define wake_up(queue) (++*(queue))
#define dev_warn_ratelimited(device, ...) ((void)sizeof(device), discard_log(__VA_ARGS__))
#define dev_err(device, ...) ((void)sizeof(device), discard_log(__VA_ARGS__))
#define wait_event(queue, condition) do { \
	if (!(condition)) { \
		finish_notification(); \
		assert(condition); \
		assert(queue); \
	} \
} while (0)

struct urb {
	struct ucx *context;
	unsigned int pipe, actual_length, transfer_buffer_length;
	int status;
	u8 *transfer_buffer;
};
struct work_struct { unsigned int scheduled; };
struct snd_pcm_runtime {
	u8 *dma_area;
	unsigned int rate, buffer_size, period_size, delay;
};
struct snd_pcm_substream {
	struct ucx *chip;
	struct snd_pcm_runtime *runtime;
	int stream;
	bool active;
};
struct snd_pcm { struct { struct snd_pcm_substream *substream; } streams[2]; };

#include "pcm-structs.h"

struct ucx {
	int lock, stream_lock;
	struct { int dev; } *intf;
	void *udev;
	bool disconnected;
	enum ucx_pcm_state pcm_state;
	unsigned int rate, pairs, pair_bytes, prepared, urb_frames, urb_bytes, blocks;
	struct ucx_stream play, capture;
	struct work_struct stream_err_work;
	unsigned long started, err_window;
	unsigned int err_restarts, unit_errors, stream_restarts;
	struct snd_pcm *pcm;
};

static unsigned long jiffies = 30000;
static unsigned int xruns, notifications, resubmits, kills;
static bool fail_start;
static void finish_notification(void);
static int ucx_engine_start(struct ucx *ucx, unsigned int rate, unsigned int blocks);
static int ucx_pcm_trigger(struct snd_pcm_substream *substream, int cmd);

static void schedule_work(struct work_struct *work) { work->scheduled++; }
static void discard_log(const char *format, ...) { (void)format; }
static unsigned int usb_pipeendpoint(unsigned int pipe) { return pipe & 0xf; }
static bool usb_pipein(unsigned int pipe) { return pipe & 0x80; }
static int usb_submit_urb(struct urb *urb, int flags) { (void)urb; (void)flags; return ++resubmits, 0; }
static void snd_pcm_period_elapsed(struct snd_pcm_substream *s) { assert(s->runtime); notifications++; }
static struct ucx *snd_pcm_substream_chip(struct snd_pcm_substream *s) { return s->chip; }

static void ucx_spdif_status(struct ucx *ucx, unsigned int pair, const u8 *buf,
			     unsigned int frames)
{
	assert(pair < ucx->pairs && buf && frames == ucx->urb_frames);
}

static void ucx_kill_urbs(struct ucx *ucx)
{
	assert(ucx->pcm_state != UCX_PCM_RUNNING);
	assert(!ucx->play.period_inflight && !ucx->capture.period_inflight);
	kills++;
}

static void usb_set_interface(void *device, unsigned int interface, unsigned int alt)
{
	(void)device;
	assert(interface == UCX_INTF_PCM && alt == 0);
}

static void snd_pcm_stop_xrun(struct snd_pcm_substream *s)
{
	if (s->active) {
		assert(kills);
		assert(!ucx_pcm_trigger(s, SNDRV_PCM_TRIGGER_STOP));
		s->active = false;
		xruns++;
	}
}

#include "pcm-callbacks.h"

static struct snd_pcm_substream *pending_notification;

static void finish_notification(void)
{
	struct ucx *ucx = pending_notification->chip;

	assert(!ucx->play.substream && ucx->capture.substream);
	/* The other direction can make progress while sync_stop waits. */
	ucx->capture.substream->runtime->period_size = 2 * ucx->urb_frames;
	u8 capture[UCX_URB_BLOCKS * 0x380] = {0};
	assert(!ucx_copy(ucx, &ucx->capture, 0, capture, ucx->urb_frames, false));
	assert(ucx->capture.hwptr == ucx->urb_frames);
	ucx_period_elapsed(&ucx->play, pending_notification);
	pending_notification = NULL;
}

static int ucx_engine_start(struct ucx *ucx, unsigned int rate, unsigned int blocks)
{
	u8 playback[UCX_URB_BLOCKS * 0x380];
	struct urb urb = {ucx, UCX_EP_PCM_OUT, ucx->urb_bytes, ucx->urb_bytes, 0, playback};

	assert(ucx->pcm_state == UCX_PCM_STOPPED);
	assert(!ucx->play.substream && !ucx->capture.substream);
	assert(xruns == 2 && kills == 1);
	assert(rate == ucx->rate && blocks == ucx->blocks);
	if (fail_start)
		return -EIO;
	ucx->pcm_state = UCX_PCM_STARTING;
	memset(playback, 0xab, ucx->urb_bytes);
	ucx_playback_complete(&urb);
	for (unsigned int i = 0; i < ucx->urb_bytes; i++)
		assert(playback[i] == 0);
	assert(ucx->play.hwptr == 0);
	memset(ucx->play.seq, 0, sizeof(ucx->play.seq));
	memset(ucx->capture.seq, 0, sizeof(ucx->capture.seq));
	ucx->pcm_state = UCX_PCM_RUNNING;
	ucx->started = jiffies;
	return 0;
}

struct fixture {
	struct ucx ucx;
	struct snd_pcm pcm;
	struct snd_pcm_runtime runtime[2];
	struct snd_pcm_substream sub[2];
	u8 ring[2][128 * 72];
};

static void begin(struct fixture *f, unsigned int rate)
{
	struct ucx *ucx = &f->ucx;

	memset(f, 0, sizeof(*f));
	xruns = notifications = resubmits = kills = 0;
	ucx->pcm = &f->pcm;
	ucx->rate = rate;
	ucx->blocks = 2;
	ucx->pairs = ucx_stream_pairs(rate);
	ucx->pair_bytes = ucx_pair_bytes(rate);
	ucx->urb_frames = ucx->blocks * ucx_frames_per_block(rate);
	ucx->urb_bytes = ucx->blocks * ucx_bytes_per_block(rate);
	ucx->pcm_state = UCX_PCM_RUNNING;
	ucx->prepared = BIT(0) | BIT(1);
	for (int i = 0; i < 2; i++) {
		f->runtime[i] = (struct snd_pcm_runtime){f->ring[i], rate, 128, ucx->urb_frames, 0};
		f->sub[i] = (struct snd_pcm_substream){ucx, &f->runtime[i], i, true};
		f->pcm.streams[i].substream = &f->sub[i];
		assert(!ucx_pcm_trigger(&f->sub[i], SNDRV_PCM_TRIGGER_START));
	}
	ucx_tick(ucx);
}

static void test_recovery(unsigned int rate, unsigned int fault)
{
	struct fixture f;
	struct ucx *ucx = &f.ucx;
	u8 packet[UCX_URB_BLOCKS * 0x380];
	const __le32 event[] = {UCX_EVENT_VALUE | UCX_EVENT_RESTART | UCX_EVENT_ERROR,
				 UCX_EVENT_ERROR, UCX_EVENT_RESTART};

	begin(&f, rate);
	struct urb in = {ucx, UCX_EP_PCM_IN, ucx->urb_bytes, ucx->urb_bytes, 0, packet};
	ucx->started = jiffies;
	ucx_pcm_events(ucx, event, 3);
	assert(!ucx->stream_err_work.scheduled && !ucx->unit_errors);
	jiffies += 100;
	if (fault == 1) {
		in.status = -EPROTO;
		ucx_capture_complete(&in);
	} else if (fault == 2) {
		in.actual_length -= ucx_bytes_per_block(rate);
		ucx_capture_complete(&in);
	} else {
		ucx_pcm_events(ucx, event, 3);
		assert(ucx->unit_errors == 1);
	}
	assert(!ucx->capture.seq[0] && !ucx->capture.hwptr && !resubmits);
	assert(ucx->pcm_state == UCX_PCM_FAILED && ucx->stream_err_work.scheduled == 1);
	ucx_capture_complete(&in);
	ucx_pcm_events(ucx, event, 3);
	assert(ucx->stream_err_work.scheduled == 1);
	assert(ucx_pcm_trigger(&f.sub[0], SNDRV_PCM_TRIGGER_START) == -EPIPE);
	ucx_stream_err_work(&ucx->stream_err_work);
	assert(xruns == 2 && ucx->stream_restarts == 1);
	if (fail_start) {
		assert(ucx->pcm_state == UCX_PCM_STOPPED);
		assert(ucx_pcm_trigger(&f.sub[0], SNDRV_PCM_TRIGGER_START) == -EPIPE);
		return;
	}
	assert(ucx->pcm_state == UCX_PCM_RUNNING);
	assert(!ucx_pcm_trigger(&f.sub[1], SNDRV_PCM_TRIGGER_START));
	in.actual_length = ucx->urb_bytes;
	in.status = 0;
	for (unsigned int pair = 0; pair < ucx->pairs; pair++) {
		for (unsigned int frame = 0; frame < ucx->urb_frames; frame++)
			memset(packet + frame * ucx->pair_bytes, frame + pair, ucx->pair_bytes);
		in.pipe = pair ? UCX_EP_PCM_IN2 : UCX_EP_PCM_IN;
		ucx_capture_complete(&in);
	}
	assert(ucx->capture.hwptr == ucx->urb_frames && notifications == 1);
	for (unsigned int frame = 0; frame < ucx->urb_frames; frame++)
		for (unsigned int pair = 0; pair < ucx->pairs; pair++)
			assert(f.ring[1][(frame * ucx->pairs + pair) * ucx->pair_bytes] == frame + pair);
}

static void test_sync_stop(void)
{
	struct fixture f;
	struct ucx *ucx = &f.ucx;
	u8 packet[UCX_URB_BLOCKS * 0x380];

	begin(&f, 48000);
	pending_notification = ucx_copy(ucx, &ucx->play, 0, packet, ucx->urb_frames, true);
	assert(pending_notification == &f.sub[0] && ucx->play.period_inflight == 1);
	assert(!ucx_pcm_trigger(&f.sub[0], SNDRV_PCM_TRIGGER_STOP));
	assert(!ucx_copy(ucx, &ucx->play, 0, packet, ucx->urb_frames, true));
	assert(!ucx_pcm_sync_stop(&f.sub[0]));
	assert(!pending_notification && !ucx->play.period_inflight && notifications == 1);
	assert(ucx->pcm_state == UCX_PCM_RUNNING && !kills);
}

int main(void)
{
	for (unsigned int fault = 1; fault <= 3; fault++) {
		test_recovery(48000, fault);
		test_recovery(192000, fault);
	}
	fail_start = true;
	test_recovery(48000, 1);
	test_sync_stop();
	puts("PCM tests passed (duplex faults, frame alignment, failed restart, per-direction stop)");
}
