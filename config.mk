# vxrwm version
VERSION = 3.0

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

# wayland
WAYLAND_SCANNER   = $(shell pkg-config --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
RIVER_PROTOCOLS = $(shell pkg-config --variable=pkgdatadir river-protocols)

# libraries
PKGS = wayland-client xkbcommon fcft pixman-1 wayland-cursor

# includes and libs
INCS = $(shell pkg-config --cflags ${PKGS})
LIBS = $(shell pkg-config --libs ${PKGS})

# flags
CPPFLAGS = -D_GNU_SOURCE -DVERSION=\"${VERSION}\" -DSRCDIR=\"${CURDIR}\"
#CFLAGS   = -g -std=c11 -pedantic -Wall -O0 ${INCS} ${CPPFLAGS}
CFLAGS   = -std=c11 -pedantic -Wall -Wno-deprecated-declarations ${INCS} ${CPPFLAGS}
LDFLAGS  = ${LIBS}

# compiler and linker
CC = cc
