/* rosgd-compositor.c -- ROSGD's compositor (RISCOSGrandDesign design 29,
 * G5): a wlroots program in the box's Linux root that blends the RISC OS
 * desktop, the Wimp's surfaces and Linux programs' Wayland surfaces, the
 * Wimp being the window manager (X1, decided 6 October 2026).
 *
 * Step 1: the RISC OS screen as the bottom layer (rosgd.display=
 * compositor), its pointer on top -- the section "the RISC OS screen".
 * Built on wlroots' own reference compositor, tinywl, as wlroots 0.20.2
 * ships it (MIT: Copyright (c) 2017, 2018 Drew DeVault; (c) 2014 Jari
 * Vetoniemi; (c) 2023 the wlroots contributors -- the licence is in
 * LICENSE.wlroots beside this file), unchanged but for the screencopy
 * and xdg-output protocols, so that a test can capture what it composites
 * (grim), and a size for an output with no modes (headless).  It is
 * the starting point the RISC OS layers and the Wimp's policy replace,
 * step by step (README.md).
 */
#define _GNU_SOURCE                     /* accept4 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
	TINYWL_CURSOR_PASSTHROUGH,
	TINYWL_CURSOR_MOVE,
	TINYWL_CURSOR_RESIZE,
};

struct tinywl_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum tinywl_cursor_mode cursor_mode;
	struct tinywl_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
};

struct tinywl_output {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct tinywl_toplevel {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	/* ROSGD: its Wimp window (the section "the Wimp as window manager") */
	uint32_t id;
	bool mapped;
	int sent_w, sent_h;
	struct wl_listener set_title;
	struct wlr_scene_tree *popups;          /* its popups, over the desktop */
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener decoration_destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct tinywl_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct tinywl_keyboard {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ROSGD: the Wimp is the window manager (below) */
static struct wlr_scene_tree *wm_windows, *wm_popups;
static void wm_new(struct tinywl_toplevel *toplevel);
static void wm_gone(struct tinywl_toplevel *toplevel);
static void wm_title(struct tinywl_toplevel *toplevel);
static void wm_size(struct tinywl_toplevel *toplevel);

static void focus_toplevel(struct tinywl_toplevel *toplevel) {
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL) {
		return;
	}
	struct tinywl_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Move the toplevel to the front */
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(struct tinywl_server *server, xkb_keysym_t sym) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * This function assumes Alt is held down.
	 */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_F1:
		/* Cycle to the next toplevel */
		if (wl_list_length(&server->toplevels) < 2) {
			break;
		}
		struct tinywl_toplevel *next_toplevel =
			wl_container_of(server->toplevels.prev, next_toplevel, link);
		focus_toplevel(next_toplevel);
		break;
	default:
		return false;
	}
	return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct tinywl_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) &&
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	/* This event is raised by the keyboard base wlr_input_device to signal
	 * the destruction of the wlr_keyboard. It will no longer receive events
	 * and should be destroyed.
	 */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct tinywl_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct tinywl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct tinywl_server *server,
		struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* ROSGD: started by the box (-b): the pointer is RISC OS's */
static bool ros_box;

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	/* ROSGD: RISC OS's pointer is the only one shown */
	(void)focused_client, (void)event;
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(
			listener, server, pointer_focus_change);
	/* This event is raised when the pointer focus is changed, including when the
	 * client is closed. We set the cursor image to its default if target surface
	 * is NULL */
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL && !ros_box) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	struct tinywl_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct tinywl_toplevel *desktop_toplevel_at(
		struct tinywl_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost node in the scene at the given layout coords.
	 * We only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a tinywl_toplevel. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Find the node corresponding to the tinywl_toplevel at the root of this
	 * surface tree, it is the only one for which we set the data field. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree->node.data;
}

static void reset_cursor_mode(struct tinywl_server *server) {
	/* Reset the cursor mode to passthrough. */
	server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct tinywl_server *server) {
	/* Move the grabbed toplevel to the new position. */
	struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct tinywl_server *server) {
	/*
	 * Resizing the grabbed toplevel can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the
	 * toplevel on one or two axes, but can also move the toplevel if you resize
	 * from the top or left edges (or top-left corner).
	 *
	 * Note that some shortcuts are taken here. In a more fleshed-out
	 * compositor, you'd wait for the client to prepare a buffer at the new
	 * size, then commit any movement that was prepared.
	 */
	struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(struct tinywl_server *server, uint32_t time) {
	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == TINYWL_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	/* Otherwise, find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct tinywl_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!toplevel && !ros_box) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		/*
		 * Send pointer enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	/* Notify the client with pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* If you released any buttons, we exit interactive move/resize mode. */
		reset_cursor_mode(server);
	} else {
		/* Focus that client if the button was _pressed_ */
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct tinywl_toplevel *toplevel = desktop_toplevel_at(server,
				server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		focus_toplevel(toplevel);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct tinywl_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	/* Render the scene if needed and commit the output */
	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* This function is called when the backend requests a new state for
	 * the output. For example, Wayland and X11 backends request a new mode
	 * when the output window is resized. */
	struct tinywl_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct tinywl_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before committing the output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* The output may be disabled, switch it on. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	/* Some backends don't have modes. DRM+KMS does, and we need to set a mode
	 * before we can use the output. The mode is a tuple of (width, height,
	 * refresh rate), and each monitor supports only a specific set of modes. We
	 * just pick the monitor's preferred mode, a more sophisticated compositor
	 * would let the user configure it. */
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	} else {
		/* ROSGD: an output with no modes of its own (headless) is given
		 * one -- in time the RISC OS desktop's size */
		wlr_output_state_set_custom_mode(&state, 1280, 800, 0);
	}

	/* Atomically applies the new output state. */
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Allocates and configures our state for this output */
	struct tinywl_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;

	/* Sets up a listener for the frame event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	/* Sets up a listener for the state request event. */
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	/* Sets up a listener for the destroy event. */
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

	/* ROSGD: not focused here; RISC OS gives it a window, and the
	 * keyboard when that window has the caret */
	toplevel->mapped = true;
	wm_new(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

	/* Reset the cursor mode if the grabbed toplevel was unmapped. */
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}

	wl_list_remove(&toplevel->link);
	toplevel->mapped = false;
	wm_gone(toplevel);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface. tinywl
		 * configures the xdg_toplevel with 0,0 size to let the client pick the
		 * dimensions itself. */
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		/* ROSGD: the Wimp draws the title bar and icons */
		if (toplevel->decoration) {
			wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
	}
	if (toplevel->mapped) {
		wm_size(toplevel);
	}
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
	if (toplevel->mapped) {
		wm_title(toplevel);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->set_title.link);
	if (toplevel->decoration) {
		wl_list_remove(&toplevel->decoration_destroy.link);
	}
	wlr_scene_node_destroy(&toplevel->popups->node);

	free(toplevel);
}

static void begin_interactive(struct tinywl_toplevel *toplevel,
		enum tinywl_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propagating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct tinywl_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == TINYWL_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

		double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = *geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, TINYWL_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on client-side
	 * decorations. tinywl doesn't support maximization, but to conform to
	 * xdg-shell protocol we still must send a configure.
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply.
	 * However, if the request was sent before an initial commit, we don't do
	 * anything and let the client finish the initial surface setup. */
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	/* Just as with request_maximize, we must send a configure here. */
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new toplevel (application window). */
	struct tinywl_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	/* Allocate a tinywl_toplevel for this surface */
	struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	/* ROSGD: under the desktop, out of sight until the Wimp places it */
	static uint32_t next_id = 1;
	toplevel->id = next_id++;
	toplevel->scene_tree = wlr_scene_xdg_surface_create(wm_windows, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
	toplevel->popups = wlr_scene_tree_create(wm_popups);
	wlr_scene_node_set_enabled(&toplevel->popups->node, false);
	xdg_toplevel->base->data = toplevel->scene_tree;

	/* Listen to the various events it can emit */
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	/* cotd */
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
	toplevel->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct tinywl_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface.
		 * tinywl sends an empty configure. A more sophisticated compositor
		 * might change an xdg_popup's geometry to ensure it's not positioned
		 * off-screen, for example. */
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_popup is destroyed. */
	struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new popup. */
	struct wlr_xdg_popup *xdg_popup = data;

	struct tinywl_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	/* We must add xdg popups to the scene graph so they get rendered. The
	 * wlroots scene graph provides a helper for this, but to use it we must
	 * provide the proper parent scene node of the xdg popup. To enable this,
	 * we always set the user data field of xdg_surfaces to the corresponding
	 * scene node. */
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	assert(parent != NULL);
	struct wlr_scene_tree *parent_tree = parent->data;
	/* ROSGD: a toplevel's popups over the desktop, in a tree that
	 * follows it, so that a menu is not cut off at its window's edge */
	if (parent->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && parent_tree->node.data) {
		struct tinywl_toplevel *t = parent_tree->node.data;
		parent_tree = t->popups;
	}
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- ROSGD: the RISC OS screen, the bottom layer --------------------------
 *
 * With rosgd.display=compositor, RISC OS draws into shared memory and does
 * not drive the display: /init publishes that memory as /dev/rosgd-screen,
 * a link to its memfd (a new one each mode).  Its last 4 KB is the screen
 * block (rosgd/include/rosgd/screen.h): the mode, the bank shown (the pan),
 * the palette, and the pointer, whose 64 x 64 image is in the buffer too.
 *
 * Every 20 ms the screen is converted, the shown bank at whatever depth, to
 * ARGB8888, into the one of two buffers not on show; if any row changed,
 * the scene is given it, damaged in those rows.  It is opaque but where
 * the Wimp's external windows are seen (the section after this), clear
 * there, and the Wayland windows are under it.  The output is given the
 * largest whole-number scale at which the screen fits, so that a layout
 * pixel is a RISC OS pixel, and the screen is centred.  RISC OS's pointer
 * is the output's cursor.  No link (an ordinary box), and this does nothing. */

struct ros_block {
	uint32_t magic, version, generation, xres, yres, pitch, bpp, pixo;
	uint32_t xoffset, yoffset;
	int32_t ptr_x, ptr_y;
	uint32_t ptr_shown, ptr_shape, ptr_image, ptr_hot;
	uint32_t palette[256];
};

/* the external windows' table, after the palette (screen.h) */
#define ROS_SCREEN_EXT (64u + 1024u)
#define ROS_EXT_WINDOWS 16u
#define ROS_EXT_RECTS 160u
struct ros_ext {
	uint32_t seq, nwindows, nrects, reserved;
	struct { uint32_t id; int16_t x, y; } window[ROS_EXT_WINDOWS];
	struct { uint32_t id; int16_t x0, y0, x1, y1; } rect[ROS_EXT_RECTS];
};
#define ROS_SCREEN_MAGIC 0x44475352u
#define ROS_SCREEN_BLOCK 4096u
#define ROS_POINTER 64

/* a buffer of our own memory, for scene nodes */
struct pixbuf {
	struct wlr_buffer base;
	uint32_t *data;
	uint32_t format;
};

static void pixbuf_destroy(struct wlr_buffer *b) {
	struct pixbuf *p = wl_container_of(b, p, base);
	wlr_buffer_finish(b);
	free(p->data);
	free(p);
}

static bool pixbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	struct pixbuf *p = wl_container_of(b, p, base);
	*data = p->data;
	*format = p->format;
	*stride = (size_t)b->width * 4;
	return true;
}

static void pixbuf_end(struct wlr_buffer *b) {
}

static const struct wlr_buffer_impl pixbuf_impl = {
	.destroy = pixbuf_destroy,
	.begin_data_ptr_access = pixbuf_begin,
	.end_data_ptr_access = pixbuf_end,
};

static struct pixbuf *pixbuf_new(int width, int height, uint32_t format) {
	struct pixbuf *p = calloc(1, sizeof(*p));
	if (!p) {
		return NULL;
	}
	p->data = calloc((size_t)width * height, 4);
	if (!p->data) {
		free(p);
		return NULL;
	}
	p->format = format;
	wlr_buffer_init(&p->base, &pixbuf_impl, width, height);
	return p;
}

static struct {
	struct tinywl_server *server;
	const char *path;
	char link[128];
	int fd;
	uint8_t *map;
	size_t size;
	uint32_t generation;
	int width, height;
	struct pixbuf *buf[2];
	int shown;
	struct wlr_scene_buffer *desktop;
	uint32_t ptr_shape;
	int ptr_set;                    /* the cursor has RISC OS's image */
	int ptr_lx, ptr_ly;             /* and is there, layout */
	struct wl_event_source *timer;
	struct ros_ext ext;             /* the last whole table read */
	struct ros_ext ext_drawn;       /* the table the buffer was made with */
	int ox, oy;                     /* the desktop's top left, layout */
	uint8_t *raw;                   /* the RISC OS rows as last converted */
	size_t raw_size;
	uint32_t raw_pal[256];
	unsigned still, tick;           /* power: frames looked at with no change */
} screen = { .fd = -1 };

static void screen_close(void) {
	if (screen.map) {
		munmap(screen.map, screen.size);
	}
	if (screen.fd >= 0) {
		close(screen.fd);
	}
	screen.map = NULL;
	screen.fd = -1;
	screen.link[0] = 0;
}

/* (re)open the memory when the link names another memfd */
static bool screen_open(void) {
	char link[sizeof(screen.link)];
	ssize_t n = readlink(screen.path, link, sizeof(link) - 1);
	if (n <= 0) {
		screen_close();
		return false;
	}
	link[n] = 0;
	if (screen.map && strcmp(link, screen.link) == 0) {
		return true;
	}
	screen_close();
	int fd = open(screen.path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < ROS_SCREEN_BLOCK) {
		if (fd >= 0) {
			close(fd);
		}
		return false;
	}
	void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return false;
	}
	screen.fd = fd;
	screen.map = map;
	screen.size = (size_t)st.st_size;
	strcpy(screen.link, link);
	screen.generation = 0;
	wlr_log(WLR_INFO, "ROSGD: the RISC OS screen, %s, %zu bytes", link, screen.size);
	return true;
}

static uint32_t xrgb_from_palette(uint32_t bgr) {
	return (bgr & 0xff) << 16 | (bgr & 0xff00) | (bgr >> 16 & 0xff);
}

/* row y of the bank shown, as XRGB8888 */
static void screen_row(const struct ros_block *b, int y, uint32_t *out) {
	const uint8_t *row = screen.map + (size_t)(b->yoffset + y) * b->pitch;
	uint32_t x0 = b->xoffset;
	switch (b->bpp) {
	case 32:
		for (uint32_t x = 0; x < b->xres; x++) {
			uint32_t v = ((const uint32_t *)row)[x0 + x];
			out[x] = b->pixo ? xrgb_from_palette(v) : (v & 0xffffff);
		}
		break;
	case 16:
		for (uint32_t x = 0; x < b->xres; x++) {
			uint32_t v = ((const uint16_t *)row)[x0 + x];
			uint32_t r = v >> 11 & 31, g = v >> 5 & 63, bl = v & 31;
			out[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (bl << 3 | bl >> 2);
		}
		break;
	default: {
		/* 8 bpp and below: through the palette; the leftmost pixel in
		 * a byte's lowest bits, as RISC OS packs them */
		uint32_t bpp = b->bpp, mask = (1u << bpp) - 1;
		for (uint32_t x = 0; x < b->xres; x++) {
			uint32_t bit = (x0 + x) * bpp;
			uint32_t i = row[bit >> 3] >> (bit & 7) & mask;
			out[x] = xrgb_from_palette(b->palette[i]);
		}
		break;
	}
	}
}

/* the external windows' table, if a whole one can be read now; else the
 * last one stands */
static void screen_ext(void) {
	const volatile struct ros_ext *e =
		(const void *)(screen.map + screen.size - ROS_SCREEN_BLOCK + ROS_SCREEN_EXT);
	uint32_t seq = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
	if (seq & 1) {
		return;
	}
	struct ros_ext copy;
	memcpy(&copy, (const void *)e, sizeof(copy));
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != seq) {
		return;
	}
	if (copy.nwindows > ROS_EXT_WINDOWS) {
		copy.nwindows = ROS_EXT_WINDOWS;
	}
	if (copy.nrects > ROS_EXT_RECTS) {
		copy.nrects = ROS_EXT_RECTS;
	}
	screen.ext = copy;
}

/* clear where an external window is seen through the desktop */
static void screen_holes(int y, uint32_t *out, int w) {
	for (uint32_t k = 0; k < screen.ext.nrects; k++) {
		int x0 = screen.ext.rect[k].x0, x1 = screen.ext.rect[k].x1;
		if (y < screen.ext.rect[k].y0 || y >= screen.ext.rect[k].y1) {
			continue;
		}
		if (x0 < 0) {
			x0 = 0;
		}
		if (x1 > w) {
			x1 = w;
		}
		if (x0 < x1) {
			memset(out + x0, 0, (size_t)(x1 - x0) * 4);
		}
	}
}

/* the output's scale: the largest whole one at which the RISC OS screen
 * fits, so that a layout pixel is a RISC OS pixel */
static void screen_scale(int w, int h) {
	struct tinywl_output *o;
	wl_list_for_each(o, &screen.server->outputs, link) {
		struct wlr_output *out = o->wlr_output;
		if (!out->enabled || out->width <= 0 || out->height <= 0) {
			continue;
		}
		int sc = out->width / w < out->height / h ? out->width / w : out->height / h;
		if (sc < 1) {
			sc = 1;
		}
		if ((int)out->scale != sc) {
			struct wlr_output_state state;
			wlr_output_state_init(&state);
			wlr_output_state_set_scale(&state, (float)sc);
			wlr_output_commit_state(out, &state);
			wlr_output_state_finish(&state);
		}
	}
}

static void wm_place(void);

static int screen_tick(void *data) {
	wl_event_source_timer_update(screen.timer, 20);
	if (!screen_open()) {
		wlr_scene_node_set_enabled(&screen.desktop->node, false);
		if (screen.ptr_set) {
			wlr_cursor_unset_image(screen.server->cursor);
			screen.ptr_set = 0;
		}
		return 0;
	}
	const struct ros_block *b = (const void *)(screen.map + screen.size - ROS_SCREEN_BLOCK);
	if (b->magic != ROS_SCREEN_MAGIC || !b->xres || !b->yres || b->xres > 8192 || b->yres > 8192 ||
			(size_t)(b->yoffset + b->yres) * b->pitch > screen.size - ROS_SCREEN_BLOCK) {
		return 0;
	}
	int w = (int)b->xres, h = (int)b->yres;
	bool full = false;
	if (b->generation != screen.generation || w != screen.width || h != screen.height) {
		for (int i = 0; i < 2; i++) {
			if (screen.buf[i]) {
				wlr_buffer_drop(&screen.buf[i]->base);
			}
			screen.buf[i] = pixbuf_new(w, h, DRM_FORMAT_ARGB8888);
		}
		if (!screen.buf[0] || !screen.buf[1]) {
			return 0;
		}
		screen.generation = b->generation;
		screen.width = w;
		screen.height = h;
		memset(&screen.ext, 0, sizeof(screen.ext));
		full = true;
	}
	screen_ext();

	/* Only the rows RISC OS changed are converted: each is compared, as
	 * RISC OS left it, with what it was when last converted.  A new
	 * window table, a new palette or a new mode is all of it.  Power: a
	 * screen that has been still for a tenth of a second is compared only
	 * every fifth tick (ten a second); a change puts it back at fifty for
	 * as long as changes come.  (The pointer is a node of its own, moved
	 * every tick whatever this does.) */
	size_t need = (size_t)b->pitch * h;
	if (need != screen.raw_size) {
		free(screen.raw);
		screen.raw = malloc(need);
		screen.raw_size = screen.raw ? need : 0;
		full = true;
	}
	if (!screen.raw) {
		return 0;
	}
	if (b->bpp <= 8 && memcmp(screen.raw_pal, b->palette, sizeof(screen.raw_pal)) != 0) {
		memcpy(screen.raw_pal, b->palette, sizeof(screen.raw_pal));
		full = true;
	}
	if (memcmp(&screen.ext_drawn, &screen.ext, sizeof(screen.ext)) != 0) {
		screen.ext_drawn = screen.ext;
		full = true;
	}
	screen.tick++;
	bool look = full || screen.still < 5 || screen.tick % 5 == 0;

	/* the changed rows into the buffer not on show (the rest are the same
	 * in both), opaque but where external windows are seen */
	struct pixbuf *next = screen.buf[screen.shown ^ 1], *prev = screen.buf[screen.shown];
	int first = -1, last = -1;
	for (int y = 0; look && y < h; y++) {
		const uint8_t *src = screen.map + (size_t)(b->yoffset + y) * b->pitch;
		uint8_t *was = screen.raw + (size_t)y * b->pitch;
		if (!full && memcmp(was, src, b->pitch) == 0) {
			continue;
		}
		memcpy(was, src, b->pitch);
		uint32_t *out = next->data + (size_t)y * w;
		screen_row(b, y, out);
		for (int x = 0; x < w; x++) {
			out[x] |= 0xff000000u;
		}
		screen_holes(y, out, w);
		if (first < 0) {
			first = y;
		}
		last = y;
	}
	if (first >= 0) {
		screen.still = 0;
	} else if (look) {
		screen.still++;
	}

	/* where it goes: centred, the output scaled to fit it */
	screen_scale(w, h);
	struct wlr_box box;
	wlr_output_layout_get_box(screen.server->output_layout, NULL, &box);
	int ox = box.x + (box.width - w) / 2, oy = box.y + (box.height - h) / 2;
	screen.ox = ox, screen.oy = oy;
	wlr_scene_node_set_enabled(&screen.desktop->node, true);
	wlr_scene_node_set_position(&screen.desktop->node, ox, oy);
	wlr_scene_buffer_set_dest_size(screen.desktop, w, h);
	if (first >= 0) {
		pixman_region32_t damage;
		pixman_region32_init_rect(&damage, 0, first, w, last - first + 1);
		wlr_scene_buffer_set_buffer_with_damage(screen.desktop, &next->base, &damage);
		pixman_region32_fini(&damage);
		screen.shown ^= 1;
		/* the other buffer now holds the last frame but one: its changed
		 * rows brought up to date, so both are the screen */
		memcpy(prev->data + (size_t)first * w, next->data + (size_t)first * w,
			(size_t)(last - first + 1) * w * 4);
	}
	wm_place();

	/* the pointer: the output's cursor, with RISC OS's image and hot
	 * spot, where RISC OS puts it.  wlroots gives it the card's cursor
	 * plane if there is one -- virtio-gpu's, which QEMU's window draws
	 * at the Mac's own mouse -- so a move redraws nothing; else it is
	 * drawn over the frame, as the desktop's node was */
	struct wlr_cursor *cursor = screen.server->cursor;
	if (b->ptr_shown && b->ptr_image &&
			b->ptr_image + ROS_POINTER * ROS_POINTER * 4 <= screen.size - ROS_SCREEN_BLOCK) {
		int hx = (int)(b->ptr_hot & 0xFFFF), hy = (int)(b->ptr_hot >> 16);
		if (b->ptr_shape != screen.ptr_shape || !screen.ptr_set) {
			struct pixbuf *p = pixbuf_new(ROS_POINTER, ROS_POINTER, DRM_FORMAT_ARGB8888);
			if (p) {
				memcpy(p->data, screen.map + b->ptr_image, ROS_POINTER * ROS_POINTER * 4);
				wlr_cursor_set_buffer(cursor, &p->base, hx, hy, 1.0f);
				wlr_buffer_drop(&p->base);
				screen.ptr_shape = b->ptr_shape;
				screen.ptr_set = 1;
			}
		}
		int lx = ox + b->ptr_x + hx, ly = oy + b->ptr_y + hy;
		if (lx != screen.ptr_lx || ly != screen.ptr_ly) {
			wlr_cursor_warp(cursor, NULL, lx, ly);
			screen.ptr_lx = lx, screen.ptr_ly = ly;
		}
	} else if (screen.ptr_set) {
		wlr_cursor_unset_image(cursor);
		screen.ptr_set = 0;
	}
	return 0;
}

/* ---- ROSGD: the Wimp as window manager ------------------------------------
 *
 * Each Linux program's toplevel is a Wimp window (design 29 G5, step 3).
 * A RISC OS task, WaylandWindows (rosgd/modules/waylandwin), connects to
 * the socket /dev/rosgd-wm and makes the windows; the Wimp says where
 * they are and what of them is seen in the screen block's table, read
 * above.  Each toplevel is placed with its top left where its window's
 * work area is, under the desktop, which is clear where it is seen.
 *
 * The socket carries lines of text.  From here:
 *     new ID WIDTH HEIGHT PID TITLE  a toplevel mapped, its size in pixels,
 *                                    its client's process
 *     title ID TITLE
 *     size ID WIDTH HEIGHT           the program chose a new size
 *     gone ID                        unmapped
 * From RISC OS:
 *     configure ID WIDTH HEIGHT      the window's work area, a new size
 *     close ID                       its close icon
 *     focus ID                       the keyboard to it (0: to none)
 *     motion ID X Y                  the pointer, in the work area's pixels
 *     button ID BUTTON 1|0           pressed or released (Linux's BTN_*)
 *     axis ID STEPS                  the scroll wheel, down positive
 *     leave ID                       the pointer has left it
 *     key ID CODE                    a RISC OS key code (Wimp_Poll's)
 * Input is RISC OS's: there is no libinput here (step 2); the pointer and
 * the keyboard are passed on from the Wimp. */

static struct {
	struct tinywl_server *server;
	const char *path;
	int listen_fd, fd;
	struct wl_event_source *listen_src, *src;
	char in[4096];
	size_t inlen;
	struct wlr_keyboard keyboard;
	struct xkb_keymap *keymap;
	xkb_mod_mask_t shift, ctrl;
	/* RISC OS's characters, as a key and whether shift is down */
	uint8_t code[128];
	bool shifted[128];
} wm = { .listen_fd = -1, .fd = -1 };

static void wm_send(const char *fmt, ...) {
	if (wm.fd < 0) {
		return;
	}
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	if (send(wm.fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) != n) {
		wlr_log(WLR_ERROR, "ROSGD: rosgd-wm: short send");
	}
}

static void wm_geometry(struct tinywl_toplevel *t, int *w, int *h) {
	struct wlr_box *g = &t->xdg_toplevel->base->geometry;
	*w = g->width > 0 ? g->width : t->xdg_toplevel->base->surface->current.width;
	*h = g->height > 0 ? g->height : t->xdg_toplevel->base->surface->current.height;
}

static const char *wm_title_of(struct tinywl_toplevel *t) {
	const char *title = t->xdg_toplevel->title;
	if (!title || !*title) {
		title = t->xdg_toplevel->app_id;
	}
	return title && *title ? title : "Linux";
}

static void wm_new(struct tinywl_toplevel *t) {
	int w, h;
	wm_geometry(t, &w, &h);
	t->sent_w = w, t->sent_h = h;
	/* the client's process, so RISC OS can know a program of its own
	 * (rosgd-browser's control socket is under its pid) */
	pid_t pid = 0;
	struct wl_resource *res = t->xdg_toplevel->base->surface->resource;
	if (res) {
		uid_t uid;
		gid_t gid;
		wl_client_get_credentials(wl_resource_get_client(res), &pid, &uid, &gid);
	}
	wm_send("new %u %d %d %d %s", t->id, w, h, (int)pid, wm_title_of(t));
}

static void wm_gone(struct tinywl_toplevel *t) {
	wlr_scene_node_set_enabled(&t->scene_tree->node, false);
	wm_send("gone %u", t->id);
}

static void wm_title(struct tinywl_toplevel *t) {
	wm_send("title %u %s", t->id, wm_title_of(t));
}

static void wm_size(struct tinywl_toplevel *t) {
	int w, h;
	wm_geometry(t, &w, &h);
	if (w != t->sent_w || h != t->sent_h) {
		t->sent_w = w, t->sent_h = h;
		wm_send("size %u %d %d", t->id, w, h);
	}
}

static struct tinywl_toplevel *wm_find(uint32_t id) {
	struct tinywl_toplevel *t;
	wl_list_for_each(t, &wm.server->toplevels, link) {
		if (t->id == id) {
			return t;
		}
	}
	return NULL;
}

/* each toplevel where the Wimp's table says, or out of sight */
static void wm_place(void) {
	struct tinywl_toplevel *t;
	wl_list_for_each(t, &wm.server->toplevels, link) {
		bool shown = false;
		for (uint32_t i = 0; i < screen.ext.nwindows; i++) {
			if (screen.ext.window[i].id != t->id) {
				continue;
			}
			struct wlr_box *g = &t->xdg_toplevel->base->geometry;
			wlr_scene_node_set_position(&t->scene_tree->node,
				screen.ox + screen.ext.window[i].x - g->x,
				screen.oy + screen.ext.window[i].y - g->y);
			shown = true;
		}
		wlr_scene_node_set_enabled(&t->scene_tree->node, shown);
		wlr_scene_node_set_position(&t->popups->node, t->scene_tree->node.x, t->scene_tree->node.y);
		wlr_scene_node_set_enabled(&t->popups->node, shown);
	}
}

static uint32_t wm_msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/* the surface under a point of a toplevel's window, in the window's
 * pixels; NULL if none */
static struct wlr_surface *wm_surface_at(struct tinywl_toplevel *t, int x, int y, double *sx, double *sy) {
	struct wlr_box *g = &t->xdg_toplevel->base->geometry;
	double lx = t->scene_tree->node.x + g->x + x, ly = t->scene_tree->node.y + g->y + y;
	struct wlr_scene_node *node = wlr_scene_node_at(&t->popups->node, lx, ly, sx, sy);
	if (!node) {
		node = wlr_scene_node_at(&t->scene_tree->node, lx, ly, sx, sy);
	}
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	return ss ? ss->surface : NULL;
}

static void wm_modifiers(xkb_mod_mask_t mask) {
	struct wlr_keyboard_modifiers mods = { .depressed = mask };
	wlr_seat_keyboard_notify_modifiers(wm.server->seat, &mods);
}

static void wm_tap(uint32_t keycode, xkb_mod_mask_t mask) {
	struct wlr_seat *seat = wm.server->seat;
	if (mask) {
		wm_modifiers(mask);
	}
	wlr_seat_keyboard_notify_key(seat, wm_msec(), keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(seat, wm_msec(), keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
	if (mask) {
		wm_modifiers(0);
	}
}

/* evdev's codes (linux/input-event-codes.h) */
enum {
	K_ESC = 1, K_BACKSPACE = 14, K_TAB = 15, K_ENTER = 28, K_HOME = 102, K_UP = 103,
	K_PAGEUP = 104, K_LEFT = 105, K_RIGHT = 106, K_END = 107, K_DOWN = 108,
	K_PAGEDOWN = 109, K_INSERT = 110, K_DELETE = 111, K_F1 = 59,
};

/* a RISC OS key code, as the key that makes it */
static void wm_key(uint32_t code) {
	uint32_t key = 0;
	xkb_mod_mask_t mask = 0;
	switch (code) {
	case 8: key = K_BACKSPACE; break;
	case 9: key = K_TAB; break;
	case 13: key = K_ENTER; break;
	case 27: key = K_ESC; break;
	case 0x7f: key = K_DELETE; break;
	case 0x1e: key = K_HOME; break;
	case 0x18b: key = K_END; break;
	case 0x1cd: key = K_INSERT; break;
	case 0x18c: key = K_LEFT; break;
	case 0x18d: key = K_RIGHT; break;
	case 0x18e: key = K_DOWN; break;
	case 0x18f: key = K_UP; break;
	case 0x19e: key = K_PAGEDOWN; break;
	case 0x19f: key = K_PAGEUP; break;
	case 0x19c: key = K_LEFT; mask = wm.shift; break;
	case 0x19d: key = K_RIGHT; mask = wm.shift; break;
	case 0x1ac: key = K_LEFT; mask = wm.ctrl; break;
	case 0x1ad: key = K_RIGHT; mask = wm.ctrl; break;
	case 0x1ae: key = K_DOWN; mask = wm.ctrl; break;
	case 0x1af: key = K_UP; mask = wm.ctrl; break;
	default:
		if (code >= 0x181 && code <= 0x189) {
			key = K_F1 + code - 0x181;              /* F1-F9 */
		} else if (code == 0x1ca) {
			key = 68;                               /* F10 */
		} else if (code == 0x1cb || code == 0x1cc) {
			key = 87 + code - 0x1cb;                /* F11, F12 */
		} else if (code >= 1 && code <= 26) {
			key = wm.code['a' + code - 1];          /* CTRL and a letter */
			mask = wm.ctrl;
		} else if (code < 128 && wm.code[code]) {
			key = wm.code[code];
			mask = wm.shifted[code] ? wm.shift : 0;
		}
		break;
	}
	if (key) {
		wm_tap(key, mask);
	}
}

static void wm_command(char *line) {
	char cmd[16];
	unsigned id = 0;
	int a = 0, b = 0, n = 0;
	if (sscanf(line, "%15s %u%n", cmd, &id, &n) < 2) {
		return;
	}
	if (strcmp(cmd, "motion") != 0) {
		wlr_log(WLR_DEBUG, "ROSGD: rosgd-wm: %s", line);
	}
	struct tinywl_toplevel *t = id ? wm_find(id) : NULL;
	struct wlr_seat *seat = wm.server->seat;
	const char *rest = line + n;
	if (strcmp(cmd, "focus") == 0) {
		if (!t) {
			wlr_seat_keyboard_notify_clear_focus(seat);
			return;
		}
		wlr_xdg_toplevel_set_activated(t->xdg_toplevel, true);
		struct wlr_keyboard_modifiers mods = {0};
		wlr_seat_keyboard_notify_enter(seat, t->xdg_toplevel->base->surface, NULL, 0, &mods);
		return;
	}
	if (!t) {
		return;
	}
	if (strcmp(cmd, "configure") == 0 && sscanf(rest, "%d %d", &a, &b) == 2 && a > 0 && b > 0) {
		wlr_xdg_toplevel_set_size(t->xdg_toplevel, a, b);
	} else if (strcmp(cmd, "close") == 0) {
		wlr_xdg_toplevel_send_close(t->xdg_toplevel);
	} else if (strcmp(cmd, "motion") == 0 && sscanf(rest, "%d %d", &a, &b) == 2) {
		double sx, sy;
		struct wlr_surface *surface = wm_surface_at(t, a, b, &sx, &sy);
		if (surface) {
			wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(seat, wm_msec(), sx, sy);
		} else {
			wlr_seat_pointer_clear_focus(seat);
		}
		wlr_seat_pointer_notify_frame(seat);
	} else if (strcmp(cmd, "button") == 0 && sscanf(rest, "%d %d", &a, &b) == 2) {
		wlr_seat_pointer_notify_button(seat, wm_msec(), (uint32_t)a,
			b ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
		wlr_seat_pointer_notify_frame(seat);
	} else if (strcmp(cmd, "axis") == 0 && sscanf(rest, "%d", &a) == 1) {
		wlr_seat_pointer_notify_axis(seat, wm_msec(), WL_POINTER_AXIS_VERTICAL_SCROLL,
			15.0 * a, 120 * a, WL_POINTER_AXIS_SOURCE_WHEEL,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
		wlr_seat_pointer_notify_frame(seat);
	} else if (strcmp(cmd, "leave") == 0) {
		wlr_seat_pointer_clear_focus(seat);
	} else if (strcmp(cmd, "key") == 0 && sscanf(rest, "%d", &a) == 1) {
		wm_key((uint32_t)a);
	}
}

static void wm_drop(void) {
	if (wm.src) {
		wl_event_source_remove(wm.src);
		wm.src = NULL;
	}
	if (wm.fd >= 0) {
		close(wm.fd);
		wm.fd = -1;
	}
	wm.inlen = 0;
}

static int wm_readable(int fd, uint32_t mask, void *data) {
	ssize_t n = recv(fd, wm.in + wm.inlen, sizeof(wm.in) - 1 - wm.inlen, MSG_DONTWAIT);
	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
			wlr_log(WLR_INFO, "ROSGD: rosgd-wm: RISC OS has gone");
			wm_drop();
		}
		return 0;
	}
	wm.inlen += (size_t)n;
	char *start = wm.in, *nl;
	while ((nl = memchr(start, '\n', wm.inlen - (size_t)(start - wm.in))) != NULL) {
		*nl = 0;
		wm_command(start);
		start = nl + 1;
		if (wm.fd < 0) {
			return 0;
		}
	}
	wm.inlen -= (size_t)(start - wm.in);
	memmove(wm.in, start, wm.inlen);
	if (wm.inlen == sizeof(wm.in) - 1) {
		wm.inlen = 0;                   /* a line too long: dropped */
	}
	return 0;
}

static int wm_accept(int fd, uint32_t mask, void *data) {
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (c < 0) {
		return 0;
	}
	wm_drop();                              /* one RISC OS: the newest */
	wm.fd = c;
	wm.src = wl_event_loop_add_fd(wl_display_get_event_loop(wm.server->wl_display), c,
		WL_EVENT_READABLE, wm_readable, NULL);
	wlr_log(WLR_INFO, "ROSGD: rosgd-wm: RISC OS is the window manager");
	struct tinywl_toplevel *t;
	wl_list_for_each_reverse(t, &wm.server->toplevels, link) {
		wm_new(t);
	}
	return 0;
}

static void wm_keyboard_init(void) {
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	wm.keymap = ctx ? xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	if (!wm.keymap) {
		wlr_log(WLR_ERROR, "ROSGD: no keymap");
		return;
	}
	static const struct wlr_keyboard_impl impl = { .name = "rosgd-keyboard" };
	wlr_keyboard_init(&wm.keyboard, &impl, "RISC OS");
	wlr_keyboard_set_keymap(&wm.keyboard, wm.keymap);
	wlr_seat_set_keyboard(wm.server->seat, &wm.keyboard);
	wm.shift = 1u << xkb_keymap_mod_get_index(wm.keymap, XKB_MOD_NAME_SHIFT);
	wm.ctrl = 1u << xkb_keymap_mod_get_index(wm.keymap, XKB_MOD_NAME_CTRL);
	/* each character, as the first key that makes it, plain or shifted */
	struct xkb_state *st = xkb_state_new(wm.keymap);
	for (int level = 1; level >= 0; level--) {
		xkb_state_update_mask(st, level ? wm.shift : 0, 0, 0, 0, 0, 0);
		/* down from the top, so the lowest key wins; evdev's codes past
		 * 255 (the keymap goes to 708) are not wanted */
		xkb_keycode_t top = xkb_keymap_max_keycode(wm.keymap);
		for (xkb_keycode_t k = top < 255 + 8 ? top : 255 + 8; k >= 8; k--) {
			uint32_t c = xkb_state_key_get_utf32(st, k);
			if (c >= 32 && c < 127) {
				wm.code[c] = (uint8_t)(k - 8);
				wm.shifted[c] = level != 0;
			}
		}
	}
	xkb_state_unref(st);
}

static void wm_decoration_destroy(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *t = wl_container_of(listener, t, decoration_destroy);
	wl_list_remove(&t->decoration_destroy.link);
	t->decoration = NULL;
}

/* a program that can leave its window's frame to the window manager is
 * asked to: the Wimp's furniture is the frame */
static void wm_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *d = data;
	struct wlr_scene_tree *tree = d->toplevel->base->data;
	struct tinywl_toplevel *t = tree ? tree->node.data : NULL;
	if (!t || t->decoration) {
		return;
	}
	t->decoration = d;
	t->decoration_destroy.notify = wm_decoration_destroy;
	wl_signal_add(&d->events.destroy, &t->decoration_destroy);
	if (d->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static struct wl_listener wm_decoration_listener = { .notify = wm_new_decoration };

static void wm_start(struct tinywl_server *server) {
	wm.server = server;
	wm.path = getenv("ROSGD_WM") ? getenv("ROSGD_WM") : "/dev/rosgd-wm";
	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	struct wlr_xdg_decoration_manager_v1 *deco = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	if (deco) {
		wl_signal_add(&deco->events.new_toplevel_decoration, &wm_decoration_listener);
	}
	wm_keyboard_init();
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", wm.path);
	unlink(wm.path);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
		wlr_log(WLR_ERROR, "ROSGD: %s: %s", wm.path, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	wm.listen_fd = fd;
	wm.listen_src = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), fd,
		WL_EVENT_READABLE, wm_accept, NULL);
}

/* the windows' tree first, then the desktop's node, so that every Wayland
 * window is under the desktop, and seen only where it is clear */
static void screen_start(struct tinywl_server *server) {
	screen.server = server;
	screen.path = getenv("ROSGD_SCREEN") ? getenv("ROSGD_SCREEN") : "/dev/rosgd-screen";
	wm_windows = wlr_scene_tree_create(&server->scene->tree);
	screen.desktop = wlr_scene_buffer_create(&server->scene->tree, NULL);
	wlr_scene_buffer_set_filter_mode(screen.desktop, WLR_SCALE_FILTER_NEAREST);
	wm_popups = wlr_scene_tree_create(&server->scene->tree);
	screen.timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->wl_display),
		screen_tick, NULL);
	wl_event_source_timer_update(screen.timer, 20);
}

int main(int argc, char *argv[]) {
	char *startup_cmd = NULL;
	bool box = false;

	int c;
	while ((c = getopt(argc, argv, "bps:h")) != -1) {
		switch (c) {
		case 'b':
			box = true;
			break;
		case 'p':
			/* ROSGD: the pointer drawn into the frame -- for a card
			 * whose host shows no cursor plane (VZ's) */
			setenv("WLR_NO_HARDWARE_CURSORS", "1", 1);
			break;
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-b] [-p] [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-b] [-s startup command]\n", argv[0]);
		return 0;
	}
	/* ROSGD: -b, as the box starts it (WaylandWindows, through wperun -d):
	 * the display device its own, no seat manager, no libinput (input is
	 * RISC OS's), the CPU renderer until G5's GL ES; its log in /run */
	ros_box = box;
	if (box) {
		setenv("WLR_BACKENDS", "drm", 0);
		setenv("WLR_RENDERER", "pixman", 0);
		setenv("LIBSEAT_BACKEND", "noop", 0);
		if (!freopen("/run/rosgd-compositor.log", "w", stderr)) {
			/* stderr stays where it was */
		}
		wlr_log_init(WLR_INFO, NULL);
	} else {
		wlr_log_init(WLR_DEBUG, NULL);
	}

	struct tinywl_server server = {0};
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, managing Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces, the subcompositor allows to
	 * assign the role of subsurfaces to surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create(server.wl_display);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	/* ROSGD: what is composited can be captured (grim, the tests), and
	 * the outputs' place and size known to it (xdg-output) */
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	/* ROSGD: the RISC OS screen as the bottom layer, its pointer on top */
	screen_start(&server);

	/* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
	 * used for application windows. For more detail on shells, refer to
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html.
	 */
	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
			&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	/* ROSGD: RISC OS's window manager task, and its input */
	wm_start(&server);

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server.wl_display);


	/* Once wl_display_run returns, we destroy all clients then shut down the
	 * server. */
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);

	wl_list_remove(&server.new_output.link);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	return 0;
}
