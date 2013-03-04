/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Systemd integration
 * Systemd provides multi-seat support and other helpers that we can use in
 * uterm.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>
#include "shl_log.h"
#include "uterm_monitor.h"
#include "uterm_systemd_internal.h"

#define LOG_SUBSYSTEM "systemd"

struct uterm_sd {
	sd_login_monitor *mon;
};

int uterm_sd_new(struct uterm_sd **out)
{
	int ret;
	struct uterm_sd *sd;

	if (!out)
		return -EINVAL;

	ret = sd_booted();
	if (ret < 0) {
		log_warning("cannot determine whether system booted with systemd (%d): %s",
			    ret, strerror(-ret));
		return -EOPNOTSUPP;
	} else if (!ret) {
		log_info("system not booted with systemd, disabling multi-seat support");
		return -EOPNOTSUPP;
	}

	log_info("system booted with systemd, enabling multi-seat support");

	sd = malloc(sizeof(*sd));
	if (!sd)
		return -ENOMEM;
	memset(sd, 0, sizeof(*sd));

	ret = sd_login_monitor_new("seat", &sd->mon);
	if (ret) {
		log_err("cannot create systemd login monitor (%d): %s",
			ret, strerror(-ret));
		ret = -EFAULT;
		goto err_free;
	}

	*out = sd;
	return 0;

err_free:
	free(sd);
	return ret;
}

void uterm_sd_free(struct uterm_sd *sd)
{
	if (!sd)
		return;

	sd_login_monitor_unref(sd->mon);
	free(sd);
}

int uterm_sd_get_fd(struct uterm_sd *sd)
{
	if (!sd)
		return -EINVAL;

	return sd_login_monitor_get_fd(sd->mon);
}

void uterm_sd_flush(struct uterm_sd *sd)
{
	if (!sd)
		return;

	sd_login_monitor_flush(sd->mon);
}

int uterm_sd_get_seats(struct uterm_sd *sd, char ***seats)
{
	int ret;
	char **s;

	if (!sd || !seats)
		return -EINVAL;

	ret = sd_get_seats(&s);
	if (ret < 0) {
		log_warning("cannot read seat information from systemd: %d",
			    ret);
		return -EFAULT;
	}

	*seats = s;
	return ret;
}
