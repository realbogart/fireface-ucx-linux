/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/*
 * The Fireface UCX's ALSA hwdep device, /dev/snd/hwC<card>D0 (audio group,
 * no root needed), shared by the kernel module and user space:
 *
 *  write()  DSP words, host-endian u32 (reg << 16 | value; parity is added),
 *           at most UCX_HWDEP_DSP_MAX_WORDS per call, sent as one transfer.
 *           Words for 0x3e05/0x3e06 (flash store) are refused with EPERM.
 *  read()   The driver's register table as u32 words (reg << 16 | value),
 *           one per register the device has reported or the host has
 *           written. Read it in one call into a UCX_HWDEP_REGS_BYTES buffer
 *           (later reads of the same open file continue, then return 0).
 *  UCX_HWDEP_METERS  One read of each meter bank on endpoint 0x85
 *           (layout: UCX_METER_* in ucx_proto.h).
 *  UCX_HWDEP_STATUS  The unit's four status words, read with vendor requests
 *           0x11, 0x1c, 0x1e and 0x1f (bits: UCX_STATUS_* in ucx_proto.h).
 */
#ifndef UCX_HWDEP_H
#define UCX_HWDEP_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define UCX_HWDEP_DSP_MAX_WORDS	63
#define UCX_HWDEP_REGS_BYTES	(0x8000 * 4)
#define UCX_HWDEP_METER_BANKS	5
#define UCX_HWDEP_METER_BYTES	512

struct ucx_hwdep_meters {
	struct {
		__u32 len;	/* bytes received */
		__u8 data[UCX_HWDEP_METER_BYTES];
	} bank[UCX_HWDEP_METER_BANKS];
};

#define UCX_HWDEP_METERS	_IOR('U', 0x01, struct ucx_hwdep_meters)

/*
 * Not a register of the unit: the driver's record of the EQ+D record setting
 * (1 = input EQ and dynamics are recorded) once it has written it. The unit
 * reports this setting nowhere. In the register table only; writes refused.
 */
#define UCX_HWDEP_REG_EQD_RECORD	0x7f10

/*
 * Likewise for Loopback (vendor request 0x15), which nothing reports either:
 * slot 0x7f20 + output (0 = Line 1 ... 17 = ADAT 8), 1 = the output's signal
 * replaces the matching input in the recording. Present once written.
 */
#define UCX_HWDEP_REG_LOOPBACK		0x7f20

struct ucx_hwdep_status {
	__u32 status;		/* request 0x11: clock, lock and sync state */
	__u32 version;		/* request 0x1c */
	__u32 serial[2];	/* requests 0x1e, 0x1f */
};

#define UCX_HWDEP_STATUS	_IOR('U', 0x02, struct ucx_hwdep_status)

#endif /* UCX_HWDEP_H */
