.POSIX:

# paths
PREFIX = /usr/local
BINDIR = $(DESTDIR)$(PREFIX)/bin
MANDIR = $(DESTDIR)$(PREFIX)/share/man/man1

# build variables
CC          = cc
FREETYPEINC = /usr/include/freetype2
# OpenBSD (uncomment)
#FREETYPEINC = /usr/X11R6/include/freetype2
CFLAGS      = -Wall -Wextra -I/usr/include -I/usr/X11R6/include\
			  -I$(FREETYPEINC)
LDFLAGS     = -L/usr/lib -L/usr/X11R6/lib -lfontconfig -lXft -lX11 -lXinerama\
			  -lImlib2

# build rules
all: xmenu
xmenu: xmenu.c config.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ xmenu.c
clean:
	@rm -f xmenu

# install rules
install: all
	mkdir -p $(BINDIR)
	cp xmenu $(BINDIR)
	chmod 755 $(BINDIR)/xmenu
	mkdir -p $(MANDIR)
	cp xmenu.1 $(MANDIR)
	chmod 644 $(MANDIR)/xmenu.1
uninstall:
	rm -f $(BINDIR)/xmenu
	rm -f $(MANDIR)/xmenu.1
