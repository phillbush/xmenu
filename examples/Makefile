PROG = ctrlmenu-example
OBJS = ../util/color.o \
	../control/application.o ../control/font.o ../control/menu.o \
	../control/resources.o

_cflags = -std=c2x -D_XOPEN_SOURCE=800 -D_BSD_SOURCE -D_GNU_SOURCE \
	-I.. -I/usr{,/local,/X11R6}/include{,/freetype2} ${CPPFLAGS} ${CFLAGS}

.PHONY: all
all: ${PROG}
${PROG}: ${@:=.c} ${OBJS}
	${CC} -o $@ ${@:=.c} ${OBJS} ${_cflags} \
	-lfontconfig -lXft -lX11 -lXrandr -lXrender -lImlib2 ${LDLIBS} \
	-L/usr{,/local,/X11R6}/lib ${LDFLAGS}

${OBJS}:
	@cd .. && ${MAKE} ${@:../%=%}

.PHONY: tags
tags: ${SRCS}
	ctags ${SRCS}

.PHONY: lint
lint: ${SRCS} ${MANS}
	-mandoc -T lint -W warning ${MANS}
	-clang-tidy -checks=clang-analyzer-* ${SRCS} -- ${_cflags}

.PHONY: clean
clean:
	rm -f ${PROG} ${PROG:=.core} tags
