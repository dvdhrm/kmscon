/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * DRM shared functions
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "log.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "drm_shared"

int uterm_drm_mode_init(struct uterm_mode *mode)
{
	struct uterm_drm_mode *m;

	m = malloc(sizeof(*m));
	if (!m)
		return -ENOMEM;
	memset(m, 0, sizeof(*m));
	mode->data = m;

	return 0;
}

void uterm_drm_mode_destroy(struct uterm_mode *mode)
{
	free(mode->data);
}

const char *uterm_drm_mode_get_name(const struct uterm_mode *mode)
{
	struct uterm_drm_mode *m = mode->data;

	return m->info.name;
}

unsigned int uterm_drm_mode_get_width(const struct uterm_mode *mode)
{
	struct uterm_drm_mode *m = mode->data;

	return m->info.hdisplay;
}

unsigned int uterm_drm_mode_get_height(const struct uterm_mode *mode)
{
	struct uterm_drm_mode *m = mode->data;

	return m->info.vdisplay;
}

void uterm_drm_mode_set(struct uterm_mode *mode, drmModeModeInfo *info)
{
	struct uterm_drm_mode *m = mode->data;

	m->info = *info;
}

const struct mode_ops uterm_drm_mode_ops = {
	.init = uterm_drm_mode_init,
	.destroy = uterm_drm_mode_destroy,
	.get_name = uterm_drm_mode_get_name,
	.get_width = uterm_drm_mode_get_width,
	.get_height = uterm_drm_mode_get_height,
};

int uterm_drm_set_dpms(int fd, uint32_t conn_id, int state)
{
	int i, ret, set;
	drmModeConnector *conn;
	drmModePropertyRes *prop;

	switch (state) {
	case UTERM_DPMS_ON:
		set = DRM_MODE_DPMS_ON;
		break;
	case UTERM_DPMS_STANDBY:
		set = DRM_MODE_DPMS_STANDBY;
		break;
	case UTERM_DPMS_SUSPEND:
		set = DRM_MODE_DPMS_SUSPEND;
		break;
	case UTERM_DPMS_OFF:
		set = DRM_MODE_DPMS_OFF;
		break;
	default:
		return -EINVAL;
	}

	conn = drmModeGetConnector(fd, conn_id);
	if (!conn) {
		log_err("cannot get display connector");
		return -EFAULT;
	}

	ret = state;
	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

		if (!strcmp(prop->name, "DPMS")) {
			ret = drmModeConnectorSetProperty(fd, conn_id,
							  prop->prop_id, set);
			if (ret) {
				log_info("cannot set DPMS");
				ret = -EFAULT;
			}
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props) {
		log_warn("display does not support DPMS");
		ret = UTERM_DPMS_UNKNOWN;
	}

	drmModeFreeConnector(conn);
	return ret;
}

int uterm_drm_get_dpms(int fd, drmModeConnector *conn)
{
	int i, ret;
	drmModePropertyRes *prop;

	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

		if (!strcmp(prop->name, "DPMS")) {
			switch (conn->prop_values[i]) {
			case DRM_MODE_DPMS_ON:
				ret = UTERM_DPMS_ON;
				break;
			case DRM_MODE_DPMS_STANDBY:
				ret = UTERM_DPMS_STANDBY;
				break;
			case DRM_MODE_DPMS_SUSPEND:
				ret = UTERM_DPMS_SUSPEND;
				break;
			case DRM_MODE_DPMS_OFF:
			default:
				ret = UTERM_DPMS_OFF;
			}

			drmModeFreeProperty(prop);
			return ret;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props)
		log_warn("display does not support DPMS");
	return UTERM_DPMS_UNKNOWN;
}
