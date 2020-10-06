include config.mk

SRCS = ${PROG}.c
OBJS = ${SRCS:.c=.o}

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS}

${OBJS}: config.h ${PROG}.h

.c.o:
	${CC} ${CFLAGS} -c $<

clean:
	-rm ${OBJS} ${PROG}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/${PROG}
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	install -m 644 ${PROG}.1 ${DESTDIR}${MANPREFIX}/man1/${PROG}.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/${PROG}
	rm -f ${DESTDIR}${MANPREFIX}/man1/${PROG}.1

.PHONY: all clean install uninstall
