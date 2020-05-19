include config.mk

SRCS = ${PROG}.c
OBJS = ${SRCS:.c=.o}

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS}

${OBJS}: config.h

.c.o:
	${CC} ${CFLAGS} -c $<

clean:
	-rm ${OBJS} ${PROG}

install: all
	install -d ${DESTDIR}${PREFIX}/bin/
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/
	install -m 644 ${PROG}.1 ${DESTDIR}${MANPREFIX}

uninstall:
	rm -f ${DESTDIR}/${PREFIX}/bin/${PROG}
	rm -f ${DESTDIR}/${MANPREFIX}/man1/${PROG}.1

.PHONY: all clean install uninstall
