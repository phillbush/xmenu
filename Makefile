include config.mk

bindir = ${DESTDIR}${PREFIX}
mandir = ${DESTDIR}${MANPREFIX}

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
	mkdir -p ${bindir}/bin
	install -m 755 ${PROG} ${bindir}/bin/${PROG}
	mkdir -p ${mandir}/man1
	install -m 644 ${PROG}.1 ${mandir}/man1/${PROG}.1

uninstall:
	rm -f ${bindir}/bin/${PROG}
	rm -f ${mandir}/man1/${PROG}.1

.PHONY: all clean install uninstall
