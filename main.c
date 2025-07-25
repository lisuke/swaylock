#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wordexp.h>
#include "background-image.h"
#include "cairo.h"
#include "comm.h"
#include "log.h"
#include "loop.h"
#include "password-buffer.h"
#include "pool-buffer.h"
#include "seat.h"
#include "swaylock.h"
#include "ext-session-lock-v1-client-protocol.h"

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		swaylock_log(LOG_DEBUG, "Invalid color %s, defaulting to 0xFFFFFFFF",
				color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

int lenient_strcmp(char *a, char *b) {
	if (a == b) {
		return 0;
	} else if (!a) {
		return -1;
	} else if (!b) {
		return 1;
	} else {
		return strcmp(a, b);
	}
}

static void daemonize(void) {
	int fds[2];
	if (pipe(fds) != 0) {
		swaylock_log(LOG_ERROR, "Failed to pipe");
		exit(1);
	}
	if (fork() == 0) {
		setsid();
		close(fds[0]);
		int devnull = open("/dev/null", O_RDWR);
		dup2(STDOUT_FILENO, devnull);
		dup2(STDERR_FILENO, devnull);
		close(devnull);
		uint8_t success = 0;
		if (chdir("/") != 0) {
			write(fds[1], &success, 1);
			exit(1);
		}
		success = 1;
		if (write(fds[1], &success, 1) != 1) {
			exit(1);
		}
		close(fds[1]);
	} else {
		close(fds[1]);
		uint8_t success;
		if (read(fds[0], &success, 1) != 1 || !success) {
			swaylock_log(LOG_ERROR, "Failed to daemonize");
			exit(1);
		}
		close(fds[0]);
		exit(0);
	}
}

static void destroy_surface(struct swaylock_surface *surface) {
	wl_list_remove(&surface->link);
	if (surface->ext_session_lock_surface_v1 != NULL) {
		ext_session_lock_surface_v1_destroy(surface->ext_session_lock_surface_v1);
	}
	if (surface->subsurface) {
		wl_subsurface_destroy(surface->subsurface);
	}
	if (surface->child) {
		wl_surface_destroy(surface->child);
	}
	if (surface->surface != NULL) {
		wl_surface_destroy(surface->surface);
	}
	destroy_buffer(&surface->indicator_buffers[0]);
	destroy_buffer(&surface->indicator_buffers[1]);
	wl_output_release(surface->output);
	free(surface);
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener;

static cairo_surface_t *select_image(struct swaylock_state *state,
		struct swaylock_surface *surface);

static bool surface_is_opaque(struct swaylock_surface *surface) {
	if (surface->image) {
		return cairo_surface_get_content(surface->image) == CAIRO_CONTENT_COLOR;
	}
	return (surface->state->args.colors.background & 0xff) == 0xff;
}

static void create_surface(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;

	surface->image = select_image(state, surface);

	surface->surface = wl_compositor_create_surface(state->compositor);
	assert(surface->surface);

	surface->child = wl_compositor_create_surface(state->compositor);
	assert(surface->child);
	surface->subsurface = wl_subcompositor_get_subsurface(state->subcompositor, surface->child, surface->surface);
	assert(surface->subsurface);
	wl_subsurface_set_sync(surface->subsurface);

	surface->ext_session_lock_surface_v1 = ext_session_lock_v1_get_lock_surface(
		state->ext_session_lock_v1, surface->surface, surface->output);
	ext_session_lock_surface_v1_add_listener(surface->ext_session_lock_surface_v1,
		&ext_session_lock_surface_v1_listener, surface);

	if (surface_is_opaque(surface) &&
			surface->state->args.mode != BACKGROUND_MODE_CENTER &&
			surface->state->args.mode != BACKGROUND_MODE_FIT) {
		struct wl_region *region =
			wl_compositor_create_region(surface->state->compositor);
		wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_set_opaque_region(surface->surface, region);
		wl_region_destroy(region);
	}

	surface->created = true;
}

static void ext_session_lock_surface_v1_handle_configure(void *data,
		struct ext_session_lock_surface_v1 *lock_surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct swaylock_surface *surface = data;
	surface->width = width;
	surface->height = height;
	ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
	surface->dirty = true;
	render(surface);
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener = {
	.configure = ext_session_lock_surface_v1_handle_configure,
};

void damage_state(struct swaylock_state *state) {
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		surface->dirty = true;
		render(surface);
	}
}

static void handle_wl_output_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t width_mm, int32_t height_mm,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct swaylock_surface *surface = data;
	surface->subpixel = subpixel;
	if (surface->state->run_display) {
		surface->dirty = true;
		render(surface);
	}
}

static void handle_wl_output_mode(void *data, struct wl_output *output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

static void handle_wl_output_done(void *data, struct wl_output *output) {
	struct swaylock_surface *surface = data;
	if (!surface->created && surface->state->run_display) {
		create_surface(surface);
	}
}

static void handle_wl_output_scale(void *data, struct wl_output *output,
		int32_t factor) {
	struct swaylock_surface *surface = data;
	surface->scale = factor;
	if (surface->state->run_display) {
		surface->dirty = true;
		render(surface);
	}
}

static void handle_wl_output_name(void *data, struct wl_output *output,
		const char *name) {
	struct swaylock_surface *surface = data;
	surface->output_name = strdup(name);
}

static void handle_wl_output_description(void *data, struct wl_output *output,
		const char *description) {
	// Who cares
}

struct wl_output_listener _wl_output_listener = {
	.geometry = handle_wl_output_geometry,
	.mode = handle_wl_output_mode,
	.done = handle_wl_output_done,
	.scale = handle_wl_output_scale,
	.name = handle_wl_output_name,
	.description = handle_wl_output_description,
};

static void ext_session_lock_v1_handle_locked(void *data, struct ext_session_lock_v1 *lock) {
	struct swaylock_state *state = data;
	state->locked = true;
}

static void ext_session_lock_v1_handle_finished(void *data, struct ext_session_lock_v1 *lock) {
	swaylock_log(LOG_ERROR, "Failed to lock session -- "
			"is another lockscreen running?");
	exit(2);
}

static const struct ext_session_lock_v1_listener ext_session_lock_v1_listener = {
	.locked = ext_session_lock_v1_handle_locked,
	.finished = ext_session_lock_v1_handle_finished,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaylock_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		state->subcompositor = wl_registry_bind(registry, name,
				&wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name,
				&wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat = wl_registry_bind(
				registry, name, &wl_seat_interface, 4);
		struct swaylock_seat *swaylock_seat =
			calloc(1, sizeof(struct swaylock_seat));
		swaylock_seat->state = state;
		wl_seat_add_listener(seat, &seat_listener, swaylock_seat);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaylock_surface *surface =
			calloc(1, sizeof(struct swaylock_surface));
		surface->state = state;
		surface->output = wl_registry_bind(registry, name,
				&wl_output_interface, 4);
		surface->output_global_name = name;
		wl_output_add_listener(surface->output, &_wl_output_listener, surface);
		wl_list_insert(&state->surfaces, &surface->link);
	} else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
		state->ext_session_lock_manager_v1 = wl_registry_bind(registry, name,
				&ext_session_lock_manager_v1_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct swaylock_state *state = data;
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		if (surface->output_global_name == name) {
			destroy_surface(surface);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static int sigusr_fds[2] = {-1, -1};

void do_sigusr(int sig) {
	(void)write(sigusr_fds[1], "1", 1);
}

static cairo_surface_t *select_image(struct swaylock_state *state,
		struct swaylock_surface *surface) {
	struct swaylock_image *image;
	cairo_surface_t *default_image = NULL;
	wl_list_for_each(image, &state->images, link) {
		if (lenient_strcmp(image->output_name, surface->output_name) == 0) {
			return image->cairo_surface;
		} else if (!image->output_name) {
			default_image = image->cairo_surface;
		}
	}
	return default_image;
}

static char *join_args(char **argv, int argc) {
	assert(argc > 0);
	int len = 0, i;
	for (i = 0; i < argc; ++i) {
		len += strlen(argv[i]) + 1;
	}
	char *res = malloc(len);
	len = 0;
	for (i = 0; i < argc; ++i) {
		strcpy(res + len, argv[i]);
		len += strlen(argv[i]);
		res[len++] = ' ';
	}
	res[len - 1] = '\0';
	return res;
}

static void load_image(char *arg, struct swaylock_state *state) {
	// [[<output>]:]<path>
	struct swaylock_image *image = calloc(1, sizeof(struct swaylock_image));
	char *separator = strchr(arg, ':');
	if (separator) {
		*separator = '\0';
		image->output_name = separator == arg ? NULL : strdup(arg);
		image->path = strdup(separator + 1);
	} else {
		image->output_name = NULL;
		image->path = strdup(arg);
	}

	struct swaylock_image *iter_image, *temp;
	wl_list_for_each_safe(iter_image, temp, &state->images, link) {
		if (lenient_strcmp(iter_image->output_name, image->output_name) == 0) {
			if (image->output_name) {
				swaylock_log(LOG_DEBUG,
						"Replacing image defined for output %s with %s",
						image->output_name, image->path);
			} else {
				swaylock_log(LOG_DEBUG, "Replacing default image with %s",
						image->path);
			}
			wl_list_remove(&iter_image->link);
			free(iter_image->cairo_surface);
			free(iter_image->output_name);
			free(iter_image->path);
			free(iter_image);
			break;
		}
	}

	// The shell will not expand ~ to the value of $HOME when an output name is
	// given. Also, any image paths given in the config file need to have shell
	// expansions performed
	wordexp_t p;
	while (strstr(image->path, "  ")) {
		image->path = realloc(image->path, strlen(image->path) + 2);
		char *ptr = strstr(image->path, "  ") + 1;
		memmove(ptr + 1, ptr, strlen(ptr) + 1);
		*ptr = '\\';
	}
	if (wordexp(image->path, &p, 0) == 0) {
		free(image->path);
		image->path = join_args(p.we_wordv, p.we_wordc);
		wordfree(&p);
	}

	// Load the actual image
	image->cairo_surface = load_background_image(image->path);
	if (!image->cairo_surface) {
		free(image);
		return;
	}
	wl_list_insert(&state->images, &image->link);
	swaylock_log(LOG_DEBUG, "Loaded image %s for output %s", image->path,
			image->output_name ? image->output_name : "*");
}

static void set_default_colors(struct swaylock_colors *colors) {
	colors->background = 0xA3A3A3FF;
	colors->bs_highlight = 0xDB3300FF;
	colors->key_highlight = 0x33DB00FF;
	colors->caps_lock_bs_highlight = 0xDB3300FF;
	colors->caps_lock_key_highlight = 0x33DB00FF;
	colors->separator = 0x000000FF;
	colors->layout_background = 0x000000C0;
	colors->layout_border = 0x00000000;
	colors->layout_text = 0xFFFFFFFF;
	colors->inside = (struct swaylock_colorset){
		.input = 0x000000C0,
		.cleared = 0xE5A445C0,
		.caps_lock = 0x000000C0,
		.verifying = 0x0072FFC0,
		.wrong = 0xFA0000C0,
	};
	colors->line = (struct swaylock_colorset){
		.input = 0x000000FF,
		.cleared = 0x000000FF,
		.caps_lock = 0x000000FF,
		.verifying = 0x000000FF,
		.wrong = 0x000000FF,
	};
	colors->ring = (struct swaylock_colorset){
		.input = 0x337D00FF,
		.cleared = 0xE5A445FF,
		.caps_lock = 0xE5A445FF,
		.verifying = 0x3300FFFF,
		.wrong = 0x7D3300FF,
	};
	colors->text = (struct swaylock_colorset){
		.input = 0xE5A445FF,
		.cleared = 0x000000FF,
		.caps_lock = 0xE5A445FF,
		.verifying = 0x000000FF,
		.wrong = 0x000000FF,
	};
}

enum line_mode {
	LM_LINE,
	LM_INSIDE,
	LM_RING,
};

static int parse_options(int argc, char **argv, struct swaylock_state *state,
		enum line_mode *line_mode, char **config_path) {
	enum long_option_codes {
		LO_BS_HL_COLOR = 256,
		LO_CAPS_LOCK_BS_HL_COLOR,
		LO_CAPS_LOCK_KEY_HL_COLOR,
		LO_FONT,
		LO_FONT_SIZE,
		LO_IND_IDLE_VISIBLE,
		LO_IND_RADIUS,
		LO_IND_X_POSITION,
		LO_IND_Y_POSITION,
		LO_IND_THICKNESS,
		LO_INSIDE_COLOR,
		LO_INSIDE_CLEAR_COLOR,
		LO_INSIDE_CAPS_LOCK_COLOR,
		LO_INSIDE_VER_COLOR,
		LO_INSIDE_WRONG_COLOR,
		LO_KEY_HL_COLOR,
		LO_LAYOUT_TXT_COLOR,
		LO_LAYOUT_BG_COLOR,
		LO_LAYOUT_BORDER_COLOR,
		LO_LINE_COLOR,
		LO_LINE_CLEAR_COLOR,
		LO_LINE_CAPS_LOCK_COLOR,
		LO_LINE_VER_COLOR,
		LO_LINE_WRONG_COLOR,
		LO_RING_COLOR,
		LO_RING_CLEAR_COLOR,
		LO_RING_CAPS_LOCK_COLOR,
		LO_RING_VER_COLOR,
		LO_RING_WRONG_COLOR,
		LO_SEP_COLOR,
		LO_TEXT_COLOR,
		LO_TEXT_CLEAR_COLOR,
		LO_TEXT_CAPS_LOCK_COLOR,
		LO_TEXT_VER_COLOR,
		LO_TEXT_WRONG_COLOR,
	};

	static struct option long_options[] = {
		{"config", required_argument, NULL, 'C'},
		{"color", required_argument, NULL, 'c'},
		{"debug", no_argument, NULL, 'd'},
		{"ignore-empty-password", no_argument, NULL, 'e'},
		{"daemonize", no_argument, NULL, 'f'},
		{"ready-fd", required_argument, NULL, 'R'},
		{"help", no_argument, NULL, 'h'},
		{"image", required_argument, NULL, 'i'},
		{"disable-caps-lock-text", no_argument, NULL, 'L'},
		{"indicator-caps-lock", no_argument, NULL, 'l'},
		{"line-uses-inside", no_argument, NULL, 'n'},
		{"line-uses-ring", no_argument, NULL, 'r'},
		{"scaling", required_argument, NULL, 's'},
		{"tiling", no_argument, NULL, 't'},
		{"no-unlock-indicator", no_argument, NULL, 'u'},
		{"show-keyboard-layout", no_argument, NULL, 'k'},
		{"hide-keyboard-layout", no_argument, NULL, 'K'},
		{"show-failed-attempts", no_argument, NULL, 'F'},
		{"version", no_argument, NULL, 'v'},
		{"bs-hl-color", required_argument, NULL, LO_BS_HL_COLOR},
		{"caps-lock-bs-hl-color", required_argument, NULL, LO_CAPS_LOCK_BS_HL_COLOR},
		{"caps-lock-key-hl-color", required_argument, NULL, LO_CAPS_LOCK_KEY_HL_COLOR},
		{"font", required_argument, NULL, LO_FONT},
		{"font-size", required_argument, NULL, LO_FONT_SIZE},
		{"indicator-idle-visible", no_argument, NULL, LO_IND_IDLE_VISIBLE},
		{"indicator-radius", required_argument, NULL, LO_IND_RADIUS},
		{"indicator-thickness", required_argument, NULL, LO_IND_THICKNESS},
		{"indicator-x-position", required_argument, NULL, LO_IND_X_POSITION},
		{"indicator-y-position", required_argument, NULL, LO_IND_Y_POSITION},
		{"inside-color", required_argument, NULL, LO_INSIDE_COLOR},
		{"inside-clear-color", required_argument, NULL, LO_INSIDE_CLEAR_COLOR},
		{"inside-caps-lock-color", required_argument, NULL, LO_INSIDE_CAPS_LOCK_COLOR},
		{"inside-ver-color", required_argument, NULL, LO_INSIDE_VER_COLOR},
		{"inside-wrong-color", required_argument, NULL, LO_INSIDE_WRONG_COLOR},
		{"key-hl-color", required_argument, NULL, LO_KEY_HL_COLOR},
		{"layout-bg-color", required_argument, NULL, LO_LAYOUT_BG_COLOR},
		{"layout-border-color", required_argument, NULL, LO_LAYOUT_BORDER_COLOR},
		{"layout-text-color", required_argument, NULL, LO_LAYOUT_TXT_COLOR},
		{"line-color", required_argument, NULL, LO_LINE_COLOR},
		{"line-clear-color", required_argument, NULL, LO_LINE_CLEAR_COLOR},
		{"line-caps-lock-color", required_argument, NULL, LO_LINE_CAPS_LOCK_COLOR},
		{"line-ver-color", required_argument, NULL, LO_LINE_VER_COLOR},
		{"line-wrong-color", required_argument, NULL, LO_LINE_WRONG_COLOR},
		{"ring-color", required_argument, NULL, LO_RING_COLOR},
		{"ring-clear-color", required_argument, NULL, LO_RING_CLEAR_COLOR},
		{"ring-caps-lock-color", required_argument, NULL, LO_RING_CAPS_LOCK_COLOR},
		{"ring-ver-color", required_argument, NULL, LO_RING_VER_COLOR},
		{"ring-wrong-color", required_argument, NULL, LO_RING_WRONG_COLOR},
		{"separator-color", required_argument, NULL, LO_SEP_COLOR},
		{"text-color", required_argument, NULL, LO_TEXT_COLOR},
		{"text-clear-color", required_argument, NULL, LO_TEXT_CLEAR_COLOR},
		{"text-caps-lock-color", required_argument, NULL, LO_TEXT_CAPS_LOCK_COLOR},
		{"text-ver-color", required_argument, NULL, LO_TEXT_VER_COLOR},
		{"text-wrong-color", required_argument, NULL, LO_TEXT_WRONG_COLOR},
		{0, 0, 0, 0}
	};

	const char usage[] =
		"Usage: swaylock [options...]\n"
		"\n"
		"  -C, --config <config_file>       "
			"Path to the config file.\n"
		"  -c, --color <color>              "
			"Turn the screen into the given color instead of light gray.\n"
		"  -d, --debug                      "
			"Enable debugging output.\n"
		"  -e, --ignore-empty-password      "
			"When an empty password is provided, do not validate it.\n"
		"  -F, --show-failed-attempts       "
			"Show current count of failed authentication attempts.\n"
		"  -f, --daemonize                  "
			"Detach from the controlling terminal after locking.\n"
		"  -R, --ready-fd <fd>              "
			"File descriptor to send readiness notifications to.\n"
		"  -h, --help                       "
			"Show help message and quit.\n"
		"  -i, --image [[<output>]:]<path>  "
			"Display the given image, optionally only on the given output.\n"
		"  -k, --show-keyboard-layout       "
			"Display the current xkb layout while typing.\n"
		"  -K, --hide-keyboard-layout       "
			"Hide the current xkb layout while typing.\n"
		"  -L, --disable-caps-lock-text     "
			"Disable the Caps Lock text.\n"
		"  -l, --indicator-caps-lock        "
			"Show the current Caps Lock state also on the indicator.\n"
		"  -s, --scaling <mode>             "
			"Image scaling mode: stretch, fill, fit, center, tile, solid_color.\n"
		"  -t, --tiling                     "
			"Same as --scaling=tile.\n"
		"  -u, --no-unlock-indicator        "
			"Disable the unlock indicator.\n"
		"  -v, --version                    "
			"Show the version number and quit.\n"
		"  --bs-hl-color <color>            "
			"Sets the color of backspace highlight segments.\n"
		"  --caps-lock-bs-hl-color <color>  "
			"Sets the color of backspace highlight segments when Caps Lock "
			"is active.\n"
		"  --caps-lock-key-hl-color <color> "
			"Sets the color of the key press highlight segments when "
			"Caps Lock is active.\n"
		"  --font <font>                    "
			"Sets the font of the text.\n"
		"  --font-size <size>               "
			"Sets a fixed font size for the indicator text.\n"
		"  --indicator-idle-visible         "
			"Sets the indicator to show even if idle.\n"
		"  --indicator-radius <radius>      "
			"Sets the indicator radius.\n"
		"  --indicator-thickness <thick>    "
			"Sets the indicator thickness.\n"
		"  --indicator-x-position <x>       "
			"Sets the horizontal position of the indicator.\n"
		"  --indicator-y-position <y>       "
			"Sets the vertical position of the indicator.\n"
		"  --inside-color <color>           "
			"Sets the color of the inside of the indicator.\n"
		"  --inside-clear-color <color>     "
			"Sets the color of the inside of the indicator when cleared.\n"
		"  --inside-caps-lock-color <color> "
			"Sets the color of the inside of the indicator when Caps Lock "
			"is active.\n"
		"  --inside-ver-color <color>       "
			"Sets the color of the inside of the indicator when verifying.\n"
		"  --inside-wrong-color <color>     "
			"Sets the color of the inside of the indicator when invalid.\n"
		"  --key-hl-color <color>           "
			"Sets the color of the key press highlight segments.\n"
		"  --layout-bg-color <color>        "
			"Sets the background color of the box containing the layout text.\n"
		"  --layout-border-color <color>    "
			"Sets the color of the border of the box containing the layout text.\n"
		"  --layout-text-color <color>      "
			"Sets the color of the layout text.\n"
		"  --line-color <color>             "
			"Sets the color of the line between the inside and ring.\n"
		"  --line-clear-color <color>       "
			"Sets the color of the line between the inside and ring when "
			"cleared.\n"
		"  --line-caps-lock-color <color>   "
			"Sets the color of the line between the inside and ring when "
			"Caps Lock is active.\n"
		"  --line-ver-color <color>         "
			"Sets the color of the line between the inside and ring when "
			"verifying.\n"
		"  --line-wrong-color <color>       "
			"Sets the color of the line between the inside and ring when "
			"invalid.\n"
		"  -n, --line-uses-inside           "
			"Use the inside color for the line between the inside and ring.\n"
		"  -r, --line-uses-ring             "
			"Use the ring color for the line between the inside and ring.\n"
		"  --ring-color <color>             "
			"Sets the color of the ring of the indicator.\n"
		"  --ring-clear-color <color>       "
			"Sets the color of the ring of the indicator when cleared.\n"
		"  --ring-caps-lock-color <color>   "
			"Sets the color of the ring of the indicator when Caps Lock "
			"is active.\n"
		"  --ring-ver-color <color>         "
			"Sets the color of the ring of the indicator when verifying.\n"
		"  --ring-wrong-color <color>       "
			"Sets the color of the ring of the indicator when invalid.\n"
		"  --separator-color <color>        "
			"Sets the color of the lines that separate highlight segments.\n"
		"  --text-color <color>             "
			"Sets the color of the text.\n"
		"  --text-clear-color <color>       "
			"Sets the color of the text when cleared.\n"
		"  --text-caps-lock-color <color>   "
			"Sets the color of the text when Caps Lock is active.\n"
		"  --text-ver-color <color>         "
			"Sets the color of the text when verifying.\n"
		"  --text-wrong-color <color>       "
			"Sets the color of the text when invalid.\n"
		"\n"
		"All <color> options are of the form <rrggbb[aa]>.\n";

	int c;
	optind = 1;
	while (1) {
		int opt_idx = 0;
		c = getopt_long(argc, argv, "c:deFfhi:kKLlnrs:tuvC:R:", long_options,
				&opt_idx);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'C':
			if (config_path) {
				*config_path = strdup(optarg);
			}
			break;
		case 'c':
			if (state) {
				state->args.colors.background = parse_color(optarg);
			}
			break;
		case 'd':
			swaylock_log_init(LOG_DEBUG);
			break;
		case 'e':
			if (state) {
				state->args.ignore_empty = true;
			}
			break;
		case 'F':
			if (state) {
				state->args.show_failed_attempts = true;
			}
			break;
		case 'f':
			if (state) {
				state->args.daemonize = true;
			}
			break;
		case 'R':
			if (state) {
				state->args.ready_fd = strtol(optarg, NULL, 10);
			}
			break;
		case 'i':
			if (state) {
				load_image(optarg, state);
			}
			break;
		case 'k':
			if (state) {
				state->args.show_keyboard_layout = true;
			}
			break;
		case 'K':
			if (state) {
				state->args.hide_keyboard_layout = true;
			}
			break;
		case 'L':
			if (state) {
				state->args.show_caps_lock_text = false;
			}
			break;
		case 'l':
			if (state) {
				state->args.show_caps_lock_indicator = true;
			}
			break;
		case 'n':
			if (line_mode) {
				*line_mode = LM_INSIDE;
			}
			break;
		case 'r':
			if (line_mode) {
				*line_mode = LM_RING;
			}
			break;
		case 's':
			if (state) {
				state->args.mode = parse_background_mode(optarg);
				if (state->args.mode == BACKGROUND_MODE_INVALID) {
					return 1;
				}
			}
			break;
		case 't':
			if (state) {
				state->args.mode = BACKGROUND_MODE_TILE;
			}
			break;
		case 'u':
			if (state) {
				state->args.show_indicator = false;
			}
			break;
		case 'v':
			fprintf(stdout, "swaylock version " SWAYLOCK_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		case LO_BS_HL_COLOR:
			if (state) {
				state->args.colors.bs_highlight = parse_color(optarg);
			}
			break;
		case LO_CAPS_LOCK_BS_HL_COLOR:
			if (state) {
				state->args.colors.caps_lock_bs_highlight = parse_color(optarg);
			}
			break;
		case LO_CAPS_LOCK_KEY_HL_COLOR:
			if (state) {
				state->args.colors.caps_lock_key_highlight = parse_color(optarg);
			}
			break;
		case LO_FONT:
			if (state) {
				free(state->args.font);
				state->args.font = strdup(optarg);
			}
			break;
		case LO_FONT_SIZE:
			if (state) {
				state->args.font_size = atoi(optarg);
			}
			break;
		case LO_IND_IDLE_VISIBLE:
			if (state) {
				state->args.indicator_idle_visible = true;
			}
			break;
		case LO_IND_RADIUS:
			if (state) {
				state->args.radius = strtol(optarg, NULL, 0);
			}
			break;
		case LO_IND_THICKNESS:
			if (state) {
				state->args.thickness = strtol(optarg, NULL, 0);
			}
			break;
		case LO_IND_X_POSITION:
			if (state) {
				state->args.override_indicator_x_position = true;
				state->args.indicator_x_position = atoi(optarg);
			}
			break;
		case LO_IND_Y_POSITION:
			if (state) {
				state->args.override_indicator_y_position = true;
				state->args.indicator_y_position = atoi(optarg);
			}
			break;
		case LO_INSIDE_COLOR:
			if (state) {
				state->args.colors.inside.input = parse_color(optarg);
			}
			break;
		case LO_INSIDE_CLEAR_COLOR:
			if (state) {
				state->args.colors.inside.cleared = parse_color(optarg);
			}
			break;
		case LO_INSIDE_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.inside.caps_lock = parse_color(optarg);
			}
			break;
		case LO_INSIDE_VER_COLOR:
			if (state) {
				state->args.colors.inside.verifying = parse_color(optarg);
			}
			break;
		case LO_INSIDE_WRONG_COLOR:
			if (state) {
				state->args.colors.inside.wrong = parse_color(optarg);
			}
			break;
		case LO_KEY_HL_COLOR:
			if (state) {
				state->args.colors.key_highlight = parse_color(optarg);
			}
			break;
		case LO_LAYOUT_BG_COLOR:
			if (state) {
				state->args.colors.layout_background = parse_color(optarg);
			}
			break;
		case LO_LAYOUT_BORDER_COLOR:
			if (state) {
				state->args.colors.layout_border = parse_color(optarg);
			}
			break;
		case LO_LAYOUT_TXT_COLOR:
			if (state) {
				state->args.colors.layout_text = parse_color(optarg);
			}
			break;
		case LO_LINE_COLOR:
			if (state) {
				state->args.colors.line.input = parse_color(optarg);
			}
			break;
		case LO_LINE_CLEAR_COLOR:
			if (state) {
				state->args.colors.line.cleared = parse_color(optarg);
			}
			break;
		case LO_LINE_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.line.caps_lock = parse_color(optarg);
			}
			break;
		case LO_LINE_VER_COLOR:
			if (state) {
				state->args.colors.line.verifying = parse_color(optarg);
			}
			break;
		case LO_LINE_WRONG_COLOR:
			if (state) {
				state->args.colors.line.wrong = parse_color(optarg);
			}
			break;
		case LO_RING_COLOR:
			if (state) {
				state->args.colors.ring.input = parse_color(optarg);
			}
			break;
		case LO_RING_CLEAR_COLOR:
			if (state) {
				state->args.colors.ring.cleared = parse_color(optarg);
			}
			break;
		case LO_RING_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.ring.caps_lock = parse_color(optarg);
			}
			break;
		case LO_RING_VER_COLOR:
			if (state) {
				state->args.colors.ring.verifying = parse_color(optarg);
			}
			break;
		case LO_RING_WRONG_COLOR:
			if (state) {
				state->args.colors.ring.wrong = parse_color(optarg);
			}
			break;
		case LO_SEP_COLOR:
			if (state) {
				state->args.colors.separator = parse_color(optarg);
			}
			break;
		case LO_TEXT_COLOR:
			if (state) {
				state->args.colors.text.input = parse_color(optarg);
			}
			break;
		case LO_TEXT_CLEAR_COLOR:
			if (state) {
				state->args.colors.text.cleared = parse_color(optarg);
			}
			break;
		case LO_TEXT_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.text.caps_lock = parse_color(optarg);
			}
			break;
		case LO_TEXT_VER_COLOR:
			if (state) {
				state->args.colors.text.verifying = parse_color(optarg);
			}
			break;
		case LO_TEXT_WRONG_COLOR:
			if (state) {
				state->args.colors.text.wrong = parse_color(optarg);
			}
			break;
		default:
			fprintf(stderr, "%s", usage);
			return 1;
		}
	}

	return 0;
}

static bool file_exists(const char *path) {
	return path && access(path, R_OK) != -1;
}

static char *get_config_path(void) {
	static const char *config_paths[] = {
		"$HOME/.swaylock/config",
		"$XDG_CONFIG_HOME/swaylock/config",
		SYSCONFDIR "/swaylock/config",
	};

	char *config_home = getenv("XDG_CONFIG_HOME");
	if (!config_home || config_home[0] == '\0') {
		config_paths[1] = "$HOME/.config/swaylock/config";
	}

	wordexp_t p;
	char *path;
	for (size_t i = 0; i < sizeof(config_paths) / sizeof(char *); ++i) {
		if (wordexp(config_paths[i], &p, 0) == 0) {
			path = strdup(p.we_wordv[0]);
			wordfree(&p);
			if (file_exists(path)) {
				return path;
			}
			free(path);
		}
	}

	return NULL;
}

static int load_config(char *path, struct swaylock_state *state,
		enum line_mode *line_mode) {
	FILE *config = fopen(path, "r");
	if (!config) {
		swaylock_log(LOG_ERROR, "Failed to read config. Running without it.");
		return 0;
	}
	char *line = NULL;
	size_t line_size = 0;
	ssize_t nread;
	int line_number = 0;
	int result = 0;
	while ((nread = getline(&line, &line_size, config)) != -1) {
		line_number++;

		if (line[nread - 1] == '\n') {
			line[--nread] = '\0';
		}

		if (!*line || line[0] == '#') {
			continue;
		}

		swaylock_log(LOG_DEBUG, "Config Line #%d: %s", line_number, line);
		char *flag = malloc(nread + 3);
		if (flag == NULL) {
			free(line);
			fclose(config);
			swaylock_log(LOG_ERROR, "Failed to allocate memory");
			return 0;
		}
		sprintf(flag, "--%s", line);
		char *argv[] = {"swaylock", flag};
		result = parse_options(2, argv, state, line_mode, NULL);
		free(flag);
		if (result != 0) {
			break;
		}
	}
	free(line);
	fclose(config);
	return 0;
}

static struct swaylock_state state;

static void display_in(int fd, short mask, void *data) {
	if (wl_display_dispatch(state.display) == -1) {
		state.run_display = false;
	}
}

static void comm_in(int fd, short mask, void *data) {
	if (mask & POLLIN) {
		bool auth_success = false;
		if (!read_comm_reply(&auth_success)) {
			exit(EXIT_FAILURE);
		}
		if (auth_success) {
			// Authentication succeeded
			state.run_display = false;
		} else {
			state.auth_state = AUTH_STATE_INVALID;
			schedule_auth_idle(&state);
			++state.failed_attempts;
			damage_state(&state);
		}
	} else if (mask & (POLLHUP | POLLERR)) {
		swaylock_log(LOG_ERROR,	"Password checking subprocess crashed; exiting.");
		exit(EXIT_FAILURE);
	}
}

static void term_in(int fd, short mask, void *data) {
	state.run_display = false;
}

// Check for --debug 'early' we also apply the correct loglevel
// to the forked child, without having to first proces all of the
// configuration (including from file) before forking and (in the
// case of the shadow backend) dropping privileges
void log_init(int argc, char **argv) {
	static struct option long_options[] = {
		{"debug", no_argument, NULL, 'd'},
        {0, 0, 0, 0}
    };
    int c;
	optind = 1;
    while (1) {
		int opt_idx = 0;
		c = getopt_long(argc, argv, "-:d", long_options, &opt_idx);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'd':
			swaylock_log_init(LOG_DEBUG);
			return;
		}
	}
	swaylock_log_init(LOG_ERROR);
}

int main(int argc, char **argv) {
	log_init(argc, argv);
	initialize_pw_backend(argc, argv);
	srand(time(NULL));

	enum line_mode line_mode = LM_LINE;
	state.failed_attempts = 0;
	state.args = (struct swaylock_args){
		.mode = BACKGROUND_MODE_FILL,
		.font = strdup("sans-serif"),
		.font_size = 0,
		.radius = 50,
		.thickness = 10,
		.indicator_x_position = 0,
		.indicator_y_position = 0,
		.override_indicator_x_position = false,
		.override_indicator_y_position = false,
		.ignore_empty = false,
		.show_indicator = true,
		.show_caps_lock_indicator = false,
		.show_caps_lock_text = true,
		.show_keyboard_layout = false,
		.hide_keyboard_layout = false,
		.show_failed_attempts = false,
		.indicator_idle_visible = false,
		.ready_fd = -1,
	};
	wl_list_init(&state.images);
	set_default_colors(&state.args.colors);

	char *config_path = NULL;
	int result = parse_options(argc, argv, NULL, NULL, &config_path);
	if (result != 0) {
		free(config_path);
		return result;
	}
	if (!config_path) {
		config_path = get_config_path();
	}

	if (config_path) {
		swaylock_log(LOG_DEBUG, "Found config at %s", config_path);
		int config_status = load_config(config_path, &state, &line_mode);
		free(config_path);
		if (config_status != 0) {
			free(state.args.font);
			return config_status;
		}
	}

	if (argc > 1) {
		swaylock_log(LOG_DEBUG, "Parsing CLI Args");
		int result = parse_options(argc, argv, &state, &line_mode, NULL);
		if (result != 0) {
			free(state.args.font);
			return result;
		}
	}

	if (line_mode == LM_INSIDE) {
		state.args.colors.line = state.args.colors.inside;
	} else if (line_mode == LM_RING) {
		state.args.colors.line = state.args.colors.ring;
	}

	state.password.len = 0;
	state.password.buffer_len = 1024;
	state.password.buffer = password_buffer_create(state.password.buffer_len);
	if (!state.password.buffer) {
		return EXIT_FAILURE;
	}
	state.password.buffer[0] = 0;

	if (pipe(sigusr_fds) != 0) {
		swaylock_log(LOG_ERROR, "Failed to pipe");
		return EXIT_FAILURE;
	}
	if (fcntl(sigusr_fds[1], F_SETFL, O_NONBLOCK) == -1) {
		swaylock_log(LOG_ERROR, "Failed to make pipe end nonblocking");
		return EXIT_FAILURE;
	}

	wl_list_init(&state.surfaces);
	state.xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	state.display = wl_display_connect(NULL);
	if (!state.display) {
		free(state.args.font);
		swaylock_log(LOG_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return EXIT_FAILURE;
	}
	state.eventloop = loop_create();

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	if (wl_display_roundtrip(state.display) == -1) {
		swaylock_log(LOG_ERROR, "wl_display_roundtrip() failed");
		return EXIT_FAILURE;
	}

	if (!state.compositor) {
		swaylock_log(LOG_ERROR, "Missing wl_compositor");
		return 1;
	}

	if (!state.subcompositor) {
		swaylock_log(LOG_ERROR, "Missing wl_subcompositor");
		return 1;
	}

	if (!state.shm) {
		swaylock_log(LOG_ERROR, "Missing wl_shm");
		return 1;
	}

	if (!state.ext_session_lock_manager_v1) {
		swaylock_log(LOG_ERROR, "Missing ext-session-lock-v1");
		return 1;
	}

	state.ext_session_lock_v1 = ext_session_lock_manager_v1_lock(state.ext_session_lock_manager_v1);
	ext_session_lock_v1_add_listener(state.ext_session_lock_v1,
		&ext_session_lock_v1_listener, &state);

	if (wl_display_roundtrip(state.display) == -1) {
		free(state.args.font);
		return 1;
	}

	state.test_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
	state.test_cairo = cairo_create(state.test_surface);

	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state.surfaces, link) {
		create_surface(surface);
	}

	while (!state.locked) {
		if (wl_display_dispatch(state.display) < 0) {
			swaylock_log(LOG_ERROR, "wl_display_dispatch() failed");
			return 2;
		}
	}

	if (state.args.ready_fd >= 0) {
		if (write(state.args.ready_fd, "\n", 1) != 1) {
			swaylock_log(LOG_ERROR, "Failed to send readiness notification");
			return 2;
		}
		close(state.args.ready_fd);
		state.args.ready_fd = -1;
	}
	if (state.args.daemonize) {
		daemonize();
	}

	loop_add_fd(state.eventloop, wl_display_get_fd(state.display), POLLIN,
			display_in, NULL);

	loop_add_fd(state.eventloop, get_comm_reply_fd(), POLLIN, comm_in, NULL);

	loop_add_fd(state.eventloop, sigusr_fds[0], POLLIN, term_in, NULL);

	struct sigaction sa;
	sa.sa_handler = do_sigusr;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);

	state.run_display = true;
	while (state.run_display) {
		errno = 0;
		if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
			break;
		}
		loop_poll(state.eventloop);
	}

	ext_session_lock_v1_unlock_and_destroy(state.ext_session_lock_v1);
	wl_display_roundtrip(state.display);

	free(state.args.font);
	cairo_destroy(state.test_cairo);
	cairo_surface_destroy(state.test_surface);
	return 0;
}
