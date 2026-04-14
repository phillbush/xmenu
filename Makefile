PROG = xmenu
OBJS = ${PROG:=.o} control/font.o
SRCS = ${OBJS:.o=.c}
MAN = ${PROG:=.1}
DOC = README.md

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
bindir = ${DESTDIR}${PREFIX}/bin
mandir = ${DESTDIR}${MANPREFIX}/man1

.PHONY: all
all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} \
	-lfontconfig -lXft -lX11 -lXinerama -lXrender -lImlib2 ${LDLIBS} \
	-L/usr{,/local,/X11R6}/lib ${LDFLAGS}

.PHONY: debug
debug:
	@${MAKE} ${MAKEFLAGS} \
	CFLAGS+="-g -O0 -DDEBUG=1 -Wall -Wextra -Wpedantic ${DEBUG}" \
	${PROG}

.c.o:
	${CC} -std=c99 -D_XOPEN_SOURCE=800 -D_BSD_SOURCE -D_GNU_SOURCE \
	-I./ -I/usr{,/local,/X11R6}/include{,/freetype2} \
	${CPPFLAGS} ${CFLAGS} -o $@ -c $<

xmenu.o control/font.o: control/font.h

.PHONY: tags
tags: ${SRCS}
	ctags ${SRCS}

.PHONY: lint
lint: ${SRCS}
	-mandoc -T lint -W warning ${MAN}
	-clang-tidy ${SRCS} -- -std=c99 ${DEFS} ${INCS} ${CPPFLAGS}

.PHONY: clean
clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

.PHONY: install
install: all
	mkdir -p ${bindir}
	mkdir -p ${mandir}
	install -m 755 ${PROG} ${bindir}/${PROG}
	install -m 644 ${MAN} ${mandir}/${MAN}

.PHONY: uninstall
uninstall:
	-rm ${bindir}/${PROG}
	-rm ${mandir}/${MAN}
