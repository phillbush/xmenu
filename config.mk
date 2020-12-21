# program name
PROG = xmenu

# paths
PREFIX    ?= ${DESTDIR}/usr/local
MANPREFIX ?= ${PREFIX}/share/man

LOCALINC ?= ${DESTDIR}/usr/local/include
LOCALLIB ?= ${DESTDIR}/usr/local/lib

X11INC ?= ${DESTDIR}/usr/X11R6/include
X11LIB ?= ${DESTDIR}/usr/X11R6/lib

FREETYPEINC ?= ${DESTDIR}/usr/include/freetype2
# OpenBSD (uncomment)
#FREETYPEINC = ${X11INC}/freetype2

# includes and libs
INCS := -I${LOCALINC} -I${X11INC} -I${FREETYPEINC}
LIBS := -L${LOCALLIB} -L${X11LIB} -lfontconfig -lXft -lX11 -lXinerama -lImlib2

# flags
CPPFLAGS :=
CFLAGS   := -Wall -Wextra ${INCS} ${CPPFLAGS}
LDFLAGS  := ${LIBS}

# compiler and linker
CC = cc
