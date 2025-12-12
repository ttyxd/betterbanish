# betterbanish

**betterbanish** is a robust, enhanced version of `xbanish`. It automatically hides the mouse cursor when you start typing and unhides it when you move the mouse. This version includes fixes for modern input handling, improved resource management, and hotplug support via udev.

It is lightweight, efficient, and designed for X11 environments.

## Features

- **Automatic Hiding:** Hides the cursor after a configurable number of keystrokes.
- **Instant Unhiding:** Reappears immediately upon mouse movement or clicking.
- **Hotplug Support:** Automatically detects new keyboards and mice (via `udev`) without restarting the daemon.
- **Idle Timeout:** Optionally hides the cursor after a specific period of inactivity.
- **Cursor Relocation:** Can move the cursor to a specific corner or custom coordinates when hiding to prevent accidental hovering.
- **Modifier Awareness:** Can ignore specific modifier keys (like Shift, Ctrl) so the cursor doesn't flicker while using shortcuts.
- **Jitter Protection:** Prevents accidental unhiding due to minor sensor vibrations.

## Requirements

To build `betterbanish`, you need a C compiler (gcc/clang) and the following development libraries:

- **libX11**
- **libXfixes**
- **libXi** (XInput)
- **libudev**

### Debian/Ubuntu

```bash
sudo apt install build-essential libx11-dev libxfixes-dev libxi-dev libudev-dev
```

### Arch Linux

```bash
sudo pacman -S base-devel libx11 libxfixes libxi systemd-libs
```

## Compilation

You can compile the program using the following command:

```bash
make
```

To install it globally (optional):

```bash
sudo cp betterbanish /usr/local/bin/
```

## Usage

Start `betterbanish` in the background (usually in your `.xinitrc` or window manager startup script).

```bash
betterbanish &
```

### Command Line Options

| Option        | Description                                                                                                                                                               |
| :------------ | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `-a`          | **Always hide** the cursor persistently (useful for kiosks).                                                                                                              |
| `-c <count>`  | Number of keystrokes required before hiding (default: 1).                                                                                                                 |
| `-d`          | Enable debug mode (prints verbose output to stdout).                                                                                                                      |
| `-i <mod>`    | Ignore specific modifier keys. Options: `shift`, `lock`, `control`, `mod1` (Alt), `mod2` (NumLock), `mod3`, `mod4` (Super), `mod5`, or `all`. Can be used multiple times. |
| `-j <pixels>` | Jitter threshold. Mouse must move more than `<pixels>` to unhide.                                                                                                         |
| `-m <loc>`    | Move cursor to a location when hiding. Options: `nw`, `ne`, `sw`, `se` (screen corners), `wnw`, `wne`, `wsw`, `wse` (active window corners), or standard geometry `+x+y`. |
| `-t <sec>`    | Hide cursor after `<sec>` seconds of user inactivity.                                                                                                                     |
| `-s`          | Ignore scrolling events (scrolling won't unhide the cursor).                                                                                                              |

### Examples

**Standard usage:**
Hide instantly on any key press.

```bash
betterbanish &
```

**Ignore modifiers:**
Don't hide the cursor just because you pressed Shift or Ctrl.

```bash
betterbanish -i shift -i control &
```

**Touchpad Problems?**

```bash
betterbanish -j 20 -i all -c 5
```

This command breaks down as follows:

| **Option** | **Description**                                                                                             |
| ---------- | ----------------------------------------------------------------------------------------------------------- |
| `-c 5`     | The cursor stays visible until **5 keys** have been pressed.                                                |
| `-i all`   | Ignores **all modifier keys** (Shift, Ctrl, Alt, Super) so they **don’t count** toward the 5-key threshold. |
| `-j 20`    | Sets a **20-pixel jitter threshold** so tiny mouse movements **don’t unhide** the cursor by accident.       |

### Summary

The command configures **betterbanish** so the cursor hides only after five real keystrokes (ignoring modifiers) and won’t reappear unless the mouse is moved significantly (20+ pixels). Debug logs are enabled to show what the program is doing.

If you want, I can also rewrite this into a one-line explanation or format it for a README.

## How It Works

`betterbanish` connects to the X11 server to manage cursor visibility and uses `libudev` to monitor `/dev/input/` for input devices.

1.  It scans for existing keyboards and mice.
2.  It listens for `udev` events to handle devices plugged in after startup.
3.  It intercepts global keystrokes; if the keystroke limit is reached, it calls `XFixesHideCursor`.
4.  If the mouse moves or clicks, it calls `XFixesShowCursor`.

## Credits

Based on `xbanish` by Joshua Stein <jcs@jcs.org>.

Modifications for `betterbanish` by ttyxd <cmax0890@gmail.com>
