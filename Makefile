PROG = xmenu
LIB_OBJS = \
	control/application.o \
	control/font.o \
	control/menu.o \
	control/resources.o \
	util/color.o
OBJS = ${PROG:=.o} ${LIB_OBJS}
SRCS = ${OBJS:.o=.c}
MANS = ${PROG:=.1} \
	control/ctrlapp.3 \
	control/ctrlfnt.3 \
	control/ctrlmenu.3 \
	control/ctrlres.3
PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
bindir = ${DESTDIR}${PREFIX}/bin
mandir = ${DESTDIR}${MANPREFIX}/man1
_cflags = -std=c2x -D_XOPEN_SOURCE=800 -D_BSD_SOURCE -D_GNU_SOURCE \
	-I./ -I/usr{,/local,/X11R6}/include{,/freetype2} ${CPPFLAGS} ${CFLAGS}

.PHONY: all
all: ${PROG}
${PROG}: ${@:=.o} ${LIB_OBJS}
	${CC} -o $@ ${@:=.o} ${LIB_OBJS} \
	-lfontconfig -lXft -lX11 -lXrandr -lXrender -lImlib2 ${LDLIBS} \
	-L/usr{,/local,/X11R6}/lib ${LDFLAGS}

.PHONY: debug
debug:
	@${MAKE} ${MAKEFLAGS} \
	CFLAGS+="-g -O0 -DDEBUG=1 -Wall -Wextra -Wpedantic ${DEBUG}" \
	${PROG}

${OBJS}: util/defs.h
${PROG:=.o}: ${LIB_OBJS:.o=.h}
${LIB_OBJS}: ${@:.o=.h}
control/menu: control/application.h control/font.h control/resources.h
.c.o:
	${CC} ${_cflags} -o $@ -c $<

.PHONY: tags
tags: ${SRCS}
	ctags ${SRCS}

.PHONY: lint
lint: ${SRCS} ${MANS}
	-mandoc -T lint -W warning ${MANS}
	-clang-tidy -checks=clang-analyzer-* ${SRCS} -- ${_cflags}

.PHONY: clean
clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

.PHONY: install
install: all
	mkdir -p ${bindir}
	mkdir -p ${mandir}
	install -m 755 ${PROG} ${bindir}/${PROG}
	install -m 644 ${PROG:=.1} ${mandir}/${PROG:=.1}

.PHONY: uninstall
uninstall:
	-rm ${bindir}/${PROG}
	-rm ${mandir}/${PROG:=.1}
