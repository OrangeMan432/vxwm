# vxrwm - Versatile Wayland River Window Manager
# See LICENSE file for copyright and license details.

include config.mk

SRC = vxrwm.c drw.c util.c
OBJ = ${SRC:.c=.o}

# generated protocol sources
PROTOS = cursor-shape river-window-management river-xkb-bindings river-layer-shell
GENHDR = ${PROTOS:%=protocol/%-client-protocol.h}
GENSRC = ${PROTOS:%=protocol/%-protocol.c}
GENOBJ = ${GENSRC:.c=.o}

all: vxrwm

# referenced by the generated cursor-shape private code
protocol/tablet-v2-protocol.c: ${WAYLAND_PROTOCOLS}/unstable/tablet/tablet-unstable-v2.xml
	${WAYLAND_SCANNER} private-code $< $@
GENOBJ += protocol/tablet-v2-protocol.o

config.h:
	cp config.def.h $@

modules.h:
	cp modules.def.h $@

${OBJ}: config.h modules.h config.mk
vxrwm.o: modules/vxrwm_includes.c modules/vxrwm_includes.h $(wildcard modules/*/*.c) $(wildcard modules/*/*.h)
drw.o: ${GENHDR}
vxrwm.o: ${GENHDR}
util.o: ${GENHDR}

protocol/cursor-shape-v1.xml:
	cp ${WAYLAND_PROTOCOLS}/staging/cursor-shape/cursor-shape-v1.xml $@

protocol/river-layer-shell-v1.xml:
	cp ${RIVER_PROTOCOLS}/stable/river-layer-shell-v1.xml $@

protocol/%-client-protocol.h: protocol/%-v1.xml
	${WAYLAND_SCANNER} client-header $< $@

protocol/%-protocol.c: protocol/%-v1.xml
	${WAYLAND_SCANNER} private-code $< $@

.c.o:
	${CC} -c ${CFLAGS} $< -o $@

${GENOBJ}: ${GENHDR}

vxrwm: ${OBJ} ${GENOBJ}
	${CC} -o $@ ${OBJ} ${GENOBJ} ${LDFLAGS}

clean:
	rm -f vxrwm ${OBJ} ${GENOBJ} ${GENHDR} protocol/cursor-shape-v1.xml
	rm -f protocol/river-layer-shell-v1.xml
	rm -f protocol/tablet-v2-protocol.c protocol/tablet-v2-protocol.o
	rm -f vxrwm-${VERSION}.tar.gz

dist: clean
	mkdir -p vxrwm-${VERSION}
	cp -R LICENSE Makefile README.md config.def.h config.mk\
		vxrwm.1 drw.h util.h xkbcompat.h ${SRC} vxrwm-${VERSION}
	cp -R protocol vxrwm-${VERSION}/protocol
	rm -f vxrwm-${VERSION}/protocol/*.o vxrwm-${VERSION}/protocol/*-client-protocol.h
	tar -cf vxrwm-${VERSION}.tar vxrwm-${VERSION}
	gzip vxrwm-${VERSION}.tar
	rm -rf vxrwm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f vxrwm ${DESTDIR}${PREFIX}/bin/vxrwm
	chmod 755 ${DESTDIR}${PREFIX}/bin/vxrwm
	cp -f rvx ${DESTDIR}${PREFIX}/bin/rvx
	chmod 755 ${DESTDIR}${PREFIX}/bin/rvx
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < vxrwm.1 > ${DESTDIR}${MANPREFIX}/man1/vxrwm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/vxrwm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/vxrwm\
		${DESTDIR}${MANPREFIX}/man1/vxrwm.1
	rm -f ${DESTDIR}${PREFIX}/bin/rvx

.PHONY: all clean dist install uninstall
