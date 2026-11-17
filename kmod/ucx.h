/* SPDX-License-Identifier: GPL-2.0 OR MIT */
#ifndef UCX_H
#define UCX_H

#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <sound/core.h>
#include <sound/pcm.h>

#include "ucx_proto.h"

/*
 * UCX_URB_BLOCKS (most blocks per URB, the urb_blocks parameter) and
 * UCX_PLAY_URBS (playback URBs kept queued per endpoint) are in ucx_proto.h,
 * which the tools share. A block is 8/16/32 frames at single/double/quad
 * speed, so it is always the same slice of time: 1/6000 s, 0.167 ms.
 */
/* Endpoint pairs carrying audio: two at quad speed */
#define UCX_MAX_PAIRS		2
/* The largest block is double speed's (ucx_bytes_per_block()) */
#define UCX_URB_BYTES_MAX	(UCX_URB_BLOCKS * 0x380)
#define UCX_MIN_CHANNELS	12
#define UCX_MAX_CHANNELS	18
/* Single-register controls in ucx_mixer.c's ucx_reg_ctls[] */
#define UCX_REG_CTLS		118
/* Clock and hardware settings in ucx_reg1_ctls[] */
#define UCX_REG1_CTLS		6
/* Mix controls: the 18 outputs and the FX send bus left/right */
#define UCX_MIX_OUTS		(UCX_DSP_OUTPUTS + 2)
/* Register space of DSP words: 15 bits */
#define UCX_REGS		0x8000
/* Mixer sources of each kind: 18 hardware inputs, or 18 playback channels */
#define UCX_MIX_SOURCES		18

/*
 * One direction of the duplex engine. At quad speed the channels are split
 * over two endpoints (pair 0: channels 0-5, pair 1: 6-11), which complete on
 * their own, so each pair keeps its own position in the ALSA buffer. The
 * position reported to ALSA is that of the pair that is behind.
 */
struct ucx_stream {
	struct urb *urbs[UCX_MAX_PAIRS][UCX_URBS_PER_EP];
	/* Attached by trigger; NULL streams silence (playback) or drops data (capture) */
	struct snd_pcm_substream *substream;
	/*
	 * URBs handled per pair since the engine started. URB n of both pairs
	 * covers the same slice of time, so a substream attaches to both pairs
	 * at the same number, start_seq. Trigger only arms the stream; the next
	 * capture completion picks start_seq (ucx_tick()).
	 */
	unsigned int seq[UCX_MAX_PAIRS];
	unsigned int start_seq;
	bool armed;
	bool attached[UCX_MAX_PAIRS];
	unsigned int pos[UCX_MAX_PAIRS];	/* frames into the ALSA buffer */
	unsigned int done[UCX_MAX_PAIRS];	/* frames since prepare */
	unsigned int reported;		/* frames since prepare that both pairs have done */
	unsigned int hwptr;		/* reported % buffer size */
	unsigned int period_pos;	/* frames since the last period boundary */
};

struct ucx {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct snd_card *card;
	struct snd_pcm *pcm;

	/* Engine state: running, rate, prepared, disconnected */
	struct mutex lock;
	bool running;
	bool disconnected;
	unsigned int rate;
	/* Stream layout at that rate, fixed while the engine runs */
	unsigned int pairs;
	unsigned int pair_bytes;	/* bytes per frame on one endpoint */
	unsigned int urb_frames;
	unsigned int urb_bytes;
	unsigned int hw_rate;		/* device rate as last read or written; 0 = unknown */
	unsigned int prepared;		/* BIT(SNDRV_PCM_STREAM_*) */

	/* Taken in URB completions: substream pointers and positions */
	spinlock_t stream_lock;
	struct ucx_stream play;
	struct ucx_stream capture;
	/*
	 * A stream URB failed: the engine is restarted from stream_err_work.
	 * Set under stream_lock, cleared when the engine starts.
	 */
	bool stream_err;
	struct work_struct stream_err_work;
	unsigned long err_window;	/* jiffies: start of the restart count */
	unsigned int err_restarts;	/* restarts since err_window */

	/*
	 * Stream errors since the unit appeared, the two numbers of "Errors"
	 * in the Windows Settings dialog: errors the unit reports on 0x8b
	 * while the streams run (UCX_EVENT_ERROR), and restarts of the streams
	 * after a failed URB (Windows: dev+0x81e8 and dev+0x81ec). Windows
	 * zeroes them at every stream start; these only count up.
	 */
	atomic_t unit_errors;
	atomic_t stream_restarts;
	unsigned long started;		/* jiffies when the engine last started */
	/*
	 * The unit's error words are counted: from the end of a start, and
	 * not once a stop begins. Stopping the URBs makes the unit report 17
	 * errors (measured 2026-11-17), which are no transmission errors.
	 */
	bool count_events;

	/* System sleep: interfaces suspended, and whether one was reset */
	unsigned int pm_count;
	bool pm_reset;
	bool pm_suspended;		/* ucx_pm_suspend() ran, ucx_pm_resume() not yet */

	/* S/PDIF input channel status, collected from the capture stream */
	spinlock_t cs_lock;		/* cs[], cs_jiffies */
	u8 cs[UCX_SPDIF_CS_BITS / 8];	/* last complete block */
	unsigned long cs_jiffies;	/* when it completed; 0 = never */
	u8 cs_work[UCX_SPDIF_CS_BITS / 8];	/* URB completion only */
	unsigned int cs_pos;
	bool cs_synced;

	/* Mixer: DSP command words on interface 1 */
	struct usb_interface *aux_intf;
	struct usb_interface *status_intf;	/* interface 4: device events on 0x8b */
	struct usb_interface *midi_intf;	/* interface 2: USB MIDI */
	struct list_head midi_list;	/* for snd-usbmidi-lib */
	struct urb *event_urb;
	struct delayed_work poll_work;	/* TotalMix-style 0x3f00000N polls */
	unsigned int poll_count;
	struct mutex dsp_lock;		/* dsp_buf, dsp_gone and the caches below */
	bool dsp_gone;
	bool dsp_suspended;
	/*
	 * The command endpoint timed out (the unit's word buffer overflowed).
	 * Until a probe transfer gets through, senders fail at once, and one
	 * probe is tried per second (dsp_retry).
	 */
	bool dsp_wedged;
	unsigned long dsp_retry;
	/* Reply and event URBs that stopped on an error, restarted by recover_work */
	struct delayed_work recover_work;
	int reply_status, event_status;	/* their last error, 0 = running */
	unsigned int recover_count;	/* attempts since recover_jiffies */
	unsigned long recover_jiffies;
	__le32 *dsp_buf;		/* DMA-able, so not on the stack */
	struct urb *reply_urb;
	/* Register values reported on 0x8d (see ucx_store_replies()) */
	spinlock_t regs_lock;
	u16 regs[UCX_REGS];
	DECLARE_BITMAP(reg_seen, UCX_REGS);
	bool dump_seen;			/* the device has sent its state dump */
	struct dentry *debugfs;
	/* Controls to notify when the device reports a change */
	struct snd_kcontrol *vol_kctl[UCX_DSP_OUTPUTS];
	struct snd_kcontrol *mix_kctl[2][UCX_MIX_OUTS];		/* [kind][out] */
	struct snd_kcontrol *mixinv_kctl[2][UCX_MIX_OUTS];	/* phase invert switches */
	struct snd_kcontrol *reg_kctl[UCX_REG_CTLS];		/* ucx_reg_ctls[] */
	struct snd_kcontrol *reg1_kctl[UCX_REG1_CTLS];		/* ucx_reg1_ctls[] */
};

extern struct usb_driver ucx_driver;

/* Log each step of suspend and resume (module parameter pm_debug) */
extern bool ucx_pm_debug;
#define ucx_pm_log(ucx, fmt, ...)						\
	do {									\
		if (ucx_pm_debug)						\
			dev_info(&(ucx)->intf->dev, "pm: " fmt "\n", ##__VA_ARGS__); \
	} while (0)

int ucx_vendor_write(struct ucx *ucx, u8 request, u16 value, u16 index);
int ucx_vendor_read32(struct ucx *ucx, u8 request, u32 *value);

int ucx_pcm_init(struct ucx *ucx);
void ucx_stream_err_work(struct work_struct *work);
void ucx_pcm_suspend(struct ucx *ucx);
void ucx_pcm_disconnect(struct ucx *ucx);

int ucx_mixer_init(struct ucx *ucx);
void ucx_poll_work(struct work_struct *work);
void ucx_recover_work(struct work_struct *work);
void ucx_mixer_suspend(struct ucx *ucx);
void ucx_mixer_resume(struct ucx *ucx);
void ucx_mixer_disconnect(struct ucx *ucx);

/*
 * From snd-usbmidi-lib, the USB MIDI class driver that snd-usb-audio uses.
 * Its header (sound/usb/midi.h) is private to the kernel tree, so the two
 * exported functions used here are declared by hand.
 */
struct snd_usb_audio_quirk;
int __snd_usbmidi_create(struct snd_card *card, struct usb_interface *iface,
			 struct list_head *midi_list, const struct snd_usb_audio_quirk *quirk,
			 unsigned int usb_id, unsigned int *num_rawmidis);
void snd_usbmidi_disconnect(struct list_head *p);
void snd_usbmidi_suspend(struct list_head *p);
void snd_usbmidi_resume(struct list_head *p);

#endif /* UCX_H */
