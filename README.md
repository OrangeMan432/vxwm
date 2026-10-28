# vxrwm

vxrwm - Versatile Wayland window manager forked from `dwm`, driven by the
`river` window management protocol.

## About

vxrwm represents a significantly enhanced version of `dwm` that maintains its
lightweight nature while offering modular flexibility. Instead of manually
applying patches, you can toggle pre-installed features directly in the
configuration by switching values between 0 and 1, it is all manageable via
`modules.h`.

Instead of speaking X11, vxrwm is a plain Wayland client that runs as the
configuration command of `river` and talks to the compositor through
`river-window-management-v1`, `river-xkb-bindings-v1` and
`river-layer-shell-v1`. The background shell of every monitor is a
`zwlr-layer-shell` surface, so a wallpaper client such as `swaybg` sits
underneath the windows.

The defining feature of vxwm is its implementation of infinite tags. While
traditional tiling managers act like a slide projector, swapping one static view
for another, vxwm treats the screen as a viewport over a vast, continuous
canvas. Windows aren't hidden or layered; they exist on an infinite surface, and
you simply move your perspective across it. You can slide your view to find more
space, snap focus to a specific window, or return to the origin using the
homecanvas bind. Even though this sounds complex, this isn't resource hungry and
isn't hard to use.

### vxwm has repositories on

- [Codeberg](https://codeberg.org/wh1tepearl/vxwm)
- [GitHub (read-only mirror)](https://github.com/wh1tepearll/vxwm)

## Build and Requirements

In order to build vxrwm you need the `wayland-client`, `wayland-cursor`,
`wayland-protocols`, `river-protocols`, `xkbcommon`, `fcft` and `pixman-1`
development files, `wayland-scanner` and some sort of `make`.

Arch Linux:

```bash
sudo pacman -Sy wayland wayland-protocols river wayland-scanner \
    libxkbcommon fcft pixman
```

Void Linux:

```bash
sudo xbps-install -S wayland-devel wayland-protocols-devel river \
    wayland-scanner libxkbcommon-devel fcft-devel pixman-devel
```

Gentoo GNU/Linux:

```bash
doas emerge -av dev-libs/wayland dev-libs/wayland-protocols \
    gui-wm/river dev-libs/libxkbcommon gui-libs/fcft x11-libs/pixman \
    dev-util/wayland-scanner
```

`river-protocols` is shipped by river and located with
`pkg-config --variable=pkgdatadir river-protocols`.

## Installation

Clone this repository and `cd` into it.

```bash
git clone https://codeberg.org/wh1tepearl/vxwm.git
cd vxwm
```

Edit `config.mk` to match your local setup (vxrwm is installed into
the `/usr/local` namespace by default).

Afterwards enter the following commands to build and install vxrwm:

```bash
make
# yes, run make first and only then
sudo make clean install
```

## Running vxrwm

vxrwm is meant to be started by river as its configuration command, and the
binary has to be reachable from `PATH` (or referenced by an absolute path):

```sh
river -c vxrwm
```

`river` reads `~/.config/river/init` by default, so a typical init looks like
this:

```sh
#!/bin/sh

exec vxrwm -ignoreautostart
```

If you want to restart river without losing your session, or for a hot
configuration reload, run `rvx` from inside the session. `rvx -d` does the same
through `dbus-run-session`.

In order to talk to a specific compositor, make sure that the `WAYLAND_DISPLAY`
environment variable is set correctly, e.g.:

```bash
WAYLAND_DISPLAY=wayland-1 exec vxrwm
```

### Status text

vxrwm reads the status text of the bar from a named pipe instead of an X root
window name. The path is taken from `VXRWM_STATUS` and defaults to
`$XDG_RUNTIME_DIR/vxwm_status` (falling back to `/tmp/vxwm_status`); the pipe is
created automatically. A status script can therefore be as simple as:

```bash
while printf '%s\n' "$(date)"; do sleep 1; done > "${VXRWM_STATUS:-$XDG_RUNTIME_DIR/vxwm_status}" &
```

## Configuration

The configuration of vxrwm is done by editing config.h and modules.h to match
your preferences and (re)compiling the source code.

### Adding custom keybinds

Add this to `config.h` and replace `yoursillyprogram` with the actual `cmd` that
will be executed in hte keybind (recommended adding it right before keys array):

```c
static const char *yoursillyprogramcmd[]  = { "yoursillyprogram", NULL };
```

If your cmd uses multiple arguments, you should write them like this:

```c
static const char *yoursillyprogramcmd[] = { "yoursillyprogram", "arg1", "arg2", NULL};
```

etc...

And then add this to keys massive:

```c
{ MODKEY|ShiftMask,	XK_u, spawn, {.v = yoursillyprogramcmd } }, 
```

### Modules

Enable/disable (0/1) modules you need/don't need, thats it.

> [!NOTE]
> After any change in config/modules recompile vxrwm and restart using rvx.

## Acknowledgements

- `vxwm` was made by `wh1tepearl`, many thanks to [suckless.org] and the [dwm]
developers for making dwm in first place.
- Thanks 5element developer and hevel wayland compositor developers
for the inspiration of infinite tags.
- tinyrwm, the minimal river window manager, which the Wayland backend of
vxrwm is modelled on: <https://codeberg.org/river/tinyrwm>

Also try:

- hevel wayland compositor: <https://git.sr.ht/~dlm/hevel>
- 5element: <https://hg.sr.ht/~ohmu/5element>

## Community

- If you encounter any bugs - **please make an issue!**
- If you want something added - **please make an issue!**
- If you want to change something - **please make an issue!**
