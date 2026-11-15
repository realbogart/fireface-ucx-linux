/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * RME Fireface UCX USB protocol in "RME driver" mode (not class compliant).
 *
 * Recovered from RME's Windows driver fireface_usb_amd.sys 1.2.77
 * (sha256 5bc4285463cc020218e002b5bb43b3f8b95dc003fd1859be4fd10566ff7dea23).
 * Addresses refer to that binary (image base 0x140000000) so every fact can
 * be re-checked. UNVERIFIED marks things not yet confirmed on hardware.
 *
 * Shared by the kernel module and the userspace tools: plain C only.
 */
#ifndef UCX_PROTO_H
#define UCX_PROTO_H

/* fireface_usb.inf binds both vendor IDs */
#define UCX_VID_RME		0x2a39
#define UCX_VID_MICROCHIP	0x0424

#define UCX_PID_RME_MODE	0x3fc9
/* Class compliant (UAC2) personality, handled by snd-usb-audio */
#define UCX_PID_CC_MODE		0x3fb9

/*
 * The driver reads the device descriptor and maps idProduct - 0x3f81
 * through a jump table (0x140011a15) to an internal model id:
 *   0x3fc5 -> 0x00  0x3fc7 -> 0x01 (Babyface)  0x3fc8 -> 0x02 (UFX)
 *   0x3fc9 -> 0x03 (UCX)  0x3fcd -> 0x08 (802)  0x3fdd -> 0x09 (802)
 *   0x3fce -> 0x0a  0x3fc0 -> 0x0d (Babyface Pro)  0x3f82 -> 0x20 (UCX II)
 *   0x3f81 -> 0x21  0x3f87 -> 0x2b
 */
#define UCX_MODEL_ID		0x03

/*
 * Interfaces in RME mode, all class 0xff except MIDI (from `ucxctl info`).
 * The Windows driver submits URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER for
 * both bulk and interrupt endpoints.
 */

/* Interface 0: isochronous 0x0e/0x8f, alt 1-3. Not used by the Windows driver? */
#define UCX_INTF_ISO		0

/*
 * Interface 1: command channel. Buffers for 0x0c come only from the IOCTL
 * handler (0x140017390 -> 0x1400110c0), i.e. from TotalMix FX; replies on
 * 0x8d go into a 0x3000-byte ring (0x140010520). The UCX mixer most likely
 * lives here, in a format only TotalMix knows. UNVERIFIED.
 */
#define UCX_INTF_AUX		1
#define UCX_EP_AUX_OUT		0x0c
#define UCX_EP_AUX_IN		0x8d
/*
 * Level meters, one bank of 20 channels per bulk read (0x140012e80;
 * TotalMix 0x140077e00 reads 5 banks): u64 RMS[20] at 0x00 (scale 2^-55),
 * u32 peak[20] at 0xa0 (scale 2^-27, low 4 bits are flags), u32 bank tag
 * at 0xf0. Banks, verified with a test tone: 0x11111111 inputs after the
 * channel EQ, 0x22222222 playback, 0x33333333 outputs before output volume,
 * 0x55555555 inputs before the EQ (raw), 0x66666666 outputs after volume.
 */
#define UCX_EP_METERS		0x85
#define UCX_METER_CHANNELS	20
#define UCX_METER_PEAK_OFFSET	0xa0
#define UCX_METER_TAG_OFFSET	0xf0
#define UCX_METER_MIN_BYTES	0xf4

/* Interface 2: USB-MIDI class (01/03), bulk. OUT 0x140011130, IN 0x1400040c0 */
#define UCX_INTF_MIDI		2
#define UCX_EP_MIDI_OUT		0x07
#define UCX_EP_MIDI_IN		0x86

/* Interface 3: flash data, 256-byte blocks. Write 0x1400114c0, read 0x140011650. See DANGER below. */
#define UCX_INTF_CTRL		3
#define UCX_EP_CTRL_IN		0x88
#define UCX_EP_CTRL_OUT		0x09
#define UCX_CTRL_BLOCK_SIZE	256

/* Interface 4: 4-byte interrupt IN; completion reads two u32 (0x14000c530). Status? */
#define UCX_INTF_STATUS		4
#define UCX_EP_STATUS_IN	0x8b

/*
 * Interface 5: audio over interrupt endpoints. wMaxPacketSize of each alt
 * equals ucx_bytes_per_block(), so one packet carries one block. The alt
 * setting is the speed class the driver keeps at dev+0x7ebc (1 single,
 * 2 double, 3 quad); in alt 3 the second pair 0x03/0x84 also carries audio
 * (0x140014c70). Streams are started by 0x14000ccb0.
 */
#define UCX_INTF_PCM		5
#define UCX_EP_PCM_OUT		0x01
#define UCX_EP_PCM_IN		0x82
#define UCX_EP_PCM_OUT2		0x03
#define UCX_EP_PCM_IN2		0x84

/* Transfers kept queued per streaming endpoint (0x140010a00) */
#define UCX_URBS_PER_EP		8

/*
 * Vendor device requests, bmRequestType 0x40 (OUT) / 0xc0 (IN), built by
 * 0x140014300 as URB_FUNCTION_VENDOR_DEVICE. The request numbers match
 * RME's Babyface Pro (sound/usb/mixer_quirks.c, SND_BBFPRO_USBREQ_*).
 *
 * REG1: masked register write, wValue = new bits, wIndex = mask. At stream
 * start the driver sets bit 15 from ucx_reg1_large_buffer().
 * REG2: second register, same scheme. For the UCX, Windows writes only the
 * level fields (UCX_REG2_* below), at every device start. The ADC field had no effect on
 * the UCX's analog inputs, the DAC field none on Line Out 3, the Phones
 * field none on Phones L (all measured): the UCX ignores bits 6-11.
 * MIXER: 18-bit linear volume, wValue = vol >> 2,
 * wIndex = index | (vol & 3) << 14. Used only for models 0, 1, 0xa and
 * 0xd (0x14000e3d0), NOT the UCX.
 * CLOCK: the four rate-family frequencies (32k / 44.1k / 48k / 50k slots,
 * as REG1 bits 0x30), encoded by ucx_dds_req(); sent at every start and
 * resume (0x14000c380). It sets the real sample clock: sent to a running
 * unit, both an 8-bit-off misreading and Windows' exact words changed a
 * 48 kHz stream (to ~23.7 / ~28.5 kHz) until a power cycle (measured).
 * Why Windows' words work for Windows is unresolved. Never send it from
 * the driver; it relies on the unit's power-up values.
 * START_RESET (0x16): wValue 0 for wIndex 0..29 and 0x20..0x3d, the first
 * thing sent at every start and resume (0x14000e720). Meaning unknown; all
 * 60 sent on a running unit changed nothing measured.
 * START_AT: sent after REG1 on every stream start (0x14000ccb0). wValue 0
 * starts now; ((usb_frame + 50) & 0x7ffe) | 0x8000 starts at that frame,
 * which keeps several devices in sync (frame from 0x1400107e0).
 * CHANNELS: sent by the ASIO start (IOCTL 0x22601c, 0x140015df0) after the
 * streams are running, with the low half of the client's 64-bit channel
 * mask (0x14000d1c0). Required for playback: without it the device accepts
 * OUT packets but the DSP never sees them (verified on hardware). It is the
 * counterpart of LATTER_FETCH_MODE on FireWire. Encoding: ucx_channels_req().
 * STATUS (0x11, IN, 4 bytes): same layout as the FireWire latter status
 * register; bits 27:24 = active rate code (ucx_status_rate()), bits 11:9 =
 * clock source (0x7 internal). Read 0x02f0ff00 at 48 kHz, internal clock.
 * 0x1c (IN, 4 bytes): 0x00310103; the Windows driver keeps bits 31:16
 * (0x14000afc0), probably the firmware version.
 * 0x1e, 0x1f (IN, 4 bytes each): the serial number (0x14000b030).
 * CHANNELS_OFF (0x13): same encoding as CHANNELS; sent when a client
 * releases channels that no other client uses (0x14000b990, called from
 * 0x140016080 after the per-channel use counts at settings +0x38 drop).
 * LOOPBACK (0x15): one request per output, wIndex = output 0..29, wValue =
 * 1 on / 0 off. TotalMix builds a mask of every output's Loopback button
 * (0x14001a5b0) and sends it with IOCTL 0x226004; the driver (0x14000c0c0)
 * then sends this request for each of the 30 channels.
 * GAIN (0x1a): wIndex = channel + 4 (UCX), wValue = value; IOCTL 0x226048
 * (0x14000e300) and the "Mic Gain" settings (0x14000d240). Not used here.
 *
 * DANGER: FLASH_WRITE commits a 256-byte block that was just sent on
 * UCX_EP_CTRL_OUT, and FLASH_STATUS is polled until bits 1:0 clear
 * (0x14000ed40, 0x14000ec60). That is flash programming (firmware or stored
 * setups). Never send these.
 */
#define UCX_REQ_REG1		0x10
#define UCX_REQ_MIXER		0x12
#define UCX_REQ_REG2		0x17
#define UCX_REQ_FLASH_WRITE	0x18
#define UCX_REQ_FLASH_STATUS	0x19
#define UCX_REQ_CLOCK		0x1b
#define UCX_REQ_START_AT	0x1d
#define UCX_REQ_CHANNELS	0x14
#define UCX_REQ_STATUS		0x11
#define UCX_REQ_CHANNELS_OFF	0x13
#define UCX_REQ_LOOPBACK	0x15
#define UCX_REQ_GAIN		0x1a
#define UCX_REQ_START_RESET	0x16

/*
 * REG2 level fields (0x14000b5b0). These are the Fireface UC's global
 * reference levels; on the UCX the levels are per-channel DSP registers and
 * the settings app has no page for these, so they keep their defaults.
 * ADC field: no effect on the UCX inputs' noise floor (measured, while a
 * DSP level switch moves it 6 dB). DAC: no effect on Line Out 3 (measured,
 * while 1208 moves it 8.8 dB). Phones: no effect on Phones L (measured
 * with -10 dBV and +4 dBu, while 1808 moves it 8.8 dB). No control.
 */
#define UCX_REG2_LEVEL_MASK		0x0fc0	/* mask Windows uses for the UCX */
#define UCX_REG2_DAC_MASK		0x00c0
#define UCX_REG2_DAC_HI_GAIN		0x0080
#define UCX_REG2_DAC_PLUS4		0x00c0
#define UCX_REG2_DAC_MINUS10		0x0040
#define UCX_REG2_ADC_MASK		0x0300
#define UCX_REG2_ADC_LO_GAIN		0x0000
#define UCX_REG2_ADC_PLUS4		0x0100
#define UCX_REG2_ADC_MINUS10		0x0300
#define UCX_REG2_PHONES_MASK		0x0c00
#define UCX_REG2_PHONES_HI_GAIN		0x0400
#define UCX_REG2_PHONES_PLUS4		0x0c00
#define UCX_REG2_PHONES_MINUS10		0x0800
/*
 * Level bits a Windows start sends with the registry defaults: ADC -10 dBV,
 * DAC 0 (its u16 setting reads 0xffff, no combo code), Phones +4 dBu.
 */
#define UCX_REG2_WINDOWS_START		(UCX_REG2_ADC_MINUS10 | UCX_REG2_PHONES_PLUS4)

/*
 * CLOCK (0x1b) request for rate-family slot 0..3 at frequency hz, as
 * 0x14000c380: v = 1e8 * 2^24 / hz (constant 0x5f5e10000000). v has 36 bits;
 * wValue keeps bits 16..31, so the top nibble is never sent and the unit
 * must supply it itself (inferred).
 */
static inline void ucx_dds_req(unsigned int slot, unsigned int hz,
			       unsigned int *value, unsigned int *index)
{
	unsigned long long v = (100000000ULL << 24) / hz;

	*value = (v >> 16) & 0xffff;
	*index = (v & 0xff00) | (slot & 3);
}

/* UCX encoding of the CHANNELS mask (models 2, 3, 8, 0x20, 0x21, 0x2b) */
static inline void ucx_channels_req(unsigned int mask, unsigned int *value, unsigned int *index)
{
	*value = (mask >> 14) & 0xffff;
	*index = ((mask & 0x3fff) << 2) & 0xffff;
}

/* REG1 bit set at stream start for buffers longer than 32 blocks */
#define UCX_REG1_LARGE_BUFFER	0x8000

/*
 * REG1 bits 5:4 select the base rate within a speed class: 32k/44.1k/48k/50k
 * (times 2 or 4 at double/quad speed; verified for 44.1k and 48k by timing a
 * capture). The speed class itself is interface 5's alt setting: on a class
 * change the Windows driver selects the alt setting and then writes these
 * bits (0x14000c670). Writing them, even unchanged, has made the device
 * reload its stored DSP mixer setup a few seconds later. Only write on change.
 */
#define UCX_REG1_RATE_MASK	0x0030

/*
 * REG1 configuration bits for the UCX, the settings of RME's Settings dialog.
 * Built by the Windows driver in 0x14000b2f0 from its registry values (names
 * in brackets; the offsets they are stored at were matched in 0x14000d660)
 * and written with mask 0x5cf. The UCX II uses the same function, and
 * oscmix documents the same bits for it.
 *
 * The unit reports five of them back in DSP registers (verified by writing
 * each bit, 2026-11-12): the register follows the write and is in the dump.
 *
 * Clock source ["Sync Ref" 0-3 -> 0, 2, 4, 6]: register 0x3d20 then reads
 * 0-3 = internal, S/PDIF coaxial, optical, word clock: the FireWire UCX's
 * order (latter/ucx.rs CFG_CLK_SRC_*), not the UCX II's. Measured with both
 * digital inputs looped (2026-11-13): 2 -> status source coaxial, 4 ->
 * optical; 6 (word clock, no signal) fell back to coaxial. With the chosen
 * input silent the unit takes another input that has a signal (AutoSync).
 */
#define UCX_REG1_CLOCK_MASK	0x0007	/* mirrored in 0x3d20 as value >> 1 */
#define UCX_REG1_WCK_TERM	0x0008	/* ["WckTerm"] word clock input 75 ohm; 0x3d40 */
#define UCX_REG1_WCK_SINGLE	0x0040	/* ["Div Wclk"] word clock out stays at 32-48 kHz; 0x3d22 */
#define UCX_REG1_SPDIF_PRO	0x0080	/* ["Output Format"] coaxial out professional; 0x3d42 */
/*
 * ["ADC Gain", reused] 1 = the capture stream is the raw input, 0 = it has
 * the input's EQ and dynamics (measured on the S/PDIF loop; the opposite of
 * the UCX II bit as oscmix describes it). Not mirrored anywhere.
 */
#define UCX_REG1_RECORD_DRY	0x0100
#define UCX_REG1_OPTICAL_SPDIF	0x0400	/* ["Optical Out" == 1] optical out sends S/PDIF; 0x3d41 */
#define UCX_DSP_CLOCK_SOURCE	0x3d20
#define UCX_DSP_SAMPLE_RATE	0x3d21	/* index 0-8: 32 .. 192 kHz, set by the unit */
#define UCX_DSP_WCK_SINGLE	0x3d22
#define UCX_DSP_WCK_TERM	0x3d40
#define UCX_DSP_OPTICAL_SPDIF	0x3d41
#define UCX_DSP_SPDIF_PRO	0x3d42

/*
 * STATUS (request 0x11) bits, the layout of the FireWire UCX's status
 * register (snd-firewire-ctl-services latter/ucx.rs). Verified over USB
 * with an S/PDIF out -> in loop: rate code, internal (7) and coaxial (1) as
 * active clock, the coaxial input's rate, lock and sync bits (0x02f0ff00
 * without the loop, 0x02f02f11 with it). The word clock and optical fields
 * are from FireWire. Bit 8 stayed 1 in both optical output modes, so it is
 * not the FireWire "optical out is ADAT" flag here.
 */
#define UCX_STATUS_RATE(s)		((s) >> 24 & 0xf)	/* active; 0xf = none */
#define UCX_STATUS_WORD_RATE(s)		((s) >> 20 & 0xf)
#define UCX_STATUS_OPTICAL_RATE(s)	((s) >> 16 & 0xf)
#define UCX_STATUS_COAX_RATE(s)		((s) >> 12 & 0xf)
#define UCX_STATUS_CLOCK(s)		((s) >> 9 & 0x7)	/* 7 internal, 3 word, 2 optical, 1 coax */
#define UCX_STATUS_SYNC_WORD		0x0040
#define UCX_STATUS_SYNC_OPTICAL		0x0020
#define UCX_STATUS_SYNC_COAX		0x0010
#define UCX_STATUS_LOCK_WORD		0x0004
#define UCX_STATUS_LOCK_OPTICAL		0x0002
#define UCX_STATUS_LOCK_COAX		0x0001

/* REG1 bit 11: MIDI on, set before the first read of 0x86 (0x14000b8c0) */
#define UCX_REG1_MIDI		0x0800

/*
 * DSP commands: little-endian 32-bit words sent as bulk OUT on
 * UCX_EP_AUX_OUT. The words are the same as in RME's FireWire "latter"
 * protocol (snd-firewire-ctl-services, protocols/fireface/src/latter.rs).
 * The USB format came from TotalMixFX_x64.exe, class CCardIOUCX: encoder
 * 0x140068ca0, queue 0x140078870, flush 0x140068d00 (IOCTL 0x226058).
 *
 * bit 31 odd parity over the whole word
 * bit 30 1: mixer gain; 0: per-port command (ch << 24 | cmd << 16 | value)
 */
#define UCX_DSP_PARITY		0x80000000
#define UCX_DSP_MIXER		0x40000000
/* Mixer source index: inputs at 0..17, playback channels from 0x20 */
#define UCX_DSP_MIX_STEP	64
#define UCX_DSP_PLAYBACK	0x20
/* Per-port channel numbers: 18 physical inputs, then the outputs */
#define UCX_DSP_PHYS_INPUTS	18
#define UCX_DSP_OUTPUTS		18
/* Mixer source kinds (bit 5 of the source index) */
#define UCX_DSP_KIND_INPUT	0
#define UCX_DSP_KIND_PLAYBACK	1
/*
 * Mixer outputs 0x1e and 0x1f are the left and right FX send bus that feeds
 * reverb and echo (FX_MIXER_0/1 in latter.rs; verified on the FX bus meters,
 * channels 18/19 of the output banks).
 */
#define UCX_DSP_FX_OUT		0x1e
/*
 * Mixer gain: linear, 32768 = 0 dB, 65536 = +6 dB (TotalMix 0x140027250).
 * A negative gain inverts the phase (verified for -32768 = 0xf000).
 */
#define UCX_DSP_MIX_UNITY	32768
#define UCX_DSP_MIX_MAX		65536
/* Output volume in 0.1 dB */
#define UCX_DSP_VOL_MIN		(-650)
#define UCX_DSP_VOL_MAX		60
#define UCX_DSP_OUTPUT_VOL	0x00	/* value in 0.1 dB, -650..60 (verified) */
/*
 * Register 0x3f99 = 0x0812: the device answers with every register on 0x8d
 * (verified: 1636 registers). It also reports later changes, such as the
 * front-panel encoder, while the host polls with 0x3f00000N.
 */
#define UCX_DSP_DUMP_REQUEST	0x3f990812
/*
 * 0x3e06 = 0x0910 + n stores the unit's whole state to setup slot n in
 * flash (oscmix, TotalMix). 0x3e05 is next to it, guarded by a magic value
 * in TotalMix, and unknown. Never sent: the driver (hwdep, debugfs) and
 * ucxctl refuse words for both.
 */
#define UCX_DSP_REG_STORE	0x3e06
#define UCX_DSP_REG_STORE_NEAR	0x3e05
/* The register of a DSP word, mixer flag included (bits 30:16) */
#define UCX_DSP_WORD_REG(w)	((w) >> 16 & 0x7fff)
/* The device sends this register only in a dump: Line 1/2 stand-alone mix, first input */
#define UCX_DSP_DUMP_ONLY_REG	0x12a0

static inline unsigned int ucx_dsp_word(unsigned int w)
{
	w &= ~UCX_DSP_PARITY;
	return __builtin_parity(w) ? w : w | UCX_DSP_PARITY;
}

/*
 * Stand-alone input mix: the unit's own record of the hardware-input part of
 * the mixer, one level per output channel and input. It does not change the
 * audio (measured 2026-11-13: the words alone open no route); the unit keeps
 * it next to the mixer gains, which it never reports, and returns it in the
 * dump. TotalMix writes it with every input fader (0x14003e780) and reads it
 * back at start (0x14003cba0).
 *
 * Write: register (output channel << 8 | 0xe0 + input), level in 0.1 dB in
 * bits 15:4, bit 2 set = "this output's own level". The unit takes that form
 * for linked pairs too and stores the value as sent (-90.0 dB and +3.8 dB
 * came back unchanged). Without bit 2 the word is the level of a stereo
 * pair, and a second word with bit 3 set is its pan (pan * 100 in bits
 * 15:4); that is what TotalMix sends for a linked output pair.
 *
 * Dump: two registers A, B per input on the first output of each pair, from
 * 0xa0 (analog 1-8), 0xc0 (S/PDIF) and 0xe0 (ADAT). A bit 3 set: left level
 * in A bits 15:4, right level in 11 bits (A bits 2:0, B bits 7:0). Clear:
 * A >> 4 is the pair's level and B its pan, -100 .. +100.
 */
#define UCX_DSP_SA_MIX_CMD	0xe0
#define UCX_DSP_SA_MIX_SHIFT	4
#define UCX_DSP_SA_MIX_PAN	0x8	/* word: pan; dump register A: per-channel levels */
#define UCX_DSP_SA_MIX_MONO	0x4
#define UCX_DSP_SA_MIX_OFF	(-650)	/* 0.1 dB; what TotalMix writes for a closed fader */
#define UCX_DSP_SA_MIX_MAX	60
#define UCX_DSP_SA_MIX_MIN	(-903)	/* gain 1, the smallest */

/*
 * Mixer gain (32768 = 0 dB) of a level in 0.1 dB: round(32768 * 10^(t/200)),
 * at least 1, in integers so that the kernel can use it. The level is
 * written as 6.0 dB minus an attenuation whose bits select factors
 * 10^(-2^i/200) in 0.32 fixed point.
 */
static inline unsigned int ucx_tenth_db_to_gain(int tenth)
{
	static const unsigned int step[10] = {
		4245803212u, 4197201904u, 4101661925u, 3917056729u, 3572398195u,
		2971391394u, 2055700593u, 983920165u, 225403088u, 11829322u,
	};
	unsigned long long acc = 280808206818757ull;	/* 32768 * 10^(60/200) << 32 */
	unsigned int att, i;

	if (tenth > UCX_DSP_SA_MIX_MAX)
		tenth = UCX_DSP_SA_MIX_MAX;
	if (tenth < UCX_DSP_SA_MIX_MAX - 1023)
		tenth = UCX_DSP_SA_MIX_MAX - 1023;
	att = UCX_DSP_SA_MIX_MAX - tenth;
	for (i = 0; i < 10; i++)
		if (att & 1u << i)
			acc = (acc >> 32) * step[i] + (((acc & 0xffffffffu) * step[i]) >> 32);
	acc = (acc + 0x80000000u) >> 32;
	return acc ? (unsigned int)acc : 1;
}

/* The level in 0.1 dB whose gain is nearest to @gain (not 0) */
static inline int ucx_gain_to_tenth_db(unsigned int gain)
{
	int lo = UCX_DSP_SA_MIX_MIN, hi = UCX_DSP_SA_MIX_MAX;

	/* The lowest level that reaches @gain, then the nearer of it and the one below */
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;

		if (ucx_tenth_db_to_gain(mid) >= gain)
			hi = mid;
		else
			lo = mid + 1;
	}
	if (lo > UCX_DSP_SA_MIX_MIN &&
	    gain - ucx_tenth_db_to_gain(lo - 1) < ucx_tenth_db_to_gain(lo) - gain)
		lo--;
	return lo;
}

/*
 * Record level of a mixer gain. Off is -65.0 dB, as TotalMix writes and
 * reads it; a gain that rounds to exactly that is recorded as -65.1.
 */
static inline int ucx_dsp_sa_level(unsigned int gain)
{
	int tenth;

	if (!gain)
		return UCX_DSP_SA_MIX_OFF;
	tenth = ucx_gain_to_tenth_db(gain);
	return tenth == UCX_DSP_SA_MIX_OFF ? tenth - 1 : tenth;
}

static inline unsigned int ucx_dsp_sa_gain(int level)
{
	return level == UCX_DSP_SA_MIX_OFF ? 0 : ucx_tenth_db_to_gain(level);
}

/* The word that records @level for input @src on output channel @out */
static inline unsigned int ucx_dsp_sa_cmd(unsigned int out, unsigned int src, int level)
{
	return ucx_dsp_word((UCX_DSP_PHYS_INPUTS + out) << 24 |
			    (UCX_DSP_SA_MIX_CMD + src) << 16 |
			    ((unsigned int)level & 0xfff) << UCX_DSP_SA_MIX_SHIFT |
			    UCX_DSP_SA_MIX_MONO);
}

/* Dump register A of input @src for the pair of output @out; B is the next one */
static inline unsigned int ucx_dsp_sa_reg(unsigned int out, unsigned int src)
{
	unsigned int cmd = src < 8 ? 0xa0 + 2 * src : src < 10 ? 0xc0 + 2 * (src - 8) :
			   0xe0 + 2 * (src - 10);

	return (UCX_DSP_PHYS_INPUTS + (out & ~1u)) << 8 | cmd;
}

/* The input of dump register @cmd (A or B), or -1 if it is not part of the record */
static inline int ucx_dsp_sa_src(unsigned int cmd)
{
	if (cmd >= 0xa0 && cmd < 0xb0)
		return (cmd - 0xa0) / 2;
	if (cmd >= 0xc0 && cmd < 0xc4)
		return 8 + (cmd - 0xc0) / 2;
	if (cmd >= 0xe0 && cmd < 0xf0)
		return 10 + (cmd - 0xe0) / 2;
	return -1;
}

/* Dump registers A and B for a left and a right level, as the unit stores them */
static inline void ucx_dsp_sa_pack(int left, int right, unsigned short *a, unsigned short *b)
{
	*a = ((unsigned int)left & 0xfff) << UCX_DSP_SA_MIX_SHIFT | UCX_DSP_SA_MIX_PAN |
	     ((unsigned int)right >> 8 & 7);
	*b = (unsigned int)right & 0xff;
}

/*
 * The two levels in dump registers A and B. Returns 0 if they can't be told:
 * a pair level with a pan that is not hard left or right, which TotalMix
 * turns into two gains with its own pan law.
 */
static inline int ucx_dsp_sa_unpack(unsigned short a, unsigned short b, int *left, int *right)
{
	int level = (int)((a >> UCX_DSP_SA_MIX_SHIFT ^ 0x800) & 0xfff) - 0x800;
	int pan = (short)b;

	if (a & UCX_DSP_SA_MIX_PAN) {
		*left = level;
		*right = (int)((((a & 7) << 8 | (b & 0xff)) ^ 0x400) & 0x7ff) - 0x400;
		return 1;
	}
	if (level <= UCX_DSP_SA_MIX_OFF)
		level = UCX_DSP_SA_MIX_OFF;
	else if (pan != -100 && pan != 100)
		return 0;
	*left = pan <= 0 || level == UCX_DSP_SA_MIX_OFF ? level : UCX_DSP_SA_MIX_OFF;
	*right = pan >= 0 || level == UCX_DSP_SA_MIX_OFF ? level : UCX_DSP_SA_MIX_OFF;
	return 1;
}

/*
 * Mixer coefficient from gain * 32768 (0 dB = 32768): values from 0x4000 up
 * lose 3 bits of precision and get bit 15, so 0 dB is 0x9000 (0x140068ca0).
 */
static inline unsigned int ucx_dsp_mix_coef(int gain)
{
	if ((unsigned int)(gain + 0x4000) > 0x7fff)
		return 0x8000 | ((gain >> 3) & 0x7fff);
	return gain & 0x7fff;
}

/* @src: input channel, or UCX_DSP_PLAYBACK + playback channel */
static inline unsigned int ucx_dsp_mix_cmd(unsigned int out, unsigned int src, int gain)
{
	return ucx_dsp_word(UCX_DSP_MIXER | (out * UCX_DSP_MIX_STEP + src) << 16 |
			    ucx_dsp_mix_coef(gain));
}

static inline unsigned int ucx_dsp_output_vol_cmd(unsigned int out, int tenth_db)
{
	return ucx_dsp_word((UCX_DSP_PHYS_INPUTS + out) << 24 | UCX_DSP_OUTPUT_VOL << 16 |
			    (tenth_db & 0xffff));
}

/*
 * Stream transfers are sized in "blocks". A block is one interrupt packet
 * and a fixed slice of time (8 frames at single speed). One transfer carries
 * buffer_frames / frames_per_block blocks (0x14000ccb0).
 */
static inline unsigned int ucx_speed_class(unsigned int rate)
{
	if (rate > 100000)
		return 3;
	if (rate > 50000)
		return 2;
	return 1;
}

static inline unsigned int ucx_frames_per_block(unsigned int rate)
{
	if (rate > 100000)
		return 32;
	if (rate > 50000)
		return 16;
	return 8;
}

/* Model 3 uses the same size in both directions (0x14000a6a0 -> 0x140014c40) */
static inline unsigned int ucx_bytes_per_block(unsigned int rate)
{
	if (rate > 100000)
		return 0x300;
	if (rate > 50000)
		return 0x380;
	return 0x240;
}

/* Endpoint pairs carrying audio: two at quad speed (alt 3 of interface 5) */
static inline unsigned int ucx_stream_pairs(unsigned int rate)
{
	return ucx_speed_class(rate) == 3 ? 2 : 1;
}

/*
 * Payload format, verified on hardware at 48 kHz (`ucxctl stream-in`):
 * no per-packet header, frames interleaved, each sample a little-endian
 * int32 with 24 valid bits at the top (ALSA S32_LE, msbits 24). The low byte
 * is always 0. Slots 0-7 carried analog noise and slots 8-17 were silent
 * (digital inputs, nothing connected).
 *
 * At quad speed each pair carries 6 channels: the Windows driver's channel
 * count per endpoint is 18/14/6 (0x140014f80) and the second pair's first
 * channel is 6 (0x140014d30). So 0x01/0x82 have channels 0-5 and 0x03/0x84
 * channels 6-11, 32 frames per block on each.
 */
#define UCX_SAMPLE_BYTES	4
#define UCX_SAMPLE_BITS		24

/* 72 / 56 / 2 x 24 bytes per frame = 18, 14 and 12 channels */
static inline unsigned int ucx_bytes_per_frame(unsigned int rate)
{
	return ucx_stream_pairs(rate) * ucx_bytes_per_block(rate) /
	       ucx_frames_per_block(rate);
}

static inline unsigned int ucx_channels(unsigned int rate)
{
	return ucx_bytes_per_frame(rate) / UCX_SAMPLE_BYTES;
}

/*
 * Channel status of the S/PDIF coaxial input, in the low byte of that input's
 * capture samples (below the 24 audio bits; measured at 48 kHz with coaxial
 * out looped to in, 2026-11-13). Both S/PDIF channels carry the same byte:
 * BLOCK once every 192 frames, and CS (0x04 and 0x08, always equal in the
 * measurements) = one channel status bit per frame. The frame with BLOCK
 * holds status bit 2, so frame f after it holds bit (f + 2) % 192. Always
 * there: nothing has to be switched on (the Windows driver's "TMS" setting
 * is never sent to the unit). The other low bits read 0.
 */
#define UCX_SPDIF_IN_CHANNEL	8	/* S/PDIF L, in every stream layout */
#define UCX_SPDIF_CS		0x04
#define UCX_SPDIF_CS_BLOCK	0x10
#define UCX_SPDIF_CS_SHIFT	2
#define UCX_SPDIF_CS_BITS	192

/* Bytes per frame on one endpoint: 72 / 56 / 24 */
static inline unsigned int ucx_pair_bytes(unsigned int rate)
{
	return ucx_bytes_per_block(rate) / ucx_frames_per_block(rate);
}

/*
 * REG1 value for UCX_REG1_LARGE_BUFFER: set when the buffer is longer than
 * 256/512/1024 frames, i.e. more than 32 blocks in every speed class.
 */
static inline unsigned int ucx_reg1_large_buffer(unsigned int rate, unsigned int buffer_frames)
{
	return buffer_frames / ucx_frames_per_block(rate) > 32 ? UCX_REG1_LARGE_BUFFER : 0;
}

/*
 * Active rate from a STATUS read, 0 if unknown. Codes as in FireWire's
 * STATUS_CLK_RATE_*; 2 = 48 kHz verified on hardware.
 */
static inline unsigned int ucx_status_rate(unsigned int status)
{
	static const unsigned int rates[16] = {
		32000, 44100, 48000, 0, 64000, 88200, 96000, 0, 128000, 176400, 192000,
	};

	return rates[(status >> 24) & 0xf];
}

/* REG1 rate bits for @rate, see UCX_REG1_RATE_MASK */
static inline unsigned int ucx_reg1_rate(unsigned int rate)
{
	unsigned int base = rate >> (ucx_speed_class(rate) - 1);

	if (base < 38050)
		return 0x00;
	if (base < 46050)
		return 0x10;
	if (base < 49000)
		return 0x20;
	return 0x30;
}

#endif /* UCX_PROTO_H */
