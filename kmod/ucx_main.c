// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * ALSA driver for the RME Fireface UCX in RME driver mode (USB PID 0x3fc9).
 * In class-compliant mode (PID 0x3fb9) the device belongs to snd-usb-audio.
 *
 * Binds to interface 5 (audio) and claims interfaces 1 (DSP commands),
 * 2 (MIDI) and 4 (device events). Provides one duplex PCM at 32-192 kHz
 * (18, 14 or 12 channels by speed class), mixer controls for the DSP, and
 * the two MIDI ports.
 * See include/ucx_proto.h and docs/PROTOCOL.md for the protocol.
 */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <sound/core.h>
#include <sound/initval.h>

#include "ucx.h"

/* Endpoints the Windows driver uses for model 3 */
static const u8 ucx_expected_eps[] = {
	UCX_EP_PCM_OUT, UCX_EP_PCM_IN, UCX_EP_PCM_OUT2, UCX_EP_PCM_IN2,
	UCX_EP_CTRL_OUT, UCX_EP_CTRL_IN, UCX_EP_MIDI_OUT, UCX_EP_MIDI_IN,
	UCX_EP_STATUS_IN,
};

/* Mirrors 0x140014300 with URB_FUNCTION_VENDOR_DEVICE and no data stage */
int ucx_vendor_write(struct ucx *ucx, u8 request, u16 value, u16 index)
{
	int err = usb_control_msg(ucx->udev, usb_sndctrlpipe(ucx->udev, 0), request,
				  USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				  value, index, NULL, 0, USB_CTRL_SET_TIMEOUT);

	return err < 0 ? err : 0;
}

/* 4-byte vendor IN request, like the status reads 0x14000b110 and 0x14000afc0 */
int ucx_vendor_read32(struct ucx *ucx, u8 request, u32 *value)
{
	__le32 *buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	int err;

	if (!buf)
		return -ENOMEM;
	err = usb_control_msg(ucx->udev, usb_rcvctrlpipe(ucx->udev, 0), request,
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      0, 0, buf, sizeof(*buf), USB_CTRL_GET_TIMEOUT);
	if (err == sizeof(*buf))
		*value = le32_to_cpu(*buf);
	kfree(buf);
	if (err < 0)
		return err;
	return err == sizeof(*buf) ? 0 : -EIO;
}

static const char *ucx_xfer_type_name(const struct usb_endpoint_descriptor *ep)
{
	switch (usb_endpoint_type(ep)) {
	case USB_ENDPOINT_XFER_BULK:
		return "bulk";
	case USB_ENDPOINT_XFER_INT:
		return "interrupt";
	case USB_ENDPOINT_XFER_ISOC:
		return "isoc";
	default:
		return "control";
	}
}

/*
 * Log the whole configuration and warn about missing endpoints, in case a
 * firmware version lays the interfaces out differently.
 */
static void ucx_check_endpoints(struct ucx *ucx)
{
	struct usb_host_config *cfg = ucx->udev->actconfig;
	bool seen[ARRAY_SIZE(ucx_expected_eps)] = {};
	unsigned int i, a, e, k;

	for (i = 0; i < cfg->desc.bNumInterfaces; i++) {
		struct usb_interface *intf = cfg->interface[i];

		for (a = 0; a < intf->num_altsetting; a++) {
			struct usb_host_interface *alt = &intf->altsetting[a];

			for (e = 0; e < alt->desc.bNumEndpoints; e++) {
				struct usb_endpoint_descriptor *ep = &alt->endpoint[e].desc;

				dev_dbg(&ucx->intf->dev, "intf %u alt %u ep 0x%02x %s maxpacket %u\n",
					alt->desc.bInterfaceNumber, alt->desc.bAlternateSetting,
					ep->bEndpointAddress, ucx_xfer_type_name(ep),
					usb_endpoint_maxp(ep));
				for (k = 0; k < ARRAY_SIZE(ucx_expected_eps); k++)
					if (ep->bEndpointAddress == ucx_expected_eps[k])
						seen[k] = true;
			}
		}
	}

	for (k = 0; k < ARRAY_SIZE(ucx_expected_eps); k++)
		if (!seen[k])
			dev_warn(&ucx->intf->dev, "expected endpoint 0x%02x not found\n",
				 ucx_expected_eps[k]);
}

static int ucx_claim_extra(struct ucx *ucx, unsigned int ifnum, struct usb_interface **slot)
{
	struct usb_interface *intf = usb_ifnum_to_if(ucx->udev, ifnum);
	int err;

	if (!intf)
		return -ENODEV;
	err = usb_driver_claim_interface(&ucx_driver, intf, ucx);
	if (!err)
		*slot = intf;
	return err;
}

/*
 * Release the claimed extra interfaces, except @current_intf, whose disconnect is
 * already running. Releasing calls ucx_disconnect() for each, which then
 * finds no driver data and returns.
 */
static void ucx_release_extra(struct ucx *ucx, struct usb_interface *current_intf)
{
	struct usb_interface **extra[] = { &ucx->aux_intf, &ucx->status_intf, &ucx->midi_intf };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(extra); i++) {
		if (!*extra[i])
			continue;
		usb_set_intfdata(*extra[i], NULL);
		if (*extra[i] != current_intf)
			usb_driver_release_interface(&ucx_driver, *extra[i]);
		*extra[i] = NULL;
	}
}

/*
 * Interface 2 is a standard USB MIDI interface with two ports, but
 * snd-usb-audio doesn't bind this device, so hand it to the same library.
 * The Windows driver turns MIDI on in REG1 before it reads (0x14000b8c0).
 * MIDI is optional: on failure the card works without it.
 */
static void ucx_midi_init(struct ucx *ucx)
{
	int err;

	err = ucx_claim_extra(ucx, UCX_INTF_MIDI, &ucx->midi_intf);
	if (!err)
		err = ucx_vendor_write(ucx, UCX_REQ_REG1, UCX_REG1_MIDI, UCX_REG1_MIDI);
	if (!err)
		err = __snd_usbmidi_create(ucx->card, ucx->midi_intf, &ucx->midi_list, NULL, 0,
					   NULL);
	if (err)
		dev_warn(&ucx->intf->dev, "no MIDI ports: %d\n", err);
}

/*
 * snd_usbmidi_disconnect() takes one device's entry in the list, not the
 * list head (snd-usb-audio's disconnect walks its list the same way).
 */
static void ucx_midi_disconnect(struct ucx *ucx)
{
	struct list_head *p;

	list_for_each(p, &ucx->midi_list)
		snd_usbmidi_disconnect(p);
}

static const struct file_operations ucx_pm_fops;

static int ucx_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct snd_card *card;
	struct ucx *ucx;
	char path[32];
	int err;

	/* One card per device, owned by the audio interface */
	if (intf->cur_altsetting->desc.bInterfaceNumber != UCX_INTF_PCM)
		return -ENODEV;

	err = snd_card_new(&intf->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			   THIS_MODULE, sizeof(*ucx), &card);
	if (err < 0)
		return err;

	ucx = card->private_data;
	ucx->udev = udev;
	ucx->intf = intf;
	ucx->card = card;
	mutex_init(&ucx->lock);
	spin_lock_init(&ucx->stream_lock);
	mutex_init(&ucx->dsp_lock);
	spin_lock_init(&ucx->regs_lock);
	INIT_DELAYED_WORK(&ucx->poll_work, ucx_poll_work);
	INIT_DELAYED_WORK(&ucx->recover_work, ucx_recover_work);
	INIT_WORK(&ucx->stream_err_work, ucx_stream_err_work);
	INIT_LIST_HEAD(&ucx->midi_list);

	usb_make_path(udev, path, sizeof(path));
	strscpy(card->driver, "FirefaceUCX", sizeof(card->driver));
	strscpy(card->shortname, "Fireface UCX", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "RME Fireface UCX %s at %s",
		 udev->serial ? udev->serial : "", path);

	ucx_check_endpoints(ucx);

	/* Interface 1: DSP commands and meters. Interface 4: device events. */
	err = ucx_claim_extra(ucx, UCX_INTF_AUX, &ucx->aux_intf);
	if (!err)
		err = ucx_claim_extra(ucx, UCX_INTF_STATUS, &ucx->status_intf);
	if (err)
		goto err_release;

	err = ucx_pcm_init(ucx);
	if (!err)
		err = ucx_mixer_init(ucx);
	if (err < 0)
		goto err_teardown;
	ucx_midi_init(ucx);
	debugfs_create_file("pm", 0200, ucx->debugfs, ucx, &ucx_pm_fops);
	err = snd_card_register(card);
	if (err < 0)
		goto err_teardown;
	usb_set_intfdata(intf, ucx);
	return 0;

err_teardown:
	ucx_midi_disconnect(ucx);
	ucx_mixer_disconnect(ucx);
	ucx_pcm_disconnect(ucx);
err_release:
	ucx_release_extra(ucx, NULL);
	snd_card_free(card);
	return err;
}

/* Called for every claimed interface, in any order: tear down on the first */
static void ucx_disconnect(struct usb_interface *intf)
{
	struct ucx *ucx = usb_get_intfdata(intf);

	if (!ucx)
		return;
	usb_set_intfdata(ucx->intf, NULL);

	snd_card_disconnect(ucx->card);
	ucx_midi_disconnect(ucx);
	ucx_mixer_disconnect(ucx);
	ucx_pcm_disconnect(ucx);
	ucx_release_extra(ucx, intf);
	snd_card_free_when_closed(ucx->card);
}

/*
 * System sleep. Without these the USB core unbinds the driver at suspend
 * and probes it again at resume (usb_suspend() in drivers/usb/core/driver.c),
 * so the card would disappear for every sleep. The core calls them once for
 * each claimed interface (3 or 4) under the device lock; like snd-usb-audio,
 * the work is done at the first suspend and the last resume. The driver
 * doesn't support runtime autosuspend (no supports_autosuspend), so these
 * only run for system sleep and hibernation.
 */
bool ucx_pm_debug;
module_param_named(pm_debug, ucx_pm_debug, bool, 0644);
MODULE_PARM_DESC(pm_debug, "Log each step of suspend and resume");

/* Called with the USB device locked, by the PM core or ucx_pm_write() */
static void ucx_pm_suspend(struct ucx *ucx)
{
	struct list_head *p;

	if (ucx->pm_suspended)
		return;
	ucx_pm_log(ucx, "suspend");
	ucx_pcm_suspend(ucx);
	ucx_pm_log(ucx, "suspend: MIDI");
	list_for_each(p, &ucx->midi_list)
		snd_usbmidi_suspend(p);
	ucx_mixer_suspend(ucx);
	snd_power_change_state(ucx->card, SNDRV_CTL_POWER_D3hot);
	ucx->pm_suspended = true;
	ucx_pm_log(ucx, "suspend: done");
}

static void ucx_pm_resume(struct ucx *ucx, bool reset)
{
	struct list_head *p;
	int err;

	if (!ucx->pm_suspended)
		return;
	ucx_pm_log(ucx, "resume%s", reset ? " after reset" : "");
	/* After a reset REG1 is back to its default: turn MIDI on as at probe */
	if (reset && ucx->midi_intf) {
		err = ucx_vendor_write(ucx, UCX_REQ_REG1, UCX_REG1_MIDI, UCX_REG1_MIDI);
		if (err)
			dev_warn(&ucx->intf->dev, "turning MIDI on after reset: %d\n", err);
	}
	ucx_mixer_resume(ucx);
	ucx_pm_log(ucx, "resume: MIDI");
	list_for_each(p, &ucx->midi_list)
		snd_usbmidi_resume(p);
	snd_power_change_state(ucx->card, SNDRV_CTL_POWER_D0);
	ucx->pm_suspended = false;
	ucx_pm_log(ucx, "resume: done");
}

static int ucx_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct ucx *ucx = usb_get_intfdata(intf);

	if (!ucx)
		return 0;
	ucx_pm_log(ucx, "suspend interface %u (event %#x, count %u)",
		   intf->cur_altsetting->desc.bInterfaceNumber, message.event, ucx->pm_count);
	if (!ucx->pm_count++)
		ucx_pm_suspend(ucx);
	return 0;
}

static int ucx_resume_intf(struct usb_interface *intf, bool reset)
{
	struct ucx *ucx = usb_get_intfdata(intf);

	if (!ucx)
		return 0;
	ucx_pm_log(ucx, "resume interface %u (reset %d, count %u)",
		   intf->cur_altsetting->desc.bInterfaceNumber, reset, ucx->pm_count);
	ucx->pm_reset |= reset;
	if (WARN_ON(!ucx->pm_count) || --ucx->pm_count)
		return 0;
	reset = ucx->pm_reset;
	ucx->pm_reset = false;
	ucx_pm_resume(ucx, reset);
	return 0;
}

/*
 * debugfs "pm" (root): write "suspend", "resume" or "reset-resume" to run
 * the driver's side of system sleep on a live system, with the USB device
 * locked as the PM core has it. Nothing is powered down and the USB core
 * does nothing, so a hang here stays in the writing process (and the hung
 * task detector reports where) instead of stopping the machine.
 */
static ssize_t ucx_pm_write(struct file *file, const char __user *ubuf, size_t len,
			    loff_t *ppos)
{
	struct ucx *ucx = file->private_data;
	char buf[16];
	int err = 0;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	strim(buf);

	/* Not a blocking lock: disconnect holds it while waiting for this write */
	if (!usb_trylock_device(ucx->udev))
		return -EBUSY;
	if (ucx->disconnected || ucx->pm_count)
		err = -EBUSY;
	else if (!strcmp(buf, "suspend"))
		ucx_pm_suspend(ucx);
	else if (!strcmp(buf, "resume"))
		ucx_pm_resume(ucx, false);
	else if (!strcmp(buf, "reset-resume"))
		ucx_pm_resume(ucx, true);
	else
		err = -EINVAL;
	usb_unlock_device(ucx->udev);
	return err ? err : len;
}

static const struct file_operations ucx_pm_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ucx_pm_write,
};

static int ucx_resume(struct usb_interface *intf)
{
	return ucx_resume_intf(intf, false);
}

/* The device was reset while asleep (every S3 wake here, and hibernation) */
static int ucx_reset_resume(struct usb_interface *intf)
{
	return ucx_resume_intf(intf, true);
}

static const struct usb_device_id ucx_ids[] = {
	{ USB_DEVICE(UCX_VID_RME, UCX_PID_RME_MODE) },
	{ USB_DEVICE(UCX_VID_MICROCHIP, UCX_PID_RME_MODE) },
	{ }
};
MODULE_DEVICE_TABLE(usb, ucx_ids);

struct usb_driver ucx_driver = {
	.name		= "snd-fireface-ucx",
	.id_table	= ucx_ids,
	.probe		= ucx_probe,
	.disconnect	= ucx_disconnect,
	.suspend	= ucx_suspend,
	.resume		= ucx_resume,
	.reset_resume	= ucx_reset_resume,
};
module_usb_driver(ucx_driver);

MODULE_DESCRIPTION("RME Fireface UCX (RME driver mode)");
MODULE_LICENSE("Dual MIT/GPL");
