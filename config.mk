# program name
PROG = xmenu

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

FREETYPEINC = /usr/include/freetype2
FREETYPELIB = -lfontconfig -lXft
# OpenBSD (uncomment)
#FREETYPEINC = $(X11INC)/freetype2

# includes and libs
INCS = -I${X11INC} -I${FREETYPEINC}
LIBS = -L${X11LIB} -L${FREETYPELIB} -lX11

# flags
CPPFLAGS =
CFLAGS = -Wall -Wextra ${INCS} ${CPPFLAGS}
LDFLAGS = ${LIBS}

# compiler and linker
CC = cc
