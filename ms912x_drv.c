// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>

#include "ms912x.h"

static int ms912x_usb_suspend(struct usb_interface *interface,
			      pm_message_t message)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);

	return drm_mode_config_helper_suspend(&ms912x->drm);
}

static int ms912x_usb_resume(struct usb_interface *interface)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);

	return drm_mode_config_helper_resume(&ms912x->drm);
}

static unsigned int boot_delay_ms = 3000;
module_param(boot_delay_ms, uint, 0644);
MODULE_PARM_DESC(boot_delay_ms,
		 "345f:* integrated panels: ms to wait after power-up before talking to the chip (default 3000, max 10000)");

static bool ms912x_needs_boot_delay(struct usb_interface *interface)
{
	return le16_to_cpu(interface_to_usbdev(interface)->descriptor.idVendor) ==
	       0x345f;
}

/* jiffies of the last recovery reset (module-wide, survives the re-probe) */
static unsigned long ms912x_last_reset;

/*
 * Integrated-panel units boot their own firmware right after power-up.
 * Commands sent in that window leave the panel output dead (LED red) until
 * the next power cycle, so wait until the chip has settled.
 */
static void ms912x_wait_for_boot(struct usb_interface *interface,
				 unsigned long since)
{
	unsigned long delay = msecs_to_jiffies(min(boot_delay_ms, 10000U));
	unsigned long elapsed = jiffies - since;

	if (ms912x_needs_boot_delay(interface) && elapsed < delay)
		msleep(jiffies_to_msecs(delay - elapsed));
}

static int full_range = -1;
module_param(full_range, int, 0444);
MODULE_PARM_DESC(full_range,
		 "YUV range: -1 auto (full for integrated panels, limited otherwise), 0 limited, 1 full");

/*
 * The chip can stop accepting frames (bulk transfers time out) and stays that
 * way until it is re-enumerated. Recover with a USB reset. The driver has no
 * pre/post_reset handlers on purpose: the reset unbinds and re-probes it, so
 * the DRM device is torn down and recreated cleanly and the compositor sees a
 * plain hotplug. (Parking/restoring the display with the suspend/resume
 * helpers is unsafe here: userspace keeps committing during the reset.)
 */
/*
 * Module-wide limits (a reset re-probes the device, so per-device state would
 * not survive it): at most one reset per MS912X_RESET_INTERVAL_MS, and stop
 * after MS912X_MAX_RESETS within MS912X_RESET_WINDOW_MS so a dead chip
 * cannot cause a reset loop.
 */
static DEFINE_SPINLOCK(ms912x_reset_lock);
static unsigned long ms912x_window_start;
static unsigned int ms912x_resets_in_window;

void ms912x_schedule_recovery(struct ms912x_device *ms912x, const char *why)
{
	unsigned long now = jiffies;
	bool do_reset = false;

	/* One request per device at a time; expires in case the reset never ran */
	if (ms912x->recovery_requested &&
	    time_before(now, ms912x->recovery_requested +
				     msecs_to_jiffies(MS912X_RECOVERY_PENDING_MS)))
		return;
	ms912x->recovery_requested = now ? now : 1;

	spin_lock(&ms912x_reset_lock);
	if (!ms912x_window_start ||
	    time_after(now, ms912x_window_start +
				    msecs_to_jiffies(MS912X_RESET_WINDOW_MS))) {
		ms912x_window_start = now;
		ms912x_resets_in_window = 0;
	}
	if ((!ms912x_last_reset ||
	     time_after(now, ms912x_last_reset +
				     msecs_to_jiffies(MS912X_RESET_INTERVAL_MS))) &&
	    ms912x_resets_in_window < MS912X_MAX_RESETS) {
		ms912x_last_reset = now;
		ms912x_resets_in_window++;
		do_reset = true;
	}
	spin_unlock(&ms912x_reset_lock);

	if (!do_reset) {
		drm_err_once(&ms912x->drm,
			     "%s; reset limit reached, replug the display\n", why);
		return;
	}

	drm_warn(&ms912x->drm, "%s, resetting device\n", why);
	usb_queue_reset_device(ms912x->intf);
}

/* The device lost power (or was reset) while suspended: chip rebooted. */
static int ms912x_usb_reset_resume(struct usb_interface *interface)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);

	ms912x_wait_for_boot(interface, jiffies);
	return drm_mode_config_helper_resume(&ms912x->drm);
}

DEFINE_DRM_GEM_FOPS(ms912x_driver_fops);

static struct drm_driver ms912x_drm_driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,

	/* GEM hooks */
	.fops = &ms912x_driver_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};

static const struct drm_mode_config_funcs ms912x_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs
ms912x_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static const struct ms912x_mode ms912x_mode_list[] = {
	/* Found in captures of the Windows driver and dumped from device */
	MS912X_MODE(720, 480, 60, 0x02), /* 60.00 Hz */
	MS912X_MODE(720, 576, 50, 0x11), /* 50.00 Hz */
	MS912X_MODE(1280,  720, 50, 0x13), /* 50.00 Hz */
	MS912X_MODE(1920, 1080, 50, 0x1f), /* 50.00 Hz */
	MS912X_MODE(1920, 1080, 30, 0x22), /* 30.00 Hz */
	MS912X_MODE(640, 480, 60, 0x40), /* 59.94 Hz */
	MS912X_MODE(800, 600, 60, 0x42), /* 60.32 Hz */
	MS912X_MODE(800, 600, 75, 0x44), /* 75.00 Hz */
	MS912X_MODE(1024,  768, 60, 0x47), /* 60.00 Hz */
	MS912X_MODE(1024,  768, 75, 0x49), /* 75.03 Hz */
	MS912X_MODE(1152,  864, 60, 0x4c), /* 60.00 Hz */
	MS912X_MODE(1280,  600, 60, 0x4e), /* 60.00 Hz */
	MS912X_MODE(1280,  720, 60, 0x4f), /* 60.00 Hz */
	MS912X_MODE(1280,  768, 60, 0x54), /* 59.87 Hz */
	MS912X_MODE(1280,  768, 75, 0x56), /* 74.89 Hz */
	MS912X_MODE(1280,  800, 60, 0x57), /* 59.81 Hz */
	MS912X_MODE(1280,  960, 60, 0x5b), /* 60.00 Hz */
	MS912X_MODE(1280, 1024, 60, 0x60), /* 60.02 Hz */
	MS912X_MODE(1280, 1024, 75, 0x61), /* 75.02 Hz */
	MS912X_MODE(1360,  768, 60, 0x64), /* 60.02 Hz */
	MS912X_MODE(1366,  768, 60, 0x66), /* 59.79 Hz */
	MS912X_MODE(1400, 1050, 60, 0x67), /* 59.98 Hz */
	MS912X_MODE(1440,  900, 60, 0x6b), /* 59.89 Hz */
	MS912X_MODE(1600, 1200, 60, 0x73), /* 60.00 Hz */
	MS912X_MODE(1680, 1050, 60, 0x78), /* 59.95 Hz */
	MS912X_MODE(1920, 1080, 60, 0x81), /* 60.00 Hz */
};

static const struct ms912x_mode *
ms912x_get_mode(struct ms912x_device *ms912x,
		const struct drm_display_mode *mode)
{
	unsigned int i;
	int width = mode->hdisplay;
	int height = mode->vdisplay;
	int hz = drm_mode_vrefresh(mode);

	for (i = 0; i < ms912x->num_custom_modes; i++) {
		const struct ms912x_mode *custom_mode =
			&ms912x->custom_modes[i].mode;

		if (custom_mode->width == width &&
		    custom_mode->height == height &&
		    custom_mode->hz == hz)
			return custom_mode;
	}

	if (ms912x->panel_only)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(ms912x_mode_list); i++) {
		if (ms912x_mode_list[i].width == width &&
		    ms912x_mode_list[i].height == height &&
		    ms912x_mode_list[i].hz == hz) {
			return &ms912x_mode_list[i];
		}
	}
	return NULL;
}

static void ms912x_crtc_atomic_enable(struct drm_crtc *crtc,
				      struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_device *dev = crtc->dev;
	struct ms912x_device *ms912x = to_ms912x(dev);
	const struct ms912x_mode *ms_mode;
	int ret;

	ret = ms912x_power_on(ms912x);
	if (ret) {
		drm_err(dev, "failed to power on display: %d\n", ret);
		ms912x_schedule_recovery(ms912x, "power on failed");
		return;
	}

	ms_mode = ms912x_get_mode(ms912x, &crtc_state->mode);
	if (!ms_mode) {
		drm_err(dev, "unsupported mode passed to CRTC enable\n");
		return;
	}

	ret = ms912x_set_resolution(ms912x, ms_mode);
	if (ret) {
		drm_err(dev, "failed to set display mode: %d\n", ret);
		ms912x_schedule_recovery(ms912x, "mode set failed");
	}
}

static void ms912x_cancel_transfer_work(struct ms912x_device *ms912x)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ms912x->requests); i++) {
		struct ms912x_usb_request *request = &ms912x->requests[i];

		if (cancel_work_sync(&request->work))
			complete(&request->done);
	}
}

static void ms912x_crtc_atomic_disable(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct drm_device *dev = crtc->dev;
	struct ms912x_device *ms912x = to_ms912x(dev);
	int ret;

	ms912x_cancel_transfer_work(ms912x);
	ret = ms912x_power_off(ms912x);
	if (ret && ret != -ENODEV)
		drm_err(dev, "failed to power off display: %d\n", ret);
}

static enum drm_mode_status
ms912x_crtc_mode_valid(struct drm_crtc *crtc,
		       const struct drm_display_mode *mode)
{
	struct ms912x_device *ms912x = to_ms912x(crtc->dev);
	const struct ms912x_mode *ms_mode = ms912x_get_mode(ms912x, mode);

	if (!ms_mode)
		return MODE_BAD;

	return MODE_OK;
}

static int ms912x_plane_atomic_check(struct drm_plane *plane,
				     struct drm_atomic_commit *state)
{
	struct drm_plane_state *old_plane_state;
	struct drm_plane_state *new_plane_state;
	struct drm_crtc_state *crtc_state = NULL;

	old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (new_plane_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state,
							   new_plane_state->crtc);

	/*
	 * A mode set does change how the device interprets the framebuffer
	 * so ask for a full plane update for the first frame after one.
	 */
	new_plane_state->ignore_damage_clips =
		old_plane_state->fb != new_plane_state->fb ||
		(crtc_state && drm_atomic_crtc_needs_modeset(crtc_state));

	return drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static void ms912x_clear_rect(struct drm_rect *rect)
{
	rect->x1 = INT_MAX;
	rect->y1 = INT_MAX;
	rect->x2 = 0;
	rect->y2 = 0;
}

static void ms912x_merge_rects(struct drm_rect *dest, struct drm_rect *r1,
			       struct drm_rect *r2)
{
	dest->x1 = min(r1->x1, r2->x1);
	dest->y1 = min(r1->y1, r2->y1);
	dest->x2 = max(r1->x2, r2->x2);
	dest->y2 = max(r1->y2, r2->y2);
}

static void ms912x_plane_atomic_update(struct drm_plane *plane,
				       struct drm_atomic_commit *state)
{
	struct drm_plane_state *old_plane_state;
	struct drm_plane_state *new_plane_state;
	struct drm_crtc_state *new_crtc_state;
	struct drm_shadow_plane_state *shadow_plane_state;
	struct ms912x_device *ms912x = to_ms912x(plane->dev);
	struct drm_rect current_rect, rect;

	old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	new_plane_state = drm_atomic_get_new_plane_state(state, plane);

	if (!new_plane_state->fb)
		return;
	new_crtc_state = drm_atomic_get_new_crtc_state(state,
						       new_plane_state->crtc);
	shadow_plane_state = to_drm_shadow_plane_state(new_plane_state);

	/* Damage from the previous mode may not fit the new framebuffer. */
	if (drm_atomic_crtc_needs_modeset(new_crtc_state))
		ms912x_clear_rect(&ms912x->update_rect);

	if (drm_atomic_helper_damage_merged(old_plane_state, new_plane_state,
					    &current_rect)) {
		/*
		 * The device double buffers, so we need to send the update
		 * rects of the last two frames.
		 */
		ms912x_merge_rects(&rect, &current_rect, &ms912x->update_rect);
		if (ms912x_fb_send_rect(new_plane_state->fb,
					&shadow_plane_state->data[0],
					&shadow_plane_state->fmtcnv_state,
					&rect)) {
			/* In case of error, merge the rects to update later */
			ms912x_merge_rects(&ms912x->update_rect,
					   &ms912x->update_rect, &rect);
		} else {
			ms912x->update_rect = current_rect;
		}
	}
}

static const struct drm_crtc_helper_funcs ms912x_crtc_helper_funcs = {
	.mode_valid = ms912x_crtc_mode_valid,
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_enable = ms912x_crtc_atomic_enable,
	.atomic_disable = ms912x_crtc_atomic_disable,
};

static const struct drm_crtc_funcs ms912x_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_plane_helper_funcs ms912x_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = ms912x_plane_atomic_check,
	.atomic_update = ms912x_plane_atomic_update,
};

static const struct drm_plane_funcs ms912x_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const u32 ms912x_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u8 ms912x_bulk_out_endpoints[] = {
	MS912X_BULK_OUT_ENDPOINT, 0
};

/*
 * Wayland compositors built on aquamarine (Hyprland) only accept render-less
 * KMS devices when the DRM driver is DisplayLink's "evdi"; any other name makes
 * them try to create an EGL renderer on the device, fail, and never light the
 * output. Opt-in: report "evdi" so they use their display-only path instead.
 */
static bool aq_evdi_compat;
module_param(aq_evdi_compat, bool, 0444);
MODULE_PARM_DESC(aq_evdi_compat,
		 "Report DRM driver name \"evdi\" so aquamarine/Hyprland drive the display (default off)");

static int ms912x_usb_probe(struct usb_interface *interface,
			    const struct usb_device_id *id)
{
	int ret;
	unsigned int i;
	struct ms912x_device *ms912x;
	struct drm_device *dev;
	struct device *dma_dev;
	struct usb_device *usbdev;
	bool is_panel;
	int port;

	if (aq_evdi_compat)
		ms912x_drm_driver.name = "evdi";

	ms912x = devm_drm_dev_alloc(&interface->dev, &ms912x_drm_driver,
				    struct ms912x_device, drm);
	if (IS_ERR(ms912x))
		return PTR_ERR(ms912x);

	ms912x->intf = interface;
	ret = devm_mutex_init(&interface->dev, &ms912x->ctrl_lock);
	if (ret)
		return ret;

	if (!usb_check_bulk_endpoints(interface, ms912x_bulk_out_endpoints))
		return -ENXIO;

	{
		/* A recovery reset may reboot the chip too: wait from whichever is later */
		unsigned long since = interface_to_usbdev(interface)->connect_time;
		unsigned long reset = READ_ONCE(ms912x_last_reset);

		if (reset && time_after(reset, since))
			since = reset;
		ms912x_wait_for_boot(interface, since);
	}

	for (i = 0; i < 3; i++) {
		ret = ms912x_read_custom_timing(ms912x);
		if (ret != -EPROTO && ret != -ETIMEDOUT && ret != -EPIPE)
			break;
		msleep(200);
	}
	if (ret < 0) {
		drm_warn(&ms912x->drm,
			 "failed to read custom timing: %d\n", ret);
	}

	for (i = 0; i < 3; i++) {
		port = ms912x_read_byte(ms912x, MS912X_REG_VIDEO_PORT);
		if (port >= 0 || port == -ENODEV)
			break;
		msleep(200);
	}
	if (port < 0)
		return port;
	is_panel = port == MS912X_VIDEO_PORT_DIGITAL;
	if (is_panel) {
		ret = ms912x_add_panel_fallback_timing(ms912x);
		if (ret < 0)
			drm_warn(&ms912x->drm,
				 "failed to add panel fallback timing: %d\n", ret);
		ms912x->panel_only = ms912x->num_custom_modes > 0;
	}
	ms912x->full_range = full_range < 0 ? is_panel : full_range != 0;
	for (i = 0; i < ms912x->num_custom_modes; i++) {
		const struct ms912x_mode *mode = &ms912x->custom_modes[i].mode;

		drm_info(&ms912x->drm,
			 "custom mode %dx%d@%d uses mode 0x%02x\n",
			 mode->width, mode->height, mode->hz, mode->mode);
	}

	usbdev = interface_to_usbdev(interface);
	ms912x->bulk_pipe =
		usb_sndbulkpipe(usbdev, MS912X_BULK_OUT_ENDPOINT);
	dev = &ms912x->drm;

	dma_dev = usb_intf_get_dma_device(interface);
	if (dma_dev) {
		drm_dev_set_dma_dev(dev, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(dev,
			 "buffer sharing not supported\n"); /* not an error */
	}
	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = 0;
	dev->mode_config.max_width = MS912X_MAX_WIDTH;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_height = MS912X_MAX_HEIGHT;
	dev->mode_config.funcs = &ms912x_mode_config_funcs;
	dev->mode_config.helper_private = &ms912x_mode_config_helper_funcs;

	/*
	 * This stops weird behavior in the device. Integrated panels cannot
	 * display 720x480 and never recover from it, so skip it for them.
	 */
	if (!is_panel) {
		ret = ms912x_set_resolution(ms912x, &ms912x_mode_list[0]);
		if (ret)
			return ret;
	}

	ms912x->workqueue =
		drmm_alloc_ordered_workqueue(dev, DRIVER_NAME, 0);
	if (IS_ERR(ms912x->workqueue))
		return PTR_ERR(ms912x->workqueue);

	ret = ms912x_init_request(ms912x, &ms912x->requests[0],
				  MS912X_MAX_TRANSFER_LEN);
	if (ret)
		return ret;

	ret = ms912x_init_request(ms912x, &ms912x->requests[1],
				  MS912X_MAX_TRANSFER_LEN);
	if (ret)
		goto err_free_request_0;

	ret = drm_universal_plane_init(dev, &ms912x->plane, 0,
				       &ms912x_plane_funcs,
				       ms912x_plane_formats,
				       ARRAY_SIZE(ms912x_plane_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		goto err_free_request_1;

	drm_plane_helper_add(&ms912x->plane, &ms912x_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&ms912x->plane);
	ms912x_clear_rect(&ms912x->update_rect);

	ret = drm_crtc_init_with_planes(dev, &ms912x->crtc, &ms912x->plane,
					NULL, &ms912x_crtc_funcs, NULL);
	if (ret)
		goto err_free_request_1;

	drm_crtc_helper_add(&ms912x->crtc, &ms912x_crtc_helper_funcs);

	ret = ms912x_connector_init(ms912x);
	if (ret)
		goto err_free_request_1;

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, ms912x);

	drmm_kms_helper_poll_init(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_free_request_1;

	drm_client_setup(dev, NULL);

	return 0;

err_free_request_1:
	ms912x_free_request(&ms912x->requests[1]);
err_free_request_0:
	ms912x_free_request(&ms912x->requests[0]);
	return ret;
}

static void ms912x_usb_disconnect(struct usb_interface *interface)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);
	struct drm_device *dev = &ms912x->drm;

	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	ms912x_cancel_transfer_work(ms912x);
	ms912x_free_request(&ms912x->requests[0]);
	ms912x_free_request(&ms912x->requests[1]);
}

static void ms912x_usb_shutdown(struct usb_interface *interface)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);

	drm_atomic_helper_shutdown(&ms912x->drm);
}

static const struct usb_device_id id_table[] = {
	/* USB 2 */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x534d, 0x6021, 0xff, 0x00, 0x00) },
	/* USB 2 Sometimes this PID will pop up */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x534d, 0x0821, 0xff, 0x00, 0x00) },
	/* USB 3 */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x345f, 0x9132, 0xff, 0x00, 0x00) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x345f, 0x9133, 0xff, 0x00, 0x00) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x345f, 0x9135, 0xff, 0x00, 0x00) },
	{ }
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver ms912x_driver = {
	.name = DRIVER_NAME,
	.probe = ms912x_usb_probe,
	.disconnect = ms912x_usb_disconnect,
	.suspend = ms912x_usb_suspend,
	.resume = ms912x_usb_resume,
	.reset_resume = ms912x_usb_reset_resume,
	.shutdown = ms912x_usb_shutdown,
	.id_table = id_table,
};
module_usb_driver(ms912x_driver);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
