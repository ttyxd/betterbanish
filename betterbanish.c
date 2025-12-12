/*
 * xbanish
 * Copyright (c) 2013-2021 joshua stein <jcs@jcs.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN

 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <err.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <libudev.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>
#include <X11/Xutil.h>

void hide_cursor(void);
void show_cursor(void);
int snoop_evdev(void);
void set_alarm(XSyncAlarm *, XSyncTestType);
void usage(char *);
int swallow_error(Display *, XErrorEvent *);
int parse_geometry(const char *s);
int test_bit(int bit, unsigned long *array);



int add_device(const char *);
void remove_device(const char *);

static Display *dpy;
static int hiding = 0, always_hide = 0, ignore_scroll = 0, keystroke_count = 1, current_keystrokes = 0;
static unsigned timeout = 0;
static int jitter = 0;
static int hide_x = 0, hide_y = 0;
static unsigned char ignored;
static XSyncCounter idler_counter = 0;
static XSyncAlarm idle_alarm = None;

static int debug = 0;
#define DPRINTF(x) { if (debug) { printf x; } };

#define MAX_INPUT_DEVICES 16
static int keyboard_fds[MAX_INPUT_DEVICES];
static char *keyboard_paths[MAX_INPUT_DEVICES];
static int num_keyboards = 0;
static int mouse_fds[MAX_INPUT_DEVICES];
static char *mouse_paths[MAX_INPUT_DEVICES];
static int num_mice = 0;

static int move = 0, move_x, move_y, move_custom_x, move_custom_y, move_custom_mask;
enum move_types {
	MOVE_NW = 1,
	MOVE_NE,
	MOVE_SW,
	MOVE_SE,
	MOVE_WIN_NW,
	MOVE_WIN_NE,
	MOVE_WIN_SW,
	MOVE_WIN_SE,
	MOVE_CUSTOM,
};

int
add_device(const char *path)
{
	int fd, i;
	char name[256];
	unsigned long ev_bits[EV_MAX / (sizeof(long) * 8) + 1];
	unsigned long key_bits[KEY_MAX / (sizeof(long) * 8) + 1];

	for (i = 0; i < num_keyboards; i++)
		if (strcmp(keyboard_paths[i], path) == 0)
			return 0;
	for (i = 0; i < num_mice; i++)
		if (strcmp(mouse_paths[i], path) == 0)
			return 0;

	if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) {
		warn("add_device: can't open %s", path);
		return 0;
	}

	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
		warn("add_device: can't get device name for %s", path);
		close(fd);
		return 0;
	}
	DPRINTF(("add_device: checking device %s: %s\n", path, name));

	memset(ev_bits, 0, sizeof(ev_bits));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
		warn("add_device: can't get event bits for %s", path);
		close(fd);
		return 0;
	}

	if (test_bit(EV_KEY, ev_bits) && num_keyboards < MAX_INPUT_DEVICES) {
		memset(key_bits, 0, sizeof(key_bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
			warn("add_device: can't get key bits for %s", path);
			close(fd);
			return 0;
		}

		if (test_bit(KEY_SPACE, key_bits)) {
			DPRINTF(("found keyboard: %s\n", path));
			keyboard_fds[num_keyboards] = fd;
			keyboard_paths[num_keyboards] = strdup(path);
			num_keyboards++;
			return fd;
		}
	}

	if ((test_bit(EV_REL, ev_bits) || test_bit(EV_ABS, ev_bits)) && num_mice < MAX_INPUT_DEVICES) {
		memset(key_bits, 0, sizeof(key_bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
			warn("add_device: can't get key bits for %s", path);
			close(fd);
			return 0;
		}

		if (test_bit(BTN_MOUSE, key_bits) || test_bit(BTN_TOUCH, key_bits)) {
			DPRINTF(("found pointer: %s\n", path));
			mouse_fds[num_mice] = fd;
			mouse_paths[num_mice] = strdup(path);
			num_mice++;
			return fd;
		}
	}

	close(fd);
	return 0;
}


void
remove_device(const char *path)
{
	int i;

	for (i = 0; i < num_keyboards; i++) {
		if (keyboard_paths[i] && strcmp(keyboard_paths[i], path) == 0) {
			DPRINTF(("removing keyboard: %s\n", path));
			close(keyboard_fds[i]);
			free(keyboard_paths[i]);
			keyboard_paths[i] = NULL;
			/* Shift remaining elements */
			for (int j = i; j < num_keyboards - 1; j++) {
				keyboard_fds[j] = keyboard_fds[j + 1];
				keyboard_paths[j] = keyboard_paths[j + 1];
			}
			num_keyboards--;
			return;
		}
	}

	for (i = 0; i < num_mice; i++) {
		if (mouse_paths[i] && strcmp(mouse_paths[i], path) == 0) {
			DPRINTF(("removing pointer: %s\n", path));
			close(mouse_fds[i]);
			free(mouse_paths[i]);
			mouse_paths[i] = NULL;
			/* Shift remaining elements */
			for (int j = i; j < num_mice - 1; j++) {
				mouse_fds[j] = mouse_fds[j + 1];
				mouse_paths[j] = mouse_paths[j + 1];
			}
			num_mice--;
			return;
		}
	}
}

int
main(int argc, char *argv[])
{
	int ch, i;
	XEvent e;
	XSyncAlarmNotifyEvent *alarm_e;
	XGenericEventCookie *cookie;
	XSyncSystemCounter *counters;
	int sync_event, error;
	int major, minor, ncounters;

	struct mod_lookup {
		char *name;
		int mask;
	} mods[] = {
		{"shift", ShiftMask}, {"lock", LockMask},
		{"control", ControlMask}, {"mod1", Mod1Mask},
		{"mod2", Mod2Mask}, {"mod3", Mod3Mask},
		{"mod4", Mod4Mask}, {"mod5", Mod5Mask},
		{"all", -1},
	};

	while ((ch = getopt(argc, argv, "ac:di:j:m:t:s")) != -1)
		switch (ch) {
		case 'a':
			always_hide = 1;
			break;
		case 'c':
			keystroke_count = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			debug = 1;
			break;
		case 'i':
			for (i = 0;
			    i < sizeof(mods) / sizeof(struct mod_lookup); i++)
				if (strcasecmp(optarg, mods[i].name) == 0)
					ignored |= mods[i].mask;

			if (strcasecmp(optarg, "all") == 0)
				ignored &= ~Mod2Mask;

			break;
		case 'j':
			jitter = strtoul(optarg, NULL, 0);
			break;
		case 'm':
			if (strcmp(optarg, "nw") == 0)
				move = MOVE_NW;
			else if (strcmp(optarg, "ne") == 0)
				move = MOVE_NE;
			else if (strcmp(optarg, "sw") == 0)
				move = MOVE_SW;
			else if (strcmp(optarg, "se") == 0)
				move = MOVE_SE;
			else if (strcmp(optarg, "wnw") == 0)
				move = MOVE_WIN_NW;
			else if (strcmp(optarg, "wne") == 0)
				move = MOVE_WIN_NE;
			else if (strcmp(optarg, "wsw") == 0)
				move = MOVE_WIN_SW;
			else if (strcmp(optarg, "wse") == 0)
				move = MOVE_WIN_SE;
			else if (parse_geometry(optarg))
				move = MOVE_CUSTOM;
			else {
				warnx("invalid '-m' argument");
				usage(argv[0]);
			}
			break;
		case 't':
			timeout = strtoul(optarg, NULL, 0);
			break;
		case 's':
			ignore_scroll = 1;
			break;
		default:
			usage(argv[0]);
		}

	argc -= optind;
	argv += optind;

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath proc exec", NULL) == -1)
		err(1, "pledge");
#endif

	XSetErrorHandler(swallow_error);

	if (snoop_evdev() == 0)
		errx(1, "no keyboard or pointer devices found with evdev. "
		    "Try without -E and see evasions issue #1.");

	if (always_hide)
		hide_cursor();

	/* required setup for the xsync alarms used by timeout */
	if (timeout) {
		if (XSyncQueryExtension(dpy, &sync_event, &error) != True)
			errx(1, "no sync extension available");

		XSyncInitialize(dpy, &major, &minor);

		counters = XSyncListSystemCounters(dpy, &ncounters);
		for (i = 0; i < ncounters; i++) {
			if (!strcmp(counters[i].name, "IDLETIME")) {
				idler_counter = counters[i].counter;
				break;
			}
		}
		XSyncFreeSystemCounterList(counters);

		if (!idler_counter)
			errx(1, "no idle counter");
	}

	struct udev *udev;
	struct udev_monitor *mon;
	int udev_fd;
	fd_set fds;
	int max_fd;
	struct timeval tv;
	struct input_event ev;

	udev = udev_new();
	if (!udev)
		errx(1, "udev_new() failed");

	mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!mon)
		errx(1, "udev_monitor_new_from_netlink() failed");

	udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL);
	udev_monitor_enable_receiving(mon);
	udev_fd = udev_monitor_get_fd(mon);

	max_fd = udev_fd;

	for (i = 0; i < num_keyboards; i++)
		if (keyboard_fds[i] > max_fd)
			max_fd = keyboard_fds[i];
	for (i = 0; i < num_mice; i++)
		if (mouse_fds[i] > max_fd)
			max_fd = mouse_fds[i];

	for (;;) {
		FD_ZERO(&fds);
		FD_SET(udev_fd, &fds);
		for (i = 0; i < num_keyboards; i++)
			FD_SET(keyboard_fds[i], &fds);
		for (i = 0; i < num_mice; i++)
			FD_SET(mouse_fds[i], &fds);

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(max_fd + 1, &fds, NULL, NULL, &tv) == -1)
			err(1, "select failed");

		if (FD_ISSET(udev_fd, &fds)) {
			DPRINTF(("udev event received\n"));
			struct udev_device *dev = udev_monitor_receive_device(mon);
			if (dev) {
				const char *action = udev_device_get_action(dev);
				const char *path = udev_device_get_devnode(dev);

				if (action && path) {
					DPRINTF(("udev action: %s for %s\n",
					    action, path));

					if (strcmp(action, "add") == 0) {
						int new_fd = add_device(path);
						if (new_fd > max_fd)
							max_fd = new_fd;
					} else if (strcmp(action, "remove") == 0) {
						remove_device(path);

						max_fd = udev_fd;
						for (i = 0; i < num_keyboards; i++)
							if (keyboard_fds[i] > max_fd)
								max_fd = keyboard_fds[i];
						for (i = 0; i < num_mice; i++)
							if (mouse_fds[i] > max_fd)
								max_fd = mouse_fds[i];
					}
				}

				udev_device_unref(dev);
			}
		}

		for (i = 0; i < num_keyboards; i++) {
			if (FD_ISSET(keyboard_fds[i], &fds)) {
				while(read(keyboard_fds[i], &ev, sizeof(ev)) > 0) {
					if (ev.type == EV_KEY && ev.value == 1) {
						DPRINTF(("evdev: key press\n"));
						current_keystrokes++;
						if (current_keystrokes >= keystroke_count)
							hide_cursor();
					}
				}
			}
		}

		for (i = 0; i < num_mice; i++) {
			if (FD_ISSET(mouse_fds[i], &fds)) {
				while(read(mouse_fds[i], &ev, sizeof(ev)) > 0) {
					if (ev.type == EV_REL || ev.type == EV_ABS) {
						DPRINTF(("evdev: pointer move\n"));
						if (!always_hide)
							show_cursor();
					} else if (ev.type == EV_KEY && ev.value == 1) {
						DPRINTF(("evdev: pointer button press\n"));
						if (!always_hide)
							show_cursor();
					}
				}
			}
		}
	}
}
void
hide_cursor(void)
{
	Window win;
	XWindowAttributes attrs;
	int x = 0, y = 0, h, w, junk;
	unsigned int ujunk;

	DPRINTF(("keystroke, %shiding cursor\n", (hiding ? "already " : "")));

	if (hiding)
		return;

	if (XQueryPointer(dpy, DefaultRootWindow(dpy),
	    &win, &win, &hide_x, &hide_y, &junk, &junk, &ujunk)) {
		if (move) {
			move_x = hide_x;
			move_y = hide_y;

			XGetWindowAttributes(dpy, win, &attrs);

			h = XHeightOfScreen(DefaultScreenOfDisplay(dpy));
			w = XWidthOfScreen(DefaultScreenOfDisplay(dpy));

			switch (move) {
			case MOVE_NW:
				x = 0;
				y = 0;
				break;
			case MOVE_NE:
				x = w;
				y = 0;
				break;
			case MOVE_SW:
				x = 0;
				y = h;
				break;
			case MOVE_SE:
				x = w;
				y = h;
				break;
			case MOVE_WIN_NW:
				x = attrs.x;
				y = attrs.y;
				break;
			case MOVE_WIN_NE:
				x = attrs.x + attrs.width;
				y = attrs.y;
				break;
			case MOVE_WIN_SW:
				x = attrs.x;
				y = attrs.x + attrs.height;
				break;
			case MOVE_WIN_SE:
				x = attrs.x + attrs.width;
				y = attrs.x + attrs.height;
				break;
			case MOVE_CUSTOM:
				x = (move_custom_mask & XNegative ? w : 0) + move_custom_x;
				y = (move_custom_mask & YNegative ? h : 0) + move_custom_y;
				break;
			}

			XWarpPointer(dpy, None, DefaultRootWindow(dpy),
			    0, 0, 0, 0, x, y);
		}
	} else if (move) {
		move_x = -1;
		move_y = -1;
		warn("failed finding cursor coordinates");
	}

	XFixesHideCursor(dpy, DefaultRootWindow(dpy));
	XFlush(dpy);
	hiding = 1;

	if (jitter)
		system("xmodmap -e \"pointer = 0 0 0\"");
}

void
show_cursor(void)
{
	current_keystrokes = 0;

	Window win;
	int cur_x, cur_y, junk;
	unsigned int ujunk;

	DPRINTF(("mouse moved, %sunhiding cursor\n",
	    (hiding ? "" : "already ")));

	if (timeout) {
		DPRINTF(("(re)setting timeout of %us\n", timeout));
		set_alarm(&idle_alarm, XSyncPositiveComparison);
	}

	if (!hiding)
		return;

	if (jitter) {
		if (!XQueryPointer(dpy, DefaultRootWindow(dpy),
		    &win, &win, &cur_x, &cur_y, &junk, &junk, &ujunk)) {
			warn("failed finding cursor coordinates");
			return;
		}

		if ((abs(cur_x - hide_x) < jitter) &&
		    (abs(cur_y - hide_y) < jitter)) {
			DPRINTF(("not unhiding, cursor moved less than "
			    "jitter %d\n", jitter));
			return;
		}
	}

	if (move && move_x != -1 && move_y != -1)
		XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0,
		    move_x, move_y);

	if (jitter)
		system("xmodmap -e \"pointer = default\"");

	XFixesShowCursor(dpy, DefaultRootWindow(dpy));
	XFlush(dpy);
	hiding = 0;
}

int
test_bit(int bit, unsigned long *array)
{
	return (array[bit / (sizeof(long) * 8)] >> (bit % (sizeof(long) * 8))) & 1;
}

int
snoop_evdev(void)
{
	DIR *dir;
	struct dirent *entry;
	char path[512];

	if (!(dir = opendir("/dev/input"))) {
		warn("can't open /dev/input");
		return 0;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "event", 5) == 0) {
			snprintf(path, sizeof(path), "/dev/input/%s",
			    entry->d_name);
			add_device(path);
		}
	}

	closedir(dir);
	return num_keyboards + num_mice;
}



void
set_alarm(XSyncAlarm *alarm, XSyncTestType test)
{
	XSyncAlarmAttributes attr;
	XSyncValue value;
	unsigned int flags;

	XSyncQueryCounter(dpy, idler_counter, &value);

	attr.trigger.counter = idler_counter;
	attr.trigger.test_type = test;
	attr.trigger.value_type = XSyncRelative;
	XSyncIntsToValue(&attr.trigger.wait_value, timeout * 1000,
	    (unsigned long)(timeout * 1000) >> 32);
	XSyncIntToValue(&attr.delta, 0);

	flags = XSyncCACounter | XSyncCATestType | XSyncCAValue | XSyncCADelta;

	if (*alarm)
		XSyncDestroyAlarm(dpy, *alarm);

	*alarm = XSyncCreateAlarm(dpy, flags, &attr);
}

void
usage(char *progname)
{
	fprintf(stderr, "usage: %s [-a] [-c count] [-d] [-i mod] [-j pixels] "
	    "[-m [w]nw|ne|sw|se|+/-xy] [-t seconds] [-s]\n", progname);
	exit(1);
}

int
swallow_error(Display *d, XErrorEvent *e)
{
	if (e->error_code == BadWindow)
		/* no biggie */
		return 0;

	if (e->error_code & FirstExtensionError)
		/* error requesting input on a particular xinput device */
		return 0;

	errx(1, "got X error %d", e->error_code);
}

int
parse_geometry(const char *s)
{
	int x, y;
	unsigned int junk;
	int ret = XParseGeometry(s, &x, &y, &junk, &junk);
	if (((ret & XValue) || (ret & XNegative)) &&
	    ((ret & YValue) || (ret & YNegative))) {
		move_custom_x = x;
		move_custom_y = y;
		move_custom_mask = ret;
		return 1;
	}
	return 0;
}
