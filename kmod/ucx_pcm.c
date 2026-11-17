// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * PCM for the Fireface UCX: a duplex engine of interrupt URBs on interface 5.
 *
 * The device paces both endpoints (it NAKs until it has data or room), so
 * every completed URB is one fixed slice of time. Completions copy between
 * the URB and the ALSA ring buffer and resubmit at once, like the Windows
 * driver's 0x140010520. Both directions always run together; trigger only
 * attaches a substream to its direction.
 *
 * The rate decides the stream layout (ucx_proto.h): 18 channels up to 48 kHz,
 * 14 up to 96 kHz, and 12 up to 192 kHz, where the channels are split over
 * two endpoint pairs.
 */
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <sound/control.h>
#include <sound/pcm_params.h>

#include "ucx.h"

/*
 * Playback URBs wait in the host controller's queue until the device takes
 * them, so the queue is output latency: UCX_PLAY_URBS * urb_blocks blocks,
 * UCX_PLAY_URBS * urb_blocks / 6 ms at 48 kHz (a block is 1/6000 s in the
 * 48 kHz family, longer at 44.1 and 32 kHz; see ucx.h).
 * A shorter queue means less latency but also less time for a completion to
 * refill it before the device runs dry. Capture always queues
 * UCX_URBS_PER_EP: waiting IN URBs cost no latency.
 *
 * Round trip at 48 kHz: 106 frames with 2 blocks, 138 with 4. Shorter queues
 * glitched under load or were never soaked. Read when a
 * stream starts.
 */
static unsigned int urb_blocks = 2;
module_param(urb_blocks, uint, 0644);
MODULE_PARM_DESC(urb_blocks,
		 "Blocks of 8/16/32 frames (1/6 ms at 48 kHz) per URB: 2 (default) or 4, for 0.7 ms more latency at 48 kHz if playback glitches under load");

static const struct snd_pcm_hardware ucx_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_64000 | SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		 SNDRV_PCM_RATE_128000 | SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
	.rate_min = 32000,
	.rate_max = 192000,
	.channels_min = UCX_MIN_CHANNELS,
	.channels_max = UCX_MAX_CHANNELS,
	.buffer_bytes_max = 4 * 1024 * 1024,
	/* Two blocks at single speed; ucx_rule_period() sets the real minimum */
	.period_bytes_min = 2 * 0x240,
	.period_bytes_max = 2 * 1024 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

static unsigned int ucx_urb_blocks(void)
{
	return ucx_urb_blocks_used(READ_ONCE(urb_blocks));
}

static struct ucx_stream *ucx_stream(struct ucx *ucx, int dir)
{
	return dir == SNDRV_PCM_STREAM_PLAYBACK ? &ucx->play : &ucx->capture;
}

/*
 * Attach the streams that trigger armed. This runs in the completion of
 * capture endpoint 0x82, the device's clock, before that URB is counted: it
 * is the one point where the URB numbers of both directions have a fixed
 * relation (all completions of one host controller are handled in order by
 * one context). Choosing the start in trigger itself, which can run between
 * a playback and a capture completion, made the playback-to-capture offset
 * vary by one URB. Both directions and both pairs start at the same URB
 * number, which keeps the round trip the same from start to start.
 */
static void ucx_tick(struct ucx *ucx)
{
	struct ucx_stream *play = &ucx->play, *cap = &ucx->capture;
	unsigned int seq;

	lockdep_assert_held(&ucx->stream_lock);
	if (ucx->pcm_state == UCX_PCM_RUNNING && (play->armed || cap->armed)) {
		/*
		 * Normally every endpoint has completed the same number of
		 * URBs here. Under load one of them can be a URB behind, so
		 * take the largest number: all of them still have that ahead.
		 */
		seq = cap->seq[0];
		if ((int)(play->seq[0] - seq) > 0)
			seq = play->seq[0];
		if (ucx->pairs == 2) {
			if ((int)(cap->seq[1] - seq) > 0)
				seq = cap->seq[1];
			if ((int)(play->seq[1] - seq) > 0)
				seq = play->seq[1];
		}
		if (cap->armed) {
			cap->start_seq = seq;
			cap->armed = false;
		}
		if (play->armed) {
			play->start_seq = seq;
			play->armed = false;
		}
	}
}

/*
 * Copy up to the end of the ring. Quad speed interleaves two six-channel
 * endpoints in ALSA. A fixed copy size lets the compiler inline each slice.
 */
static void ucx_copy_span(u8 *ring, u8 *buf, unsigned int frames, unsigned int frame_bytes,
			  bool split, bool playback)
{
	const unsigned int slice = 6 * UCX_SAMPLE_BYTES;
	size_t i;

	if (!split) {
		if (playback)
			memcpy(buf, ring, frames * frame_bytes);
		else
			memcpy(ring, buf, frames * frame_bytes);
	} else if (playback) {
		for (i = 0; i < frames; i++)
			memcpy(buf + i * slice, ring + i * (2 * slice), slice);
	} else {
		for (i = 0; i < frames; i++)
			memcpy(ring + i * (2 * slice), buf + i * slice, slice);
	}
}

/*
 * Move @frames of one endpoint pair between @buf and the ALSA ring buffer of
 * @s. Returns the substream when a period boundary was crossed, for
 * snd_pcm_period_elapsed().
 */
static struct snd_pcm_substream *ucx_copy(struct ucx *ucx, struct ucx_stream *s,
					  unsigned int pair, u8 *buf, unsigned int frames,
					  bool playback)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned int pair_bytes = ucx->pair_bytes, frame_bytes = ucx->pairs * pair_bytes;
	unsigned int first, pos, done, seq;
	unsigned long flags;
	bool elapsed = false;
	u8 *ring;

	spin_lock_irqsave(&ucx->stream_lock, flags);
	if (!playback && !pair)
		ucx_tick(ucx);
	substream = ucx->pcm_state == UCX_PCM_RUNNING ? s->substream : NULL;
	seq = s->seq[pair]++;
	/* Both pairs attach at the same URB number, see struct ucx_stream */
	if (substream && !s->armed && !s->attached[pair] && (int)(seq - s->start_seq) >= 0)
		s->attached[pair] = true;
	if (!substream || !s->attached[pair]) {
		if (playback)
			memset(buf, 0, frames * pair_bytes);
		spin_unlock_irqrestore(&ucx->stream_lock, flags);
		return NULL;
	}

	runtime = substream->runtime;
	pos = s->pos[pair];
	ring = runtime->dma_area + pair * pair_bytes;
	first = min_t(unsigned int, frames, runtime->buffer_size - pos);
	ucx_copy_span(ring + pos * frame_bytes, buf, first, frame_bytes, ucx->pairs == 2, playback);
	if (first < frames)
		ucx_copy_span(ring, buf + first * pair_bytes, frames - first, frame_bytes,
			      ucx->pairs == 2, playback);
	pos += frames;
	if (pos >= runtime->buffer_size)
		pos -= runtime->buffer_size;
	s->pos[pair] = pos;
	s->done[pair] += frames;

	/* A frame is done when the pair that is behind has done it */
	done = s->done[0];
	if (ucx->pairs == 2 && (int)(s->done[1] - done) < 0)
		done = s->done[1];
	frames = done - s->reported;
	s->reported = done;
	s->hwptr += frames;
	if (s->hwptr >= runtime->buffer_size)
		s->hwptr -= runtime->buffer_size;
	s->period_pos += frames;
	if (s->period_pos >= runtime->period_size) {
		s->period_pos -= runtime->period_size;
		elapsed = true;
		atomic_inc(&s->period_inflight);
	}
	/* What was just copied is played after the URBs queued ahead of it */
	if (playback)
		runtime->delay = UCX_PLAY_URBS * ucx->urb_frames;
	spin_unlock_irqrestore(&ucx->stream_lock, flags);

	return elapsed ? substream : NULL;
}

/* Pair 1 is endpoints 0x03 and 0x84 */
static unsigned int ucx_urb_pair(struct urb *urb)
{
	return usb_pipeendpoint(urb->pipe) > 2;
}

/*
 * A stream URB failed. Its slice of time is lost, and the directions or pairs
 * no longer count the same URBs, which would shift the round trip for good.
 * So the URB is not resubmitted, and stream_err_work restarts the engine.
 */
static bool ucx_stream_fail_locked(struct ucx *ucx)
{
	lockdep_assert_held(&ucx->stream_lock);
	switch (ucx->pcm_state) {
	case UCX_PCM_RUNNING:
		WRITE_ONCE(ucx->pcm_state, UCX_PCM_FAILED);
		schedule_work(&ucx->stream_err_work);
		return true;
	case UCX_PCM_STARTING:
		/* The caller of ucx_engine_start() will unwind this start. */
		WRITE_ONCE(ucx->pcm_state, UCX_PCM_FAILED);
		return true;
	default:
		return false;
	}
}

static void ucx_stream_error(struct ucx *ucx, struct urb *urb, int err, const char *what)
{
	unsigned long flags;
	bool first;

	spin_lock_irqsave(&ucx->stream_lock, flags);
	first = ucx_stream_fail_locked(ucx);
	spin_unlock_irqrestore(&ucx->stream_lock, flags);
	if (first)
		dev_warn_ratelimited(&ucx->intf->dev,
				     "%s PCM URB on 0x%02x: %d\n",
				     what, usb_pipeendpoint(urb->pipe) |
				     (usb_pipein(urb->pipe) ? USB_DIR_IN : 0), err);
}

/*
 * The unit sends error and restart words in both the start and stop bursts.
 * Check the window and change state under the same lock as start and stop.
 */
#define UCX_EVENT_START_MS	100

void ucx_pcm_events(struct ucx *ucx, const __le32 *buf, unsigned int words)
{
	unsigned long flags;
	unsigned int i;
	u32 restart = 0;

	spin_lock_irqsave(&ucx->stream_lock, flags);
	if (ucx->pcm_state != UCX_PCM_RUNNING ||
	    time_before(jiffies, ucx->started + msecs_to_jiffies(UCX_EVENT_START_MS)))
		goto unlock;
	for (i = 0; i < words; i++) {
		u32 word = le32_to_cpu(buf[i]);

		if (word & UCX_EVENT_VALUE)
			continue;
		if (word & UCX_EVENT_RESTART) {
			ucx_stream_fail_locked(ucx);
			restart = word;
			break;
		}
		if (word & UCX_EVENT_ERROR)
			atomic_inc(&ucx->unit_errors);
	}
unlock:
	spin_unlock_irqrestore(&ucx->stream_lock, flags);
	if (restart)
		dev_warn_ratelimited(&ucx->intf->dev,
				     "unit asked for a stream restart (event %08x)\n", restart);
}

/* True when the URB must not be resubmitted: killed, device gone, or failed */
static bool ucx_urb_stopped(struct ucx *ucx, struct urb *urb)
{
	switch (urb->status) {
	case 0:
		if (urb->actual_length == urb->transfer_buffer_length)
			return false;
		/* The sequence number describes a whole URB of audio. */
		ucx_stream_error(ucx, urb, -EREMOTEIO, "unexpected length for");
		return true;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return true;
	default:
		ucx_stream_error(ucx, urb, urb->status, "failed");
		return true;
	}
}

static void ucx_resubmit(struct ucx *ucx, struct urb *urb)
{
	int err = usb_submit_urb(urb, GFP_ATOMIC);

	/* -EPERM: being killed by ucx_engine_stop(); the others: device gone or asleep */
	if (err && err != -EPERM && err != -ENODEV && err != -ESHUTDOWN &&
	    err != -EHOSTUNREACH)
		ucx_stream_error(ucx, urb, err, "resubmitting");
}

static void ucx_period_elapsed(struct ucx_stream *s, struct snd_pcm_substream *substream)
{
	if (!substream)
		return;
	snd_pcm_period_elapsed(substream);
	if (atomic_dec_and_test(&s->period_inflight))
		wake_up(&s->period_wait);
}

#ifdef UCX_PCM_TEST
/* One-shot faults for tests/pcm-device.c, on the primary capture endpoint. */
static unsigned int pcm_fault;
module_param(pcm_fault, uint, 0600);
MODULE_PARM_DESC(pcm_fault, "Inject one PCM fault: 1=URB error, 2=short capture, 3=restart event");

static void ucx_pcm_inject_fault(struct ucx *ucx, struct urb *urb)
{
	const __le32 restart = cpu_to_le32(UCX_EVENT_RESTART);

	if (!READ_ONCE(pcm_fault) || urb->status || ucx_urb_pair(urb) ||
	    READ_ONCE(ucx->pcm_state) != UCX_PCM_RUNNING)
		return;
	switch (xchg(&pcm_fault, 0)) {
	case 1:
		urb->status = -EPROTO;
		break;
	case 2:
		urb->actual_length -= min_t(unsigned int, urb->actual_length,
					    ucx->urb_bytes / ucx->blocks);
		break;
	case 3:
		ucx_pcm_events(ucx, &restart, 1);
		break;
	}
}
#endif

static void ucx_playback_complete(struct urb *urb)
{
	struct ucx *ucx = urb->context;
	struct snd_pcm_substream *elapsed;

	if (ucx_urb_stopped(ucx, urb))
		return;
	elapsed = ucx_copy(ucx, &ucx->play, ucx_urb_pair(urb), urb->transfer_buffer,
			   ucx->urb_frames, true);
	ucx_resubmit(ucx, urb);
	ucx_period_elapsed(&ucx->play, elapsed);
}

/*
 * Collect the S/PDIF input's channel status from the low byte of its samples
 * (UCX_SPDIF_CS_* in ucx_proto.h). Runs for every capture URB of the endpoint
 * that carries the channel, whether or not a PCM is attached.
 */
static void ucx_spdif_status(struct ucx *ucx, unsigned int pair, const u8 *buf,
			     unsigned int frames)
{
	unsigned int stride = ucx->pair_bytes;
	unsigned int at = UCX_SPDIF_IN_CHANNEL * UCX_SAMPLE_BYTES;
	unsigned int pos, i, bit;
	unsigned long flags;

	if (pair != ucx->pairs - 1)
		return;
	/* At quad speed channels 6..11 arrive on pair 1. */
	buf += at - pair * stride;
	/* A position past the end ignores data until a new marker. */
	pos = ucx->cs_pos;
	for (i = 0; i < frames; i++, pos++) {
		u8 status = buf[i * stride];

		if (status & UCX_SPDIF_CS_BLOCK) {
			if (pos == UCX_SPDIF_CS_BITS) {
				spin_lock_irqsave(&ucx->cs_lock, flags);
				memcpy(ucx->cs, ucx->cs_work, sizeof(ucx->cs));
				ucx->cs_jiffies = jiffies | 1;
				spin_unlock_irqrestore(&ucx->cs_lock, flags);
			}
			memset(ucx->cs_work, 0, sizeof(ucx->cs_work));
			pos = 0;
		}
		if ((status & UCX_SPDIF_CS) && pos < UCX_SPDIF_CS_BITS) {
			bit = (pos + UCX_SPDIF_CS_SHIFT) % UCX_SPDIF_CS_BITS;
			ucx->cs_work[bit / 8] |= 1 << (bit % 8);
		}
	}
	ucx->cs_pos = min_t(unsigned int, pos, UCX_SPDIF_CS_BITS + 1);
}

/*
 * "IEC958 Capture Default": the channel status the S/PDIF coaxial input
 * receives. Only known while the streams run (any PCM open) and the input
 * has a signal; all zeros otherwise.
 */
static int ucx_iec958_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	info->count = 1;
	return 0;
}

static int ucx_iec958_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned long flags;

	spin_lock_irqsave(&ucx->cs_lock, flags);
	if (ucx->cs_jiffies && time_before(jiffies, ucx->cs_jiffies + HZ / 4))
		memcpy(value->value.iec958.status, ucx->cs, sizeof(ucx->cs));
	spin_unlock_irqrestore(&ucx->cs_lock, flags);
	return 0;
}

static const struct snd_kcontrol_new ucx_iec958_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	.name = SNDRV_CTL_NAME_IEC958("", CAPTURE, DEFAULT),
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = ucx_iec958_info,
	.get = ucx_iec958_get,
};

/*
 * Stream errors since the unit appeared (struct ucx): read-only, and only
 * read when asked, so they cost the streams nothing
 */
static int ucx_errors_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = INT_MAX;
	return 0;
}

static int ucx_errors_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);

	value->value.integer.value[0] =
		atomic_read(kctl->private_value ? &ucx->stream_restarts : &ucx->unit_errors);
	return 0;
}

static const struct snd_kcontrol_new ucx_errors_ctls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_CARD,
		.name = "Stream Errors",
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = ucx_errors_info,
		.get = ucx_errors_get,
		.private_value = 0,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_CARD,
		.name = "Stream Restarts",
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = ucx_errors_info,
		.get = ucx_errors_get,
		.private_value = 1,
	},
};

static void ucx_capture_complete(struct urb *urb)
{
	struct ucx *ucx = urb->context;
	struct snd_pcm_substream *elapsed;
	unsigned int pair;

#ifdef UCX_PCM_TEST
	ucx_pcm_inject_fault(ucx, urb);
#endif
	if (ucx_urb_stopped(ucx, urb))
		return;
	pair = ucx_urb_pair(urb);
	ucx_spdif_status(ucx, pair, urb->transfer_buffer, ucx->urb_frames);
	elapsed = ucx_copy(ucx, &ucx->capture, pair, urb->transfer_buffer,
			   ucx->urb_frames, false);
	ucx_resubmit(ucx, urb);
	ucx_period_elapsed(&ucx->capture, elapsed);
}

static int ucx_alloc_urbs(struct ucx *ucx, struct ucx_stream *s)
{
	unsigned int pair, i;

	for (pair = 0; pair < UCX_MAX_PAIRS; pair++) {
		for (i = 0; i < UCX_URBS_PER_EP; i++) {
			struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);

			if (!urb)
				return -ENOMEM;
			s->urbs[pair][i] = urb;
			urb->transfer_buffer = usb_alloc_coherent(ucx->udev, UCX_URB_BYTES_MAX,
								  GFP_KERNEL, &urb->transfer_dma);
			if (!urb->transfer_buffer)
				return -ENOMEM;
		}
	}
	return 0;
}

static void ucx_free_urbs(struct ucx *ucx, struct ucx_stream *s)
{
	unsigned int pair, i;

	for (pair = 0; pair < UCX_MAX_PAIRS; pair++) {
		for (i = 0; i < UCX_URBS_PER_EP; i++) {
			struct urb *urb = s->urbs[pair][i];

			if (!urb)
				continue;
			if (urb->transfer_buffer)
				usb_free_coherent(ucx->udev, UCX_URB_BYTES_MAX,
						  urb->transfer_buffer, urb->transfer_dma);
			usb_free_urb(urb);
			s->urbs[pair][i] = NULL;
		}
	}
}

static void ucx_kill_urbs(struct ucx *ucx)
{
	unsigned int pair, i;

	for (pair = 0; pair < UCX_MAX_PAIRS; pair++) {
		for (i = 0; i < UCX_URBS_PER_EP; i++) {
			usb_kill_urb(ucx->play.urbs[pair][i]);
			usb_kill_urb(ucx->capture.urbs[pair][i]);
		}
	}
}

static struct usb_endpoint_descriptor *ucx_find_ep(struct ucx *ucx, u8 address)
{
	struct usb_host_interface *alt = ucx->intf->cur_altsetting;
	unsigned int i;

	for (i = 0; i < alt->desc.bNumEndpoints; i++)
		if (alt->endpoint[i].desc.bEndpointAddress == address)
			return &alt->endpoint[i].desc;
	return NULL;
}

static const u8 ucx_out_eps[UCX_MAX_PAIRS] = { UCX_EP_PCM_OUT, UCX_EP_PCM_OUT2 };
static const u8 ucx_in_eps[UCX_MAX_PAIRS] = { UCX_EP_PCM_IN, UCX_EP_PCM_IN2 };

/*
 * The start sequence verified with ucxctl, taken from the Windows driver:
 * 0x14000c670 (alt setting, rate), 0x14000ccb0 (REG1, START_AT, URBs) and the
 * ASIO start 0x140015df0 (CHANNELS, without which playback is ignored).
 */
static int ucx_engine_start(struct ucx *ucx, unsigned int rate, unsigned int blocks)
{
	struct usb_endpoint_descriptor *out[UCX_MAX_PAIRS], *in[UCX_MAX_PAIRS];
	unsigned int pairs = ucx_stream_pairs(rate), block = ucx_bytes_per_block(rate);
	unsigned int value, index, i;
	int pair, err;

	lockdep_assert_held(&ucx->lock);

	/* The alt setting is the speed class; it sizes the endpoints for one block */
	err = usb_set_interface(ucx->udev, UCX_INTF_PCM, ucx_speed_class(rate));
	if (err)
		return err;
	for (pair = 0; pair < pairs; pair++) {
		out[pair] = ucx_find_ep(ucx, ucx_out_eps[pair]);
		in[pair] = ucx_find_ep(ucx, ucx_in_eps[pair]);
		if (!out[pair] || !in[pair] || usb_endpoint_maxp(out[pair]) != block ||
		    usb_endpoint_maxp(in[pair]) != block) {
			dev_err(&ucx->intf->dev, "unexpected stream endpoints at %u Hz\n", rate);
			err = -ENODEV;
			goto err_alt;
		}
	}

	/*
	 * Writing the rate bits makes the device reload its stored mixer setup,
	 * a few seconds later, even when the rate doesn't change. So, like the
	 * Windows driver (0x14000c670), only write them when the rate changes.
	 * After load, ask the device which rate it runs at.
	 */
	if (!ucx->hw_rate) {
		u32 status;

		if (!ucx_vendor_read32(ucx, UCX_REQ_STATUS, &status))
			ucx->hw_rate = ucx_status_rate(status);
	}
	if (rate != ucx->hw_rate) {
		err = ucx_vendor_write(ucx, UCX_REQ_REG1, ucx_reg1_rate(rate), UCX_REG1_RATE_MASK);
		if (!err)
			ucx->hw_rate = rate;
	}

	ucx->pairs = pairs;
	ucx->pair_bytes = ucx_pair_bytes(rate);
	ucx->cs_pos = UCX_SPDIF_CS_BITS + 1;
	ucx->urb_frames = blocks * ucx_frames_per_block(rate);
	ucx->urb_bytes = blocks * block;
	if (!err)
		err = ucx_vendor_write(ucx, UCX_REQ_REG1,
				       ucx_reg1_large_buffer(rate, ucx->urb_frames),
				       UCX_REG1_LARGE_BUFFER);
	if (!err)
		err = ucx_vendor_write(ucx, UCX_REQ_START_AT, 0, 0);
	if (err)
		goto err_alt;

	for (pair = 0; pair < pairs; pair++) {
		for (i = 0; i < UCX_URBS_PER_EP; i++) {
			struct urb *urb = ucx->play.urbs[pair][i];

			memset(urb->transfer_buffer, 0, ucx->urb_bytes);
			usb_fill_int_urb(urb, ucx->udev,
					 usb_sndintpipe(ucx->udev, ucx_out_eps[pair]),
					 urb->transfer_buffer, ucx->urb_bytes,
					 ucx_playback_complete, ucx, out[pair]->bInterval);
			urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
			urb = ucx->capture.urbs[pair][i];
			usb_fill_int_urb(urb, ucx->udev,
					 usb_rcvintpipe(ucx->udev, ucx_in_eps[pair]),
					 urb->transfer_buffer, ucx->urb_bytes,
					 ucx_capture_complete, ucx, in[pair]->bInterval);
			urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		}
	}

	/* The unit's start burst on 0x8b follows. */
	spin_lock_irq(&ucx->stream_lock);
	memset(ucx->play.seq, 0, sizeof(ucx->play.seq));
	memset(ucx->capture.seq, 0, sizeof(ucx->capture.seq));
	ucx->started = jiffies;
	WRITE_ONCE(ucx->pcm_state, UCX_PCM_STARTING);
	spin_unlock_irq(&ucx->stream_lock);

	/* In the Windows driver's order: 0x03, 0x84, 0x01, 0x82 */
	for (pair = pairs - 1; pair >= 0; pair--) {
		for (i = 0; i < UCX_PLAY_URBS; i++) {
			err = usb_submit_urb(ucx->play.urbs[pair][i], GFP_KERNEL);
			if (err)
				goto err_kill;
		}
		for (i = 0; i < UCX_URBS_PER_EP; i++) {
			err = usb_submit_urb(ucx->capture.urbs[pair][i], GFP_KERNEL);
			if (err)
				goto err_kill;
		}
	}

	ucx_channels_req(BIT(ucx_channels(rate)) - 1, &value, &index);
	err = ucx_vendor_write(ucx, UCX_REQ_CHANNELS, value, index);
	if (err)
		goto err_kill;

	spin_lock_irq(&ucx->stream_lock);
	if (ucx->pcm_state == UCX_PCM_FAILED) {
		err = -EIO;
	} else {
		ucx->rate = rate;
		ucx->blocks = blocks;
		WRITE_ONCE(ucx->pcm_state, UCX_PCM_RUNNING);
	}
	spin_unlock_irq(&ucx->stream_lock);
	if (!err)
		return 0;

err_kill:
	spin_lock_irq(&ucx->stream_lock);
	WRITE_ONCE(ucx->pcm_state, UCX_PCM_STOPPED);
	spin_unlock_irq(&ucx->stream_lock);
	ucx_kill_urbs(ucx);
err_alt:
	usb_set_interface(ucx->udev, UCX_INTF_PCM, 0);
	return err;
}

static void ucx_engine_stop(struct ucx *ucx, bool suspend)
{
	enum ucx_pcm_state previous;

	lockdep_assert_held(&ucx->lock);
	spin_lock_irq(&ucx->stream_lock);
	previous = ucx->pcm_state;
	if (previous != UCX_PCM_SUSPENDED)
		WRITE_ONCE(ucx->pcm_state, suspend ? UCX_PCM_SUSPENDED : UCX_PCM_STOPPED);
	spin_unlock_irq(&ucx->stream_lock);
	if (previous == UCX_PCM_STOPPED || previous == UCX_PCM_SUSPENDED)
		return;

	ucx_kill_urbs(ucx);
	if (!suspend && !ucx->disconnected)
		usb_set_interface(ucx->udev, UCX_INTF_PCM, 0);
}

/* Restarts after stream errors allowed per UCX_STREAM_ERR_WINDOW */
#define UCX_STREAM_RESTARTS	5
#define UCX_STREAM_ERR_WINDOW	(10 * HZ)

/*
 * Drain old callbacks and detach the running streams via XRUN before
 * submitting fresh URBs. Prepared streams can start on the replacement
 * engine, which uses the same geometry. If it cannot restart, the next
 * prepare can try again.
 */
void ucx_stream_err_work(struct work_struct *work)
{
	struct ucx *ucx = container_of(work, struct ucx, stream_err_work);
	unsigned int dir;
	int err = -EIO;

	mutex_lock(&ucx->lock);
	if (READ_ONCE(ucx->pcm_state) != UCX_PCM_FAILED || ucx->disconnected)
		goto unlock;
	ucx_engine_stop(ucx, false);

	/*
	 * Prepared substreams have a runtime until hw_free takes ucx->lock.
	 * snd_pcm_stop_xrun() calls trigger, which takes only stream_lock.
	 */
	for (dir = 0; dir < 2; dir++)
		if (ucx->prepared & BIT(dir))
			snd_pcm_stop_xrun(ucx->pcm->streams[dir].substream);

	if (time_after(jiffies, ucx->err_window + UCX_STREAM_ERR_WINDOW)) {
		ucx->err_window = jiffies;
		ucx->err_restarts = 0;
	}
	if (ucx->err_restarts++ < UCX_STREAM_RESTARTS) {
		atomic_inc(&ucx->stream_restarts);
		err = ucx_engine_start(ucx, ucx->rate, ucx->blocks);
	}
	if (err)
		dev_err(&ucx->intf->dev, "streams stopped after errors (%d)\n", err);
unlock:
	mutex_unlock(&ucx->lock);
}

/* Rate ranges of the speed classes, and the channels each one carries */
static const struct {
	unsigned int rate_min, rate_max, channels;
} ucx_classes[] = {
	{ 32000, 48000, 18 },
	{ 64000, 96000, 14 },
	{ 128000, 192000, 12 },
};

static int ucx_rule_channels(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	const struct snd_interval *r = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval t = {
		.min = ucx_channels(r->max),
		.max = ucx_channels(r->min),
		.integer = 1,
	};

	return snd_interval_refine(hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS), &t);
}

static int ucx_rule_rate(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	const struct snd_interval *c = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval t = { .min = UINT_MAX, .max = 0, .integer = 1 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ucx_classes); i++) {
		if (!snd_interval_test(c, ucx_classes[i].channels))
			continue;
		t.min = min(t.min, ucx_classes[i].rate_min);
		t.max = max(t.max, ucx_classes[i].rate_max);
	}
	if (t.min > t.max)
		return -EINVAL;
	return snd_interval_refine(hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE), &t);
}

/*
 * Whole URBs per period keep period interrupts on time. A URB is urb_blocks
 * times 8, 16 or 32 frames; while the rate is still open, the smallest of
 * those applies.
 */
static int ucx_rule_period(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	const struct snd_interval *r = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *p = hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	unsigned int step = ucx_urb_blocks() * ucx_frames_per_block(r->min);
	struct snd_interval t = { .integer = 1 };

	if (p->max < step)
		return -EINVAL;
	t.min = roundup(p->min + p->openmin, step);
	t.max = rounddown(p->max - p->openmax, step);
	if (t.min > t.max)
		return -EINVAL;
	return snd_interval_refine(p, &t);
}

static int ucx_pcm_open(struct snd_pcm_substream *substream)
{
	struct ucx *ucx = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;

	runtime->hw = ucx_pcm_hw;
	err = snd_pcm_hw_constraint_msbits(runtime, 0, 32, UCX_SAMPLE_BITS);
	if (err < 0)
		return err;
	err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS, ucx_rule_channels,
				  NULL, SNDRV_PCM_HW_PARAM_RATE, -1);
	if (err < 0)
		return err;
	err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, ucx_rule_rate, NULL,
				  SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	if (err < 0)
		return err;
	err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, ucx_rule_period,
				  NULL, SNDRV_PCM_HW_PARAM_RATE, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  -1);
	if (err < 0)
		return err;

	/* Both directions run on the device clock: follow a running stream's rate */
	mutex_lock(&ucx->lock);
	if (READ_ONCE(ucx->pcm_state) == UCX_PCM_RUNNING)
		err = snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_RATE, ucx->rate);
	mutex_unlock(&ucx->lock);
	return err < 0 ? err : 0;
}

static int ucx_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static int ucx_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct ucx *ucx = snd_pcm_substream_chip(substream);

	mutex_lock(&ucx->lock);
	ucx->prepared &= ~BIT(substream->stream);
	if (!ucx->prepared)
		ucx_engine_stop(ucx, false);
	mutex_unlock(&ucx->lock);
	return 0;
}

static int ucx_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct ucx *ucx = snd_pcm_substream_chip(substream);
	struct ucx_stream *s = ucx_stream(ucx, substream->stream);
	unsigned int rate = substream->runtime->rate;
	unsigned int blocks;
	int err;

	mutex_lock(&ucx->lock);
	if (ucx->disconnected) {
		err = -ENODEV;
		goto unlock;
	}
	switch (READ_ONCE(ucx->pcm_state)) {
	case UCX_PCM_STOPPED:
		blocks = ucx_urb_blocks();
		if (substream->runtime->period_size < blocks * ucx_frames_per_block(rate)) {
			err = -EINVAL;
			goto unlock;
		}
		err = ucx_engine_start(ucx, rate, blocks);
		if (err)
			goto unlock;
		break;
	case UCX_PCM_RUNNING:
		if (ucx->rate == rate && substream->runtime->period_size >= ucx->urb_frames)
			break;
		err = ucx->rate == rate ? -EINVAL : -EBUSY;
		goto unlock;
	case UCX_PCM_SUSPENDED:
		err = -ESTRPIPE;
		goto unlock;
	default:
		/* The worker is about to recover the failed engine. */
		err = -EPIPE;
		goto unlock;
	}

	ucx->prepared |= BIT(substream->stream);
	spin_lock_irq(&ucx->stream_lock);
	memset(s->pos, 0, sizeof(s->pos));
	memset(s->done, 0, sizeof(s->done));
	s->reported = 0;
	s->hwptr = 0;
	s->period_pos = 0;
	substream->runtime->delay = 0;
	spin_unlock_irq(&ucx->stream_lock);
	err = 0;
unlock:
	mutex_unlock(&ucx->lock);
	return err;
}

static int ucx_pcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct ucx *ucx = snd_pcm_substream_chip(substream);
	struct ucx_stream *s = ucx_stream(ucx, substream->stream);

	/* STOP detached this direction. Its last notification can still be in flight. */
	wait_event(s->period_wait, !atomic_read_acquire(&s->period_inflight));
	return 0;
}

static int ucx_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct ucx *ucx = snd_pcm_substream_chip(substream);
	struct ucx_stream *s = ucx_stream(ucx, substream->stream);
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&ucx->stream_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (ucx->pcm_state != UCX_PCM_RUNNING ||
		    substream->runtime->rate != ucx->rate ||
		    substream->runtime->period_size < ucx->urb_frames) {
			err = -EPIPE;
			break;
		}
		s->armed = true;
		s->attached[0] = false;
		s->attached[1] = false;
		s->substream = substream;
		break;
	/* No SNDRV_PCM_INFO_RESUME: after a sleep the client prepares again */
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		s->substream = NULL;
		s->armed = false;
		break;
	default:
		err = -EINVAL;
	}
	spin_unlock_irqrestore(&ucx->stream_lock, flags);
	return err;
}

static snd_pcm_uframes_t ucx_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct ucx *ucx = snd_pcm_substream_chip(substream);
	struct ucx_stream *s = ucx_stream(ucx, substream->stream);
	unsigned long flags;
	unsigned int pos;

	spin_lock_irqsave(&ucx->stream_lock, flags);
	pos = s->hwptr;
	spin_unlock_irqrestore(&ucx->stream_lock, flags);
	return pos;
}

static const struct snd_pcm_ops ucx_pcm_ops = {
	.open		= ucx_pcm_open,
	.close		= ucx_pcm_close,
	.hw_free	= ucx_pcm_hw_free,
	.prepare	= ucx_pcm_prepare,
	.trigger	= ucx_pcm_trigger,
	.sync_stop	= ucx_pcm_sync_stop,
	.pointer	= ucx_pcm_pointer,
};

int ucx_pcm_init(struct ucx *ucx)
{
	struct snd_pcm *pcm;
	unsigned int i;
	int err;

	init_waitqueue_head(&ucx->play.period_wait);
	init_waitqueue_head(&ucx->capture.period_wait);
	err = ucx_alloc_urbs(ucx, &ucx->play);
	if (!err)
		err = ucx_alloc_urbs(ucx, &ucx->capture);
	if (err)
		return err;

	err = snd_pcm_new(ucx->card, "Fireface UCX", 0, 1, 1, &pcm);
	if (err < 0)
		return err;
	pcm->private_data = ucx;
	strscpy(pcm->name, "Fireface UCX", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ucx_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &ucx_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	ucx->pcm = pcm;
	spin_lock_init(&ucx->cs_lock);
	for (i = 0; i < ARRAY_SIZE(ucx_errors_ctls); i++) {
		err = snd_ctl_add(ucx->card, snd_ctl_new1(&ucx_errors_ctls[i], ucx));
		if (err < 0)
			return err;
	}
	return snd_ctl_add(ucx->card, snd_ctl_new1(&ucx_iec958_ctl, ucx));
}

/*
 * System sleep. Every prepared or running substream is suspended (the PCM
 * has no SNDRV_PCM_INFO_RESUME, so its client prepares it again after
 * resume, and that starts the engine). The unit may lose power while the
 * host sleeps, so its rate is read again at the next start.
 */
void ucx_pcm_suspend(struct ucx *ucx)
{
	ucx_pm_log(ucx, "suspend: PCM engine");
	/*
	 * Only the URBs: no usb_set_interface() back to alt 0 as
	 * ucx_engine_stop() does. That is a control transfer, and it
	 * unregisters and registers the endpoint devices while the PM core
	 * is suspending devices; snd-usb-audio doesn't either (it marks the
	 * interface for setup at the next prepare). ucx_engine_start() sets
	 * the alt setting on every start. Suspends with the alt reset hung
	 * the machine twice (2026-11-15; whether this was the cause is not
	 * proven).
	 */
	mutex_lock(&ucx->lock);
	ucx_engine_stop(ucx, true);
	ucx->hw_rate = 0;
	mutex_unlock(&ucx->lock);
	ucx_pm_log(ucx, "suspend: PCM substreams");
	snd_pcm_suspend_all(ucx->pcm);
	ucx_pm_log(ucx, "suspend: stream error work");
	/* Finds the engine stopped if it runs now */
	cancel_work_sync(&ucx->stream_err_work);
}

void ucx_pcm_resume(struct ucx *ucx)
{
	mutex_lock(&ucx->lock);
	spin_lock_irq(&ucx->stream_lock);
	WRITE_ONCE(ucx->pcm_state, UCX_PCM_STOPPED);
	spin_unlock_irq(&ucx->stream_lock);
	mutex_unlock(&ucx->lock);
}

/* Stop streaming for good and free the URBs; open PCMs then fail with -ENODEV */
void ucx_pcm_disconnect(struct ucx *ucx)
{
	mutex_lock(&ucx->lock);
	ucx->disconnected = true;
	ucx_engine_stop(ucx, false);
	mutex_unlock(&ucx->lock);
	/* After the URBs are killed, so nothing schedules it again */
	cancel_work_sync(&ucx->stream_err_work);
	ucx_free_urbs(ucx, &ucx->play);
	ucx_free_urbs(ucx, &ucx->capture);
}
