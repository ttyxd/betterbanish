/*
 * betterbanish.c - A robust, fixed version of xbanish
 * Based on xbanish by joshua stein <jcs@jcs.org>
 */

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/sync.h>

#define MAX_INPUT_DEVICES 64
#define DPRINTF(x)                                                             \
  do {                                                                         \
    if (debug) {                                                               \
      printf x;                                                                \
    }                                                                          \
  } while (0)

/* Forward declarations */
static void get_mod_map(void);
static void free_mod_map(void);
static void hide_cursor(void);
static void show_cursor(void);
static int snoop_evdev(void);
static void set_alarm(XSyncAlarm *, XSyncTestType);
static void usage(char *);
static int swallow_error(Display *, XErrorEvent *);
static int parse_geometry(const char *s);
static int test_bit(int bit, unsigned long *array);
static int add_device(const char *);
static void remove_device(const char *);
static int recompute_max_fd(int udev_fd, int x11_fd);

struct mod_map_entry {
  char *name;
  int mask;
  KeyCode *keycodes;
  int keycode_count;
};

static struct mod_map_entry *mod_map;
static int mod_map_count = 0;

static Display *dpy;
static int hiding = 0, always_hide = 0, ignore_scroll = 0;
static int keystroke_count = 1, current_keystrokes = 0;
static unsigned int timeout = 0;
static int jitter = 0;
static int hide_x = 0, hide_y = 0;
static unsigned int ignored = 0; /* Changed from char to int for bitmasks */
static XSyncCounter idler_counter = 0;
static XSyncAlarm idle_alarm = None;

static int debug = 0;

static int keyboard_fds[MAX_INPUT_DEVICES];
static char *keyboard_paths[MAX_INPUT_DEVICES];
static int num_keyboards = 0;
static int mouse_fds[MAX_INPUT_DEVICES];
static char *mouse_paths[MAX_INPUT_DEVICES];
static int num_mice = 0;

static int move = 0, move_x, move_y, move_custom_x, move_custom_y,
           move_custom_mask;
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

static int add_device(const char *path) {
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
    /* Some devices don't have names, not fatal */
    snprintf(name, sizeof(name), "Unknown");
  }

  memset(ev_bits, 0, sizeof(ev_bits));
  if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
    close(fd);
    return 0;
  }

  /* Check for Keyboard */
  if (test_bit(EV_KEY, ev_bits) && num_keyboards < MAX_INPUT_DEVICES) {
    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
      if (test_bit(KEY_SPACE, key_bits)) {
        DPRINTF(("found keyboard: %s (%s)\n", path, name));
        keyboard_fds[num_keyboards] = fd;
        keyboard_paths[num_keyboards] = strdup(path);
        num_keyboards++;
        return fd;
      }
    }
  }

  /* Check for Mouse */
  if ((test_bit(EV_REL, ev_bits) || test_bit(EV_ABS, ev_bits)) &&
      num_mice < MAX_INPUT_DEVICES) {
    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
      if (test_bit(BTN_MOUSE, key_bits) || test_bit(BTN_TOUCH, key_bits)) {
        DPRINTF(("found pointer: %s (%s)\n", path, name));
        mouse_fds[num_mice] = fd;
        mouse_paths[num_mice] = strdup(path);
        num_mice++;
        return fd;
      }
    }
  }

  close(fd);
  return 0;
}

static void remove_device(const char *path) {
  int i, j;

  for (i = 0; i < num_keyboards; i++) {
    if (keyboard_paths[i] && strcmp(keyboard_paths[i], path) == 0) {
      DPRINTF(("removing keyboard: %s\n", path));
      close(keyboard_fds[i]);
      free(keyboard_paths[i]);

      for (j = i; j < num_keyboards - 1; j++) {
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

      for (j = i; j < num_mice - 1; j++) {
        mouse_fds[j] = mouse_fds[j + 1];
        mouse_paths[j] = mouse_paths[j + 1];
      }
      num_mice--;
      return;
    }
  }
}

static int recompute_max_fd(int udev_fd, int x11_fd) {
  int max = (udev_fd > x11_fd) ? udev_fd : x11_fd;
  for (int i = 0; i < num_keyboards; i++)
    if (keyboard_fds[i] > max)
      max = keyboard_fds[i];
  for (int i = 0; i < num_mice; i++)
    if (mouse_fds[i] > max)
      max = mouse_fds[i];
  return max;
}

int main(int argc, char *argv[]) {
  int ch, i;
  XEvent e;
  int sync_event = 0, error;
  int major, minor, ncounters;
  XSyncSystemCounter *counters;

  struct mod_lookup {
    char *name;
    int mask;
  } mods[] = {
      {"shift", ShiftMask}, {"lock", LockMask}, {"control", ControlMask},
      {"mod1", Mod1Mask},   {"mod2", Mod2Mask}, {"mod3", Mod3Mask},
      {"mod4", Mod4Mask},   {"mod5", Mod5Mask}, {"all", -1},
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
      for (i = 0; i < (int)(sizeof(mods) / sizeof(struct mod_lookup)); i++)
        if (strcasecmp(optarg, mods[i].name) == 0)
          ignored |= mods[i].mask;
      if (strcasecmp(optarg, "all") == 0)
        ignored &= ~Mod2Mask; /* usually numlock */
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

  if (!(dpy = XOpenDisplay(NULL)))
    errx(1, "can't open display %s", XDisplayName(NULL));

  get_mod_map();
  atexit(free_mod_map);

  XSetErrorHandler(swallow_error);

  /* XSync / Timeout Setup */
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

  if (snoop_evdev() == 0)
    warnx("no input devices found in /dev/input (check permissions?)");

  if (always_hide)
    hide_cursor();

  /* Udev Setup */
  struct udev *udev = udev_new();
  if (!udev)
    errx(1, "udev_new() failed");
  struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
  if (!mon)
    errx(1, "udev_monitor failed");
  udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL);
  udev_monitor_enable_receiving(mon);
  int udev_fd = udev_monitor_get_fd(mon);

  /* Main Loop Setup */
  int x11_fd = ConnectionNumber(dpy);
  fd_set fds;
  int max_fd = recompute_max_fd(udev_fd, x11_fd);
  struct input_event ev;

  for (;;) {
    FD_ZERO(&fds);
    FD_SET(udev_fd, &fds);
    FD_SET(x11_fd, &fds);
    for (i = 0; i < num_keyboards; i++)
      FD_SET(keyboard_fds[i], &fds);
    for (i = 0; i < num_mice; i++)
      FD_SET(mouse_fds[i], &fds);

    if (select(max_fd + 1, &fds, NULL, NULL, NULL) == -1) {
      if (errno == EINTR)
        continue;
      err(1, "select failed");
    }

    /* Handle X11 Events (Timeouts) */
    if (FD_ISSET(x11_fd, &fds)) {
      while (XPending(dpy)) {
        XNextEvent(dpy, &e);
        if (timeout && e.type == sync_event + XSyncAlarmNotify) {
          DPRINTF(("idle timeout reached, hiding cursor\n"));
          hide_cursor();
        }
      }
    }

    /* Handle Udev Events (Hotplug) */
    if (FD_ISSET(udev_fd, &fds)) {
      struct udev_device *dev = udev_monitor_receive_device(mon);
      if (dev) {
        const char *action = udev_device_get_action(dev);
        const char *path = udev_device_get_devnode(dev);
        if (action && path) {
          if (strcmp(action, "add") == 0)
            add_device(path);
          else if (strcmp(action, "remove") == 0)
            remove_device(path);
          max_fd = recompute_max_fd(udev_fd, x11_fd);
        }
        udev_device_unref(dev);
      }
    }

    /* Handle Keyboards */
    for (i = 0; i < num_keyboards; i++) {
      if (FD_ISSET(keyboard_fds[i], &fds)) {
        /* Read loop to drain buffer */
        while (read(keyboard_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
          if (ev.type == EV_KEY && ev.value == 1) { /* Key Press */
            char keys_return[32];
            XQueryKeymap(dpy, keys_return);
            int ignore_keystroke = 0;

            for (int j = 0; j < mod_map_count; j++) {
              if (mod_map[j].mask & ignored) {
                for (int k = 0; k < mod_map[j].keycode_count; k++) {
                  KeyCode keycode = mod_map[j].keycodes[k];
                  if ((keys_return[keycode >> 3] >> (keycode & 7)) & 1) {
                    ignore_keystroke = 1;
                    goto check_ignore;
                  }
                }
              }
            }
          check_ignore:
            if (!ignore_keystroke) {
              current_keystrokes++;
              if (current_keystrokes >= keystroke_count)
                hide_cursor();
            }
          }
        }
      }
    }

    /* Handle Mice */
    for (i = 0; i < num_mice; i++) {
      if (FD_ISSET(mouse_fds[i], &fds)) {
        while (read(mouse_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
          if (ev.type == EV_REL || ev.type == EV_ABS) {
            if (!always_hide)
              show_cursor();
          } else if (ev.type == EV_KEY && ev.value == 1) {
            if (!always_hide)
              show_cursor();
          }
        }
      }
    }
  }
}

static void hide_cursor(void) {
  Window win;
  XWindowAttributes attrs;
  int x = 0, y = 0, h, w, junk;
  unsigned int ujunk;

  if (hiding)
    return;
  DPRINTF(("hiding cursor\n"));

  if (XQueryPointer(dpy, DefaultRootWindow(dpy), &win, &win, &hide_x, &hide_y,
                    &junk, &junk, &ujunk)) {
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
      /* FIXED: attrs.x -> attrs.y */
      case MOVE_WIN_SW:
        x = attrs.x;
        y = attrs.y + attrs.height;
        break;
      case MOVE_WIN_SE:
        x = attrs.x + attrs.width;
        y = attrs.y + attrs.height;
        break;
      case MOVE_CUSTOM:
        x = (move_custom_mask & XNegative ? w : 0) + move_custom_x;
        y = (move_custom_mask & YNegative ? h : 0) + move_custom_y;
        break;
      }
      XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, x, y);
    }
  } else if (move) {
    move_x = -1;
    move_y = -1;
  }

  XFixesHideCursor(dpy, DefaultRootWindow(dpy));
  XFlush(dpy);
  hiding = 1;
}

static void show_cursor(void) {
  current_keystrokes = 0;
  Window win;
  int cur_x, cur_y, junk;
  unsigned int ujunk;

  /* Reset timeout alarm if configured */
  if (timeout) {
    set_alarm(&idle_alarm, XSyncPositiveComparison);
  }

  if (!hiding)
    return;

  if (jitter) {
    if (!XQueryPointer(dpy, DefaultRootWindow(dpy), &win, &win, &cur_x, &cur_y,
                       &junk, &junk, &ujunk))
      return;
    if ((abs(cur_x - hide_x) < jitter) && (abs(cur_y - hide_y) < jitter)) {
      /* Movement within jitter threshold; ignore */
      return;
    }
  }

  DPRINTF(("unhiding cursor\n"));

  if (move && move_x != -1 && move_y != -1)
    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, move_x, move_y);

  XFixesShowCursor(dpy, DefaultRootWindow(dpy));
  XFlush(dpy);
  hiding = 0;
}

static int test_bit(int bit, unsigned long *array) {
  return (array[bit / (sizeof(long) * 8)] >> (bit % (sizeof(long) * 8))) & 1;
}

static int snoop_evdev(void) {
  DIR *dir;
  struct dirent *entry;
  char path[512];

  if (!(dir = opendir("/dev/input"))) {
    warn("can't open /dev/input");
    return 0;
  }
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) == 0) {
      snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
      add_device(path);
    }
  }
  closedir(dir);
  return num_keyboards + num_mice;
}

static void set_alarm(XSyncAlarm *alarm, XSyncTestType test) {
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

static void usage(char *progname) {
  fprintf(stderr,
          "usage: %s [-a] [-c count] [-d] [-i mod] [-j pixels] "
          "[-m [w]nw|ne|sw|se|+/-xy] [-t seconds] [-s]\n",
          progname);
  exit(1);
}

static int swallow_error(Display *d, XErrorEvent *e) {
  if (e->error_code == BadWindow)
    return 0;
  if (e->error_code & FirstExtensionError)
    return 0;
  errx(1, "got X error %d", e->error_code);
}

static int parse_geometry(const char *s) {
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

static void free_mod_map(void) {
  if (mod_map) {
    for (int i = 0; i < mod_map_count; i++) {
      free(mod_map[i].name);
      free(mod_map[i].keycodes);
    }
    free(mod_map);
  }
}

static void get_mod_map(void) {
  XModifierKeymap *modmap = XGetModifierMapping(dpy);
  int i, j;

  mod_map_count = 8;
  mod_map = calloc(mod_map_count, sizeof(struct mod_map_entry));

  char *mod_names[] = {"shift", "lock", "control", "mod1",
                       "mod2",  "mod3", "mod4",    "mod5"};
  int mod_masks[] = {ShiftMask, LockMask, ControlMask, Mod1Mask,
                     Mod2Mask,  Mod3Mask, Mod4Mask,    Mod5Mask};

  for (i = 0; i < mod_map_count; i++) {
    mod_map[i].name = strdup(mod_names[i]);
    mod_map[i].mask = mod_masks[i];
    mod_map[i].keycode_count = 0;
    mod_map[i].keycodes = NULL;

    for (j = 0; j < modmap->max_keypermod; j++) {
      if (modmap->modifiermap[i * modmap->max_keypermod + j] != 0) {
        mod_map[i].keycode_count++;
        mod_map[i].keycodes = realloc(
            mod_map[i].keycodes, mod_map[i].keycode_count * sizeof(KeyCode));
        mod_map[i].keycodes[mod_map[i].keycode_count - 1] =
            modmap->modifiermap[i * modmap->max_keypermod + j];
      }
    }
  }
  XFreeModifiermap(modmap);
}
