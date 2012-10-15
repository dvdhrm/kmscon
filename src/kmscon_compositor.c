/*
 * kmscon - Compositor
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
 * Compositor
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "eloop.h"
#include "kmscon_compositor.h"
#include "kmscon_seat.h"
#include "log.h"
#include "shl_dlist.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "compositor"

struct compositor {
	struct kmscon_seat *seat;
	struct ev_eloop *eloop;
	struct kmscon_session *session;

	struct wl_display *w_display;
	struct wl_event_loop *w_eloop;
	struct ev_fd *eloop_fd;

	struct shl_dlist outputs;
	struct shl_dlist surfaces;

	struct shell_surface *active_shell_surface;
};

struct output {
	struct shl_dlist list;
	struct compositor *comp;
	struct uterm_display *disp;
	struct uterm_mode *mode;
	bool redraw;
	bool pending_page_flip;
};

struct surface {
	struct shl_dlist list;
	struct wl_surface w_surface;
	struct compositor *comp;
	struct shell_surface *shell_surface;
	struct shl_dlist frame_cbs;
	struct wl_buffer *w_buffer;
	struct wl_listener w_buffer_destroy_listener;
};

enum shell_surface_type {
	SHELL_SURFACE_UNUSED,
	SHELL_SURFACE_TOPLEVEL,
};

struct shell_surface {
	struct wl_resource w_resource;
	struct surface *surface;
	struct wl_listener w_surface_destroy_listener;
	unsigned int type;

	/* toplevel window */
	struct kmscon_session *session;
	bool active;
};

struct frame_cb {
	struct shl_dlist list;
	struct wl_resource w_resource;
};

static void output_schedule_redraw(struct output *output);
static void output_unschedule_redraw(struct output *output);

static void shell_surface_redraw_output(struct shell_surface *shell_surface,
					struct output *output)
{
	struct surface *surface;
	struct uterm_video_buffer buf;

	/* We currently draw the whole output white and the whole surface black
	 * before we draw the surface-buffer. This allows much easier debugging
	 * of the redraw-handler. This is overkill, but we will change that when
	 * we implement proper monitor handling. */

	uterm_display_fill(output->disp,
			   255,
			   255,
			   255,
			   0, 0,
			   uterm_mode_get_width(output->mode),
			   uterm_mode_get_height(output->mode));

	surface = shell_surface->surface;
	if (!surface->w_buffer)
		return;

	buf.width = wl_shm_buffer_get_width(surface->w_buffer);
	buf.height = wl_shm_buffer_get_height(surface->w_buffer);
	buf.stride = wl_shm_buffer_get_stride(surface->w_buffer);
	buf.format = UTERM_FORMAT_XRGB32;
	buf.data = wl_shm_buffer_get_data(surface->w_buffer);

	uterm_display_fill(output->disp,
			   0, 0, 0,
			   0, 0,
			   buf.width,
			   buf.height);
	uterm_display_blit(output->disp,
			   &buf,
			   0, 0);
}

static void shell_surface_schedule_redraw(struct shell_surface *shell_surface)
{
	struct compositor *comp = shell_surface->surface->comp;
	struct output *output;
	struct shl_dlist *iter;

	if (!shell_surface->active)
		return;

	shl_dlist_for_each(iter, &comp->outputs) {
		output = shl_dlist_entry(iter, struct output, list);
		output_schedule_redraw(output);
	}
}

static void w_shell_surface_pong(struct wl_client *client,
				 struct wl_resource *res,
				 uint32_t serial)
{
	log_warning("pong not implemented");
}

static void w_shell_surface_move(struct wl_client *client,
				 struct wl_resource *res,
				 struct wl_resource *seat_resource,
				 uint32_t serial)
{
	log_warning("move not implemented");
}

static void w_shell_surface_resize(struct wl_client *client,
				   struct wl_resource *res,
				   struct wl_resource *seat_res,
				   uint32_t serial,
				   uint32_t edges)
{
	log_warning("resize not implemented");
}

static void shell_surface_event(struct kmscon_session *session,
				unsigned int event, struct uterm_display *disp,
				void *data)
{
	struct shell_surface *shell_surface = data;
	struct compositor *comp = shell_surface->surface->comp;

	switch (event) {
	case KMSCON_SESSION_ACTIVATE:
		shell_surface->active = true;
		comp->active_shell_surface = shell_surface;
		shell_surface_schedule_redraw(shell_surface);
		break;
	case KMSCON_SESSION_DEACTIVATE:
		shell_surface->active = false;
		comp->active_shell_surface = NULL;
		break;
	case KMSCON_SESSION_UNREGISTER:
		shell_surface->type = SHELL_SURFACE_UNUSED;
		if (shell_surface->active) {
			shell_surface->active = false;
			comp->active_shell_surface = NULL;
		}
		break;
	}
}

static void w_shell_surface_set_toplevel(struct wl_client *client,
					 struct wl_resource *res)
{
	struct shell_surface *shell_surface;
	struct compositor *comp;
	int ret;

	shell_surface = res->data;
	comp = shell_surface->surface->comp;

	if (shell_surface->type == SHELL_SURFACE_TOPLEVEL)
		return;

	ret = kmscon_seat_register_session(comp->seat, &shell_surface->session,
					   shell_surface_event,
					   shell_surface);
	if (ret)
		return;
	kmscon_session_enable(shell_surface->session);

	shell_surface->type = SHELL_SURFACE_TOPLEVEL;
	shell_surface->active = false;
}

static void shell_surface_unset_toplevel(struct shell_surface *shell_surface)
{
	if (shell_surface->type != SHELL_SURFACE_TOPLEVEL)
		return;

	kmscon_session_unregister(shell_surface->session);
}

static void w_shell_surface_set_transient(struct wl_client *client,
					  struct wl_resource *res,
					  struct wl_resource *parent_res,
					  int32_t x, int32_t y,
					  uint32_t flags)
{
	struct shell_surface *shell_surface = res->data;

	log_warning("set-transient not implemented");

	shell_surface_unset_toplevel(shell_surface);
}

static void w_shell_surface_set_fullscreen(struct wl_client *client,
					   struct wl_resource *res,
					   uint32_t method,
					   uint32_t framerate,
					   struct wl_resource *output_res)
{
	log_warning("set-fullscreen not implemented");
}

static void w_shell_surface_set_popup(struct wl_client *client,
				      struct wl_resource *res,
				      struct wl_resource *seat_res,
				      uint32_t serial,
				      struct wl_resource *parent_res,
				      int32_t x, int32_t y,
				      uint32_t flags)
{
	struct shell_surface *shell_surface = res->data;

	log_warning("set-popup not implemented");

	shell_surface_unset_toplevel(shell_surface);
}

static void w_shell_surface_set_maximized(struct wl_client *client,
					  struct wl_resource *res,
					  struct wl_resource *output_res)
{
	log_warning("set-maximized not implemented");
}

static void w_shell_surface_set_title(struct wl_client *client,
				      struct wl_resource *res,
				      const char *title)
{
	log_warning("set-title (%s) not implemented", title);
}

static void w_shell_surface_set_class(struct wl_client *client,
				      struct wl_resource *res,
				      const char *classname)
{
	log_warning("set-class (%s) not implemented", classname);
}

static struct wl_shell_surface_interface w_shell_surface_interface = {
	.pong = w_shell_surface_pong,
	.move = w_shell_surface_move,
	.resize = w_shell_surface_resize,
	.set_toplevel = w_shell_surface_set_toplevel,
	.set_transient = w_shell_surface_set_transient,
	.set_fullscreen = w_shell_surface_set_fullscreen,
	.set_popup = w_shell_surface_set_popup,
	.set_maximized = w_shell_surface_set_maximized,
	.set_title = w_shell_surface_set_title,
	.set_class = w_shell_surface_set_class,
};

static void w_destroy_shell_surface(struct wl_resource *res)
{
	struct shell_surface *shell_surface = res->data;

	shell_surface_unset_toplevel(shell_surface);
	wl_list_remove(&shell_surface->w_surface_destroy_listener.link);
	shell_surface->surface->shell_surface = NULL;
	free(shell_surface);
}

static void w_shell_surface_on_surface_destroy(struct wl_listener *listener,
					       void *data)
{
	struct shell_surface *shell_surface;

	shell_surface = shl_offsetof(listener,
				     struct shell_surface,
				     w_surface_destroy_listener);

	wl_resource_destroy(&shell_surface->w_resource);
}

static void w_shell_get_shell_surface(struct wl_client *client,
				      struct wl_resource *res,
				      uint32_t id,
				      struct wl_resource *surface_res)
{
	struct surface *surface = surface_res->data;
	struct shell_surface *shell_surface;

	if (surface->shell_surface) {
		wl_resource_post_error(surface_res,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "shell::get_shell_surface already requested");
		log_debug("client request shell-surface twice");
		return;
	}

	shell_surface = malloc(sizeof(*shell_surface));
	if (!shell_surface) {
		wl_resource_post_no_memory(surface_res);
		return;
	}
	memset(shell_surface, 0, sizeof(*shell_surface));
	shell_surface->surface = surface;
	shell_surface->type = SHELL_SURFACE_UNUSED;
	shell_surface->w_surface_destroy_listener.notify =
					w_shell_surface_on_surface_destroy;

	shell_surface->w_resource.object.interface = &wl_shell_surface_interface;
	shell_surface->w_resource.object.implementation =
			(void (*const*) (void))&w_shell_surface_interface;
	shell_surface->w_resource.object.id = id;
	shell_surface->w_resource.destroy = w_destroy_shell_surface;
	shell_surface->w_resource.data = shell_surface;

	if (!wl_client_add_resource(client, &shell_surface->w_resource)) {
		free(shell_surface);
		return;
	}

	wl_signal_add(&surface->w_surface.resource.destroy_signal,
		      &shell_surface->w_surface_destroy_listener);
	surface->shell_surface = shell_surface;
}

static struct wl_shell_interface w_shell_interface = {
	.get_shell_surface = w_shell_get_shell_surface,
};

static void w_shell_bind(struct wl_client *client, void *data,
			 uint32_t version, uint32_t id)
{
	struct compositor *comp = data;
	struct wl_resource *res;

	res = wl_client_add_object(client, &wl_shell_interface,
				   &w_shell_interface, id, comp);
	if (!res)
		log_warning("cannot add shell-interface to client (%d): %m",
			    errno);
}

static void surface_call_fcbs(struct surface *surface)
{
	struct shl_dlist *iter, *tmp;
	struct frame_cb *fcb;
	struct timeval tv;
	uint32_t t;

	gettimeofday(&tv, NULL);
	t = tv.tv_sec * 1000 + tv.tv_usec / 1000;

	shl_dlist_for_each_safe(iter, tmp, &surface->frame_cbs) {
		fcb = shl_dlist_entry(iter, struct frame_cb, list);
		wl_callback_send_done(&fcb->w_resource, t);
		wl_resource_destroy(&fcb->w_resource);
	}
}

static void w_surface_destroy(struct wl_client *client, struct wl_resource *res)
{
	wl_resource_destroy(res);
}

static void w_surface_attach(struct wl_client *client, struct wl_resource *res,
			     struct wl_resource *buffer_res,
			     int32_t sx, int32_t sy)
{
	struct surface *surface = res->data;

	if (surface->w_buffer) {
		wl_list_remove(&surface->w_buffer_destroy_listener.link);
		surface->w_buffer = NULL;
		/* TODO: send buffer-release event */
	}

	if (!buffer_res)
		return;

	surface->w_buffer = buffer_res->data;
	if (!wl_buffer_is_shm(surface->w_buffer)) {
		log_debug("non-shm buffer attached; not supported");
		return;
	}

	wl_signal_add(&surface->w_buffer->resource.destroy_signal,
		      &surface->w_buffer_destroy_listener);

	/* TODO: send configure event */
}

static void w_surface_damage(struct wl_client *client, struct wl_resource *res,
			     int32_t x, int32_t y, int32_t w, int32_t h)
{
	struct surface *surface = res->data;

	if (!surface->shell_surface)
		return;

	shell_surface_schedule_redraw(surface->shell_surface);
}

static void w_destroy_frame_cb(struct wl_resource *res)
{
	struct frame_cb *fcb = res->data;

	shl_dlist_unlink(&fcb->list);
	free(fcb);
}

static void w_surface_frame(struct wl_client *client, struct wl_resource *res,
			    uint32_t callback)
{
	struct frame_cb *fcb;
	struct surface *surface = res->data;

	fcb = malloc(sizeof(*fcb));
	if (!fcb) {
		wl_resource_post_no_memory(res);
		return;
	}
	memset(fcb, 0, sizeof(*fcb));
	fcb->w_resource.object.interface = &wl_callback_interface;
	fcb->w_resource.object.implementation = NULL;
	fcb->w_resource.object.id = callback;
	fcb->w_resource.destroy = w_destroy_frame_cb;
	fcb->w_resource.data = fcb;

	if (!wl_client_add_resource(client, &fcb->w_resource)) {
		free(fcb);
		return;
	}
	shl_dlist_link(&surface->frame_cbs, &fcb->list);
}

static void w_surface_set_opaque_region(struct wl_client *client,
					struct wl_resource *res,
					struct wl_resource *region_res)
{
	log_warning("opaque region not implemented");
}

static void w_surface_set_input_region(struct wl_client *client,
				       struct wl_resource *res,
				       struct wl_resource *region_res)
{
	log_warning("input region not implemented");
}

static struct wl_surface_interface w_surface_interface = {
	.destroy = w_surface_destroy,
	.attach = w_surface_attach,
	.damage = w_surface_damage,
	.frame = w_surface_frame,
	.set_opaque_region = w_surface_set_opaque_region,
	.set_input_region = w_surface_set_input_region,
};

static void w_destroy_surface(struct wl_resource *res)
{
	struct surface *surface = res->data;
	struct shl_dlist *iter, *tmp;
	struct frame_cb *fcb;

	shl_dlist_for_each_safe(iter, tmp, &surface->frame_cbs) {
		fcb = shl_dlist_entry(iter, struct frame_cb, list);
		wl_resource_destroy(&fcb->w_resource);
	}

	shl_dlist_unlink(&surface->list);
	free(surface);
}

static void w_surface_on_buffer_destroy(struct wl_listener *listener,
					void *data)
{
	struct surface *surface;

	surface = shl_offsetof(listener, struct surface,
			       w_buffer_destroy_listener);

	if (surface->w_buffer) {
		wl_list_remove(&surface->w_buffer_destroy_listener.link);
		surface->w_buffer = NULL;
	}
}

static void w_compositor_create_surface(struct wl_client *client,
					struct wl_resource *res,
					uint32_t id)
{
	struct compositor *comp = res->data;
	struct surface *surface;

	surface = malloc(sizeof(*surface));
	if (!surface) {
		wl_resource_post_no_memory(res);
		return;
	}
	memset(surface, 0, sizeof(*surface));
	surface->comp = comp;
	shl_dlist_init(&surface->frame_cbs);

	surface->w_surface.resource.object.interface = &wl_surface_interface;
	surface->w_surface.resource.object.implementation =
			(void (*const*) (void))&w_surface_interface;
	surface->w_surface.resource.object.id = id;
	surface->w_surface.resource.destroy = w_destroy_surface;
	surface->w_surface.resource.data = surface;

	surface->w_buffer_destroy_listener.notify = w_surface_on_buffer_destroy;

	if (!wl_client_add_resource(client, &surface->w_surface.resource)) {
		free(surface);
		return;
	}

	shl_dlist_link(&comp->surfaces, &surface->list);
}

static void w_compositor_create_region(struct wl_client *client,
				       struct wl_resource *res,
				       uint32_t id)
{
	log_warning("region interface not implemented");
}

static const struct wl_compositor_interface w_compositor_interface = {
	.create_surface = w_compositor_create_surface,
	.create_region = w_compositor_create_region,
};

static void w_compositor_bind(struct wl_client *client, void *data,
			      uint32_t version, uint32_t id)
{
	struct compositor *comp = data;
	struct wl_resource *res;

	res = wl_client_add_object(client, &wl_compositor_interface,
				   &w_compositor_interface, id, comp);
	if (!res)
		log_warning("cannot add compositor-interface to client (%d): %m",
			    errno);
}

/* Output management */

static void output_redraw_event(struct ev_eloop *eloop, void *ptr, void *data)
{
	struct output *output = data;
	struct compositor *comp = output->comp;
	int ret;

	output_unschedule_redraw(output);

	if (!output->comp->active_shell_surface)
		return;

	output->pending_page_flip = true;
	shell_surface_redraw_output(comp->active_shell_surface, output);
	ret = uterm_display_swap(output->disp);
	if (ret) {
		log_warning("cannot schedule page flip: %d", ret);
		output->pending_page_flip = false;
	}
}

static void output_page_flip_event(struct output *output)
{
	output->pending_page_flip = false;

	if (output->comp->active_shell_surface)
		surface_call_fcbs(output->comp->active_shell_surface->surface);

	if (output->redraw)
		output_redraw_event(output->comp->eloop, NULL, output);
}

static void output_schedule_redraw(struct output *output)
{
	int ret;
	struct compositor *comp = output->comp;

	if (output->redraw)
		return;

	if (!output->pending_page_flip) {
		ret = ev_eloop_register_idle_cb(comp->eloop,
						output_redraw_event,
						output);
		if (ret) {
			log_warning("cannot register redraw idle callback: %d",
				    ret);
			return;
		}
	}

	output->redraw = true;
}

static void output_unschedule_redraw(struct output *output)
{
	struct compositor *comp = output->comp;

	if (!output->redraw)
		return;

	output->redraw = false;
	if (!output->pending_page_flip)
		ev_eloop_unregister_idle_cb(comp->eloop, output_redraw_event,
					    output);
}

static void output_event(struct uterm_display *disp,
			 struct uterm_display_event *ev,
			 void *data)
{
	struct output *output = data;

	switch (ev->action) {
	case UTERM_PAGE_FLIP:
		output_page_flip_event(output);
		break;
	}
}

static int compositor_add_output(struct compositor *comp,
				 struct uterm_display *disp)
{
	struct output *output;
	struct uterm_mode *mode;
	int ret;

	mode = uterm_display_get_current(disp);
	if (!mode) {
		log_error("display has no mode set");
		return -EFAULT;
	}

	output = malloc(sizeof(*output));
	if (!output)
		return -ENOMEM;
	memset(output, 0, sizeof(*output));
	output->comp = comp;
	output->disp = disp;
	output->mode = mode;

	ret = uterm_display_register_cb(output->disp, output_event,
					output);
	if (ret)
		goto err_free;

	shl_dlist_link(&comp->outputs, &output->list);
	output_schedule_redraw(output);
	return 0;

err_free:
	free(output);
	return ret;
}

static void compositor_remove_output(struct compositor *comp,
				     struct output *output)
{
	shl_dlist_unlink(&output->list);
	output_unschedule_redraw(output);
	uterm_display_unregister_cb(output->disp, output_event,
				    output);
	free(output);
}

static struct output *compositor_find_output(struct compositor *comp,
					     struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct output *output;

	shl_dlist_for_each(iter, &comp->outputs) {
		output = shl_dlist_entry(iter, struct output, list);
		if (output->disp == disp)
			return output;
	}

	return NULL;
}

/*
 * Wayland Event Loop
 * Each wayland display uses its own event loop implementation. As the
 * idle-sources of wayland event-loops have no associated file-descriptor, we
 * cannot simply add it into eloop. Therefore, we use a combination of pre-cb
 * and fd-cb to integrate wayland-event-loops into eloops.
 */

static void compositor_eloop_event(struct ev_fd *fd, int mask, void *data)
{
	struct compositor *comp = data;

	wl_event_loop_dispatch(comp->w_eloop, 0);
}

static void compositor_pre_event(struct ev_eloop *eloop, void *nil, void *data)
{
	struct compositor *comp = data;

	wl_event_loop_dispatch_idle(comp->w_eloop);
}

static void compositor_destroy(struct compositor *comp)
{
	struct output *output;

	while (!shl_dlist_empty(&comp->outputs)) {
		output = shl_dlist_entry(comp->outputs.prev,
					 struct output, list);
		compositor_remove_output(comp, output);
	}

	ev_eloop_unregister_pre_cb(comp->eloop, compositor_pre_event, comp);
	ev_eloop_rm_fd(comp->eloop_fd);
	wl_display_destroy(comp->w_display);
	free(comp);
}

static void compositor_session_event(struct kmscon_session *s, unsigned int ev,
				     struct uterm_display *disp, void *data)
{
	struct compositor *comp = data;
	struct output *output;

	switch (ev) {
	case KMSCON_SESSION_DISPLAY_NEW:
		compositor_add_output(comp, disp);
		break;
	case KMSCON_SESSION_DISPLAY_GONE:
		output = compositor_find_output(comp, disp);
		if (!output)
			return;
		compositor_remove_output(comp, output);
		break;
	case KMSCON_SESSION_UNREGISTER:
		compositor_destroy(comp);
		break;
	}
}

int kmscon_compositor_register(struct kmscon_session **out,
			       struct kmscon_seat *seat)
{
	struct compositor *comp;
	int ret;
	void *global;

	if (!out || !seat)
		return -EINVAL;

	comp = malloc(sizeof(*comp));
	if (!comp)
		return -ENOMEM;
	memset(comp, 0, sizeof(*comp));
	comp->seat = seat;
	comp->eloop = kmscon_seat_get_eloop(seat);
	shl_dlist_init(&comp->outputs);
	shl_dlist_init(&comp->surfaces);

	comp->w_display = wl_display_create();
	if (!comp->w_display) {
		log_error("cannot create wayland display (%d): %m", errno);
		ret = -EFAULT;
		goto err_free;
	}
	comp->w_eloop = wl_display_get_event_loop(comp->w_display);

	ret = ev_eloop_new_fd(comp->eloop,
			      &comp->eloop_fd,
			      wl_event_loop_get_fd(comp->w_eloop),
			      EV_READABLE,
			      compositor_eloop_event,
			      comp);
	if (ret) {
		log_error("cannot register eloop-fd: %d", ret);
		goto err_w_display;
	}

	ret = ev_eloop_register_pre_cb(comp->eloop, compositor_pre_event, comp);
	if (ret) {
		log_error("cannot register pre-cb: %d", ret);
		goto err_eloop_fd;
	}

	ret = wl_display_add_socket(comp->w_display, NULL);
	if (ret) {
		log_error("cannot add default socket to wl-display (%d): %m",
			  errno);
		goto err_pre;
	}

	global = wl_display_add_global(comp->w_display,
				       &wl_compositor_interface, comp,
				       w_compositor_bind);
	if (!global) {
		log_error("cannot add wl-compositor global (%d): %m", errno);
		goto err_pre;
	}

	global = wl_display_add_global(comp->w_display, &wl_shell_interface,
				       comp, w_shell_bind);
	if (!global) {
		log_error("cannot add wl-shell global (%d): %m", errno);
		goto err_pre;
	}

	ret = wl_display_init_shm(comp->w_display);
	if (ret) {
		log_error("cannot init wl-shm global (%d): %m", errno);
		ret = -EFAULT;
		goto err_pre;
	}

	ret = kmscon_seat_register_session(comp->seat, &comp->session,
					   compositor_session_event, comp);
	if (ret) {
		log_error("cannot register session for compositor: %d", ret);
		goto err_pre;
	}

	*out = comp->session;
	return 0;

err_pre:
	ev_eloop_unregister_pre_cb(comp->eloop, compositor_pre_event, comp);
err_eloop_fd:
	ev_eloop_rm_fd(comp->eloop_fd);
err_w_display:
	wl_display_destroy(comp->w_display);
err_free:
	free(comp);
	return ret;
}
