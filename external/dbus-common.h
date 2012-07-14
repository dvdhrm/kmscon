/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>

#define new0(_x, _y) zero_alloc(sizeof(_x) * (_y))

static void *zero_alloc(size_t s)
{
	void *mem = malloc(s);
	if (mem)
		memset(mem, 0, s);
	return mem;
}

#define zero(_x) memset(&(_x), 0, sizeof(_x))
#define PTR_TO_INT(_x) ((int) ((intptr_t) (_x)))
#define INT_TO_PTR(u) ((void*) ((intptr_t) (u)))
#define close_nointr_nofail(_x) close(_x)
#define assert_se(_x) (void)(_x)
#define USEC_PER_MSEC 1000ULL
#define USEC_PER_SEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#define log_error(...)

static struct timespec *timespec_store(struct timespec *ts, int64_t u)  {
        assert(ts);
        ts->tv_sec = (time_t) (u / USEC_PER_SEC);
        ts->tv_nsec = (long int) ((u % USEC_PER_SEC) * NSEC_PER_USEC);
        return ts;
}

uint32_t bus_flags_to_events(DBusWatch *bus_watch) {
        unsigned flags;
        uint32_t events = 0;

        assert(bus_watch);

        /* no watch flags for disabled watches */
        if (!dbus_watch_get_enabled(bus_watch))
                return 0;

        flags = dbus_watch_get_flags(bus_watch);

        if (flags & DBUS_WATCH_READABLE)
                events |= EPOLLIN;
        if (flags & DBUS_WATCH_WRITABLE)
                events |= EPOLLOUT;

        return events | EPOLLHUP | EPOLLERR;
}

unsigned bus_events_to_flags(uint32_t events) {
        unsigned flags = 0;

        if (events & EPOLLIN)
                flags |= DBUS_WATCH_READABLE;
        if (events & EPOLLOUT)
                flags |= DBUS_WATCH_WRITABLE;
        if (events & EPOLLHUP)
                flags |= DBUS_WATCH_HANGUP;
        if (events & EPOLLERR)
                flags |= DBUS_WATCH_ERROR;

        return flags;
}
