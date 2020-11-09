#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <Imlib2.h>
#include "xmenu.h"

/* X stuff */
static Display *dpy;
static int screen;
static Visual *visual;
static Window rootwin;
static Colormap colormap;
static struct DC dc;
static struct Monitor mon;
static Atom utf8string;
static Atom wmdelete;
static Atom netatom[NetLast];

/* flags */
static int iflag = 0;   /* whether to disable icons */
static int rflag = 0;   /* whether to disable right-click */
static int mflag = 0;   /* whether the user specified a monitor with -p */
static int pflag = 0;   /* whether the user specified a position with -p */
static int wflag = 0;   /* whether to let the window manager control XMenu */

/* include config variable */
#include "config.h"

/* show usage */
static void
usage(void)
{
	(void)fprintf(stderr, "usage: xmenu [-irw] [-p position] [title]\n");
	exit(1);
}

/* parse position string from -p,
 * put results on config.posx, config.posy, and config.monitor */
static void
parseposition(char *optarg)
{
	long n;
	char *s = optarg;
	char *endp;

	n = strtol(s, &endp, 10);
	if (errno == ERANGE || n > INT_MAX || n < 0 || endp == s || *endp != 'x')
		goto error;
	config.posx = n;
	s = endp+1;
	n = strtol(s, &endp, 10);
	if (errno == ERANGE || n > INT_MAX || n < 0 || endp == s)
		goto error;
	config.posy = n;
	if (*endp == ':') {
		s = endp+1;
		mflag = 1;
		if (strncasecmp(s, "CUR", 3) == 0) {
			config.monitor = -1;
			endp = s+3;
		} else {
			n = strtol(s, &endp, 10);
			if (errno == ERANGE || n > INT_MAX || n < 0 || endp == s || *endp != '\0')
				goto error;
			config.monitor = n;
		}
	} else if (*endp != '\0') {
		goto error;
	}

	return;

error:
	errx(1, "improper position: %s", optarg);
}

/* parse font string */
static void
parsefonts(const char *s)
{
	const char *p;
	char buf[1024];
	size_t nfont = 0;

	dc.nfonts = 1;
	for (p = s; *p; p++)
		if (*p == ',')
			dc.nfonts++;

	if ((dc.fonts = calloc(dc.nfonts, sizeof *dc.fonts)) == NULL)
		err(1, "calloc");

	p = s;
	while (*p != '\0') {
		size_t i;

		i = 0;
		while (isspace(*p))
			p++;
		while (i < sizeof buf && *p != '\0' && *p != ',')
			buf[i++] = *p++;
		if (i >= sizeof buf)
			errx(1, "font name too long");
		if (*p == ',')
			p++;
		buf[i] = '\0';
		if (nfont == 0)
			if ((dc.pattern = FcNameParse((FcChar8 *)buf)) == NULL)
				errx(1, "the first font in the cache must be loaded from a font string");
		if ((dc.fonts[nfont++] = XftFontOpenName(dpy, screen, buf)) == NULL)
			errx(1, "could not load font");
	}
}

/* get color from color string */
static void
ealloccolor(const char *s, XftColor *color)
{
	if(!XftColorAllocName(dpy, visual, colormap, s, color))
		errx(1, "could not allocate color: %s", s);
}

/* query monitor information and cursor position */
static void
initmonitor(void)
{
	XineramaScreenInfo *info = NULL;
	Window dw;          /* dummy variable */
	int di;             /* dummy variable */
	unsigned du;        /* dummy variable */
	int cursx, cursy;   /* cursor position */
	int nmons;
	int i;

	XQueryPointer(dpy, rootwin, &dw, &dw, &cursx, &cursy, &di, &di, &du);

	mon.x = mon.y = 0;
	mon.w = DisplayWidth(dpy, screen);
	mon.h = DisplayHeight(dpy, screen);

	if ((info = XineramaQueryScreens(dpy, &nmons)) != NULL) {
		int selmon = 0;

		if (!mflag || config.monitor < 0 || config.monitor >= nmons) {
			for (i = 0; i < nmons; i++) {
				if (BETWEEN(cursx, info[i].x_org, info[i].x_org + info[i].width) &&
				    BETWEEN(cursy, info[i].y_org, info[i].y_org + info[i].height)) {
					selmon = i;
					break;
				}
			}
		} else {
			selmon = config.monitor;
		}

		mon.x = info[selmon].x_org;
		mon.y = info[selmon].y_org;
		mon.w = info[selmon].width;
		mon.h = info[selmon].height;

		XFree(info);
	}

	if (!pflag) {
		config.posx = cursx;
		config.posy = cursy;
	} else if (mflag) {
		config.posx += mon.x;
		config.posy += mon.y;
	}
}

/* read xrdb for configuration options */
static void
initresources(void)
{
	long n;
	char *type;
	char *xrm;
	XrmDatabase xdb;
	XrmValue xval;

	XrmInitialize();
	if ((xrm = XResourceManagerString(dpy)) == NULL)
		return;

	xdb = XrmGetStringDatabase(xrm);

	if (XrmGetResource(xdb, "xmenu.borderWidth", "*", &type, &xval) == True)
		if ((n = strtol(xval.addr, NULL, 10)) > 0)
			config.border_pixels = n;
	if (XrmGetResource(xdb, "xmenu.separatorWidth", "*", &type, &xval) == True)
		if ((n = strtol(xval.addr, NULL, 10)) > 0)
			config.separator_pixels = n;
	if (XrmGetResource(xdb, "xmenu.height", "*", &type, &xval) == True)
		if ((n = strtol(xval.addr, NULL, 10)) > 0)
			config.height_pixels = n;
	if (XrmGetResource(xdb, "xmenu.width", "*", &type, &xval) == True)
		if ((n = strtol(xval.addr, NULL, 10)) > 0)
			config.width_pixels = n;
	if (XrmGetResource(xdb, "xmenu.gap", "*", &type, &xval) == True)
		if ((n = strtol(xval.addr, NULL, 10)) > 0)
			config.gap_pixels = n;
	if (XrmGetResource(xdb, "xmenu.background", "*", &type, &xval) == True)
		config.background_color = strdup(xval.addr);
	if (XrmGetResource(xdb, "xmenu.foreground", "*", &type, &xval) == True)
		config.foreground_color = strdup(xval.addr);
	if (XrmGetResource(xdb, "xmenu.selbackground", "*", &type, &xval) == True)
		config.selbackground_color = strdup(xval.addr);
	if (XrmGetResource(xdb, "xmenu.selforeground", "*", &type, &xval) == True)
		config.selforeground_color = strdup(xval.addr);
	if (XrmGetResource(xdb, "xmenu.separator", "*", &type, &xval) == True)
		config.separator_color = strdup(xval.addr);
	if (XrmGetResource(xdb, "xmenu.border", "*", &type, &xval) == True)
		config.border_color = strdup(xval.addr);
	if (XrmGetResource(xdb, "xmenu.font", "*", &type, &xval) == True)
		config.font = strdup(xval.addr);

	XrmDestroyDatabase(xdb);
}

/* init draw context */
static void
initdc(void)
{
	/* get color pixels */
	ealloccolor(config.background_color,    &dc.normal[ColorBG]);
	ealloccolor(config.foreground_color,    &dc.normal[ColorFG]);
	ealloccolor(config.selbackground_color, &dc.selected[ColorBG]);
	ealloccolor(config.selforeground_color, &dc.selected[ColorFG]);
	ealloccolor(config.separator_color,     &dc.separator);
	ealloccolor(config.border_color,        &dc.border);

	/* parse fonts */
	parsefonts(config.font);

	/* create common GC */
	dc.gc = XCreateGC(dpy, rootwin, 0, NULL);
}

/* calculate icon size */
static void
initiconsize(void)
{
	config.iconsize = config.height_pixels - config.iconpadding * 2;
}

/* intern atoms */
static void
initatoms(void)
{
	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	wmdelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypePopupMenu] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
}

/* allocate an item */
static struct Item *
allocitem(const char *label, const char *output, char *file)
{
	struct Item *item;

	if ((item = malloc(sizeof *item)) == NULL)
		err(1, "malloc");
	if (label == NULL) {
		item->label = NULL;
		item->output = NULL;
	} else {
		if ((item->label = strdup(label)) == NULL)
			err(1, "strdup");
		if (label == output) {
			item->output = item->label;
		} else {
			if ((item->output = strdup(output)) == NULL)
				err(1, "strdup");
		}
	}
	if (file == NULL) {
		item->file = NULL;
	} else {
		if ((item->file = strdup(file)) == NULL)
			err(1, "strdup");
	}
	item->y = 0;
	item->h = 0;
	item->next = NULL;
	item->submenu = NULL;
	item->icon = NULL;

	return item;
}

/* allocate a menu and create its window */
static struct Menu *
allocmenu(struct Menu *parent, struct Item *list, unsigned level)
{
	XSetWindowAttributes swa;
	struct Menu *menu;

	if ((menu = malloc(sizeof *menu)) == NULL)
		err(1, "malloc");
	menu->parent = parent;
	menu->list = list;
	menu->caller = NULL;
	menu->selected = NULL;
	menu->w = 0;        /* recalculated by setupmenu() */
	menu->h = 0;        /* recalculated by setupmenu() */
	menu->x = mon.x;    /* recalculated by setupmenu() */
	menu->y = mon.y;    /* recalculated by setupmenu() */
	menu->level = level;
	menu->drawn = 0;
	menu->hasicon = 0;

	swa.override_redirect = (wflag) ? False : True;
	swa.background_pixel = dc.normal[ColorBG].pixel;
	swa.border_pixel = dc.border.pixel;
	swa.save_under = True;  /* pop-up windows should save_under*/
	swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask
	               | PointerMotionMask | LeaveWindowMask;
	if (wflag)
		swa.event_mask |= StructureNotifyMask;
	menu->win = XCreateWindow(dpy, rootwin, 0, 0, 1, 1, 0,
	                          CopyFromParent, CopyFromParent, CopyFromParent,
	                          CWOverrideRedirect | CWBackPixel |
	                          CWBorderPixel | CWEventMask | CWSaveUnder,
	                          &swa);

	return menu;
}

/* build the menu tree */
static struct Menu *
buildmenutree(unsigned level, const char *label, const char *output, char *file)
{
	static struct Menu *prevmenu = NULL;    /* menu the previous item was added to */
	static struct Menu *rootmenu = NULL;    /* menu to be returned */
	struct Item *curritem = NULL;           /* item currently being read */
	struct Item *item;                      /* dummy item for loops */
	struct Menu *menu;                      /* dummy menu for loops */
	unsigned i;

	/* create the item */
	curritem = allocitem(label, output, file);

	/* put the item in the menu tree */
	if (prevmenu == NULL) {                 /* there is no menu yet */
		menu = allocmenu(NULL, curritem, level);
		rootmenu = menu;
		prevmenu = menu;
		curritem->prev = NULL;
	} else if (level < prevmenu->level) {   /* item is continuation of a parent menu */
		/* go up the menu tree until find the menu this item continues */
		for (menu = prevmenu, i = level;
			  menu != NULL && i != prevmenu->level;
			  menu = menu->parent, i++)
			;
		if (menu == NULL)
			errx(1, "improper indentation detected");

		/* find last item in the new menu */
		for (item = menu->list; item->next != NULL; item = item->next)
			;

		prevmenu = menu;
		item->next = curritem;
		curritem->prev = item;
	} else if (level == prevmenu->level) {  /* item is a continuation of current menu */
		/* find last item in the previous menu */
		for (item = prevmenu->list; item->next != NULL; item = item->next)
			;

		item->next = curritem;
		curritem->prev = item;
	} else if (level > prevmenu->level) {   /* item begins a new menu */
		menu = allocmenu(prevmenu, curritem, level);

		/* find last item in the previous menu */
		for (item = prevmenu->list; item->next != NULL; item = item->next)
			;

		prevmenu = menu;
		menu->caller = item;
		item->submenu = menu;
		curritem->prev = NULL;
	}

	if (curritem->file)
		prevmenu->hasicon = 1;

	return rootmenu;
}

/* create menus and items from the stdin */
static struct Menu *
parsestdin(void)
{
	struct Menu *rootmenu;
	char *s, buf[BUFSIZ];
	char *file, *label, *output;
	unsigned level = 0;

	rootmenu = NULL;

	while (fgets(buf, BUFSIZ, stdin) != NULL) {
		/* get the indentation level */
		level = strspn(buf, "\t");

		/* get the label */
		s = level + buf;
		label = strtok(s, "\t\n");

		/* get the filename */
		file = NULL;
		if (label != NULL && strncmp(label, "IMG:", 4) == 0) {
			file = label + 4;
			label = strtok(NULL, "\t\n");
		}

		/* get the output */
		output = strtok(NULL, "\n");
		if (output == NULL) {
			output = label;
		} else {
			while (*output == '\t')
				output++;
		}

		rootmenu = buildmenutree(level, label, output, file);
	}

	return rootmenu;
}

/* get next utf8 char from s return its codepoint and set next_ret to pointer to end of character */
static FcChar32
getnextutf8char(const char *s, const char **next_ret)
{
	static const unsigned char utfbyte[] = {0x80, 0x00, 0xC0, 0xE0, 0xF0};
	static const unsigned char utfmask[] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
	static const FcChar32 utfmin[] = {0, 0x00,  0x80,  0x800,  0x10000};
	static const FcChar32 utfmax[] = {0, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};
	/* 0xFFFD is the replacement character, used to represent unknown characters */
	static const FcChar32 unknown = 0xFFFD;
	FcChar32 ucode;         /* FcChar32 type holds 32 bits */
	size_t usize = 0;       /* n' of bytes of the utf8 character */
	size_t i;

	*next_ret = s+1;

	/* get code of first byte of utf8 character */
	for (i = 0; i < sizeof utfmask; i++) {
		if (((unsigned char)*s & utfmask[i]) == utfbyte[i]) {
			usize = i;
			ucode = (unsigned char)*s & ~utfmask[i];
			break;
		}
	}

	/* if first byte is a continuation byte or is not allowed, return unknown */
	if (i == sizeof utfmask || usize == 0)
		return unknown;

	/* check the other usize-1 bytes */
	s++;
	for (i = 1; i < usize; i++) {
		*next_ret = s+1;
		/* if byte is nul or is not a continuation byte, return unknown */
		if (*s == '\0' || ((unsigned char)*s & utfmask[0]) != utfbyte[0])
			return unknown;
		/* 6 is the number of relevant bits in the continuation byte */
		ucode = (ucode << 6) | ((unsigned char)*s & ~utfmask[0]);
		s++;
	}

	/* check if ucode is invalid or in utf-16 surrogate halves */
	if (!BETWEEN(ucode, utfmin[usize], utfmax[usize])
	    || BETWEEN (ucode, 0xD800, 0xDFFF))
		return unknown;

	return ucode;
}

/* get which font contains a given code point */
static XftFont *
getfontucode(FcChar32 ucode)
{
	FcCharSet *fccharset = NULL;
	FcPattern *fcpattern = NULL;
	FcPattern *match = NULL;
	XftFont *retfont = NULL;
	XftResult result;
	size_t i;

	for (i = 0; i < dc.nfonts; i++)
		if (XftCharExists(dpy, dc.fonts[i], ucode) == FcTrue)
			return dc.fonts[i];

	/* create a charset containing our code point */
	fccharset = FcCharSetCreate();
	FcCharSetAddChar(fccharset, ucode);

	/* create a pattern akin to the dc.pattern but containing our charset */
	if (fccharset) {
		fcpattern = FcPatternDuplicate(dc.pattern);
		FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
	}

	/* find pattern matching fcpattern */
	if (fcpattern) {
		FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
		FcDefaultSubstitute(fcpattern);
		match = XftFontMatch(dpy, screen, fcpattern, &result);
	}

	/* if found a pattern, open its font */
	if (match) {
		retfont = XftFontOpenPattern(dpy, match);
		if (retfont && XftCharExists(dpy, retfont, ucode) == FcTrue) {
			if ((dc.fonts = realloc(dc.fonts, dc.nfonts+1)) == NULL)
				err(1, "realloc");
			dc.fonts[dc.nfonts] = retfont;
			return dc.fonts[dc.nfonts++];
		} else {
			XftFontClose(dpy, retfont);
		}
	}

	/* in case no fount was found, return the first one */
	return dc.fonts[0];
}

/* draw text into XftDraw, return width of text glyphs */
static int
drawtext(XftDraw *draw, XftColor *color, int x, int y, unsigned h, const char *text)
{
	int textwidth = 0;

	while (*text) {
		XftFont *currfont;
		XGlyphInfo ext;
		FcChar32 ucode;
		const char *next;
		size_t len;

		ucode = getnextutf8char(text, &next);
		currfont = getfontucode(ucode);

		len = next - text;
		XftTextExtentsUtf8(dpy, currfont, (XftChar8 *)text, len, &ext);
		textwidth += ext.xOff;

		if (draw) {
			int texty;

			texty = y + (h - (currfont->ascent + currfont->descent))/2 + currfont->ascent;
			XftDrawStringUtf8(draw, color, currfont, x, texty, (XftChar8 *)text, len);
			x += ext.xOff;
		}

		text = next;
	}

	return textwidth;
}

/* setup the height, width and icon of the items of a menu */
static void
setupitems(struct Menu *menu)
{
	struct Item *item;

	menu->w = config.width_pixels;
	for (item = menu->list; item != NULL; item = item->next) {
		int itemwidth;
		int textwidth;

		item->y = menu->h;

		if (item->label == NULL)   /* height for separator item */
			item->h = config.separator_pixels;
		else
			item->h = config.height_pixels;
		menu->h += item->h;

		if (item->label)
			textwidth = drawtext(NULL, NULL, 0, 0, 0, item->label);
		else
			textwidth = 0;

		/*
		 * set menu width
		 *
		 * the item width depends on the size of its label (textwidth),
		 * and it is only used to calculate the width of the menu (which
		 * is equal to the width of the largest item).
		 *
		 * the horizontal padding appears 4 times through the width of a
		 * item: before and after its icon, and before and after its triangle.
		 * if the iflag is set (icons are disabled) then the horizontal
		 * padding appears 3 times: before the label and around the triangle.
		 */
		itemwidth = textwidth + config.triangle_width + config.horzpadding * 3;
		itemwidth += (iflag || !menu->hasicon) ? 0 : config.iconsize + config.horzpadding;
		menu->w = MAX(menu->w, itemwidth);
	}
}

/* setup the position of a menu */
static void
setupmenupos(struct Menu *menu)
{
	int width, height;

	width = menu->w + config.border_pixels * 2;
	height = menu->h + config.border_pixels * 2;
	if (menu->parent == NULL) { /* if root menu, calculate in respect to cursor */
		if (pflag || (config.posx >= mon.x && mon.x + mon.w - config.posx >= width))
			menu->x = config.posx;
		else if (config.posx > width)
			menu->x = config.posx - width;

		if (pflag || (config.posy >= mon.y && mon.y + mon.h - config.posy >= height))
			menu->y = config.posy;
		else if (mon.y + mon.h > height)
			menu->y = mon.y + mon.h - height;
	} else {                    /* else, calculate in respect to parent menu */
		int parentwidth;

		parentwidth = menu->parent->x + menu->parent->w + config.border_pixels + config.gap_pixels;

		if (mon.x + mon.w - parentwidth >= width)
			menu->x = parentwidth;
		else if (menu->parent->x > menu->w + config.border_pixels + config.gap_pixels)
			menu->x = menu->parent->x - menu->w - config.border_pixels - config.gap_pixels;

		if (mon.y + mon.h - (menu->caller->y + menu->parent->y) >= height)
			menu->y = menu->caller->y + menu->parent->y;
		else if (mon.y + mon.h > height)
			menu->y = mon.y + mon.h - height;
	}
}

/* recursivelly setup menu configuration and its pixmap */
static void
setupmenu(struct Menu *menu, XClassHint *classh)
{
	char *title;
	struct Item *item;
	XWindowChanges changes;
	XSizeHints sizeh;
	XTextProperty wintitle;

	/* setup size and position of menus */
	setupitems(menu);
	setupmenupos(menu);

	/* update menu geometry */
	changes.border_width = config.border_pixels;
	changes.height = menu->h;
	changes.width = menu->w;
	changes.x = menu->x;
	changes.y = menu->y;
	XConfigureWindow(dpy, menu->win, CWBorderWidth | CWWidth | CWHeight | CWX | CWY, &changes);

	/* set window title (used if wflag is on) */
	if (menu->parent == NULL) {
		title = classh->res_name;
	} else {
		title = menu->caller->output;
	}
	XStringListToTextProperty(&title, 1, &wintitle);

	/* set window manager hints */
	sizeh.flags = USPosition | PMaxSize | PMinSize;
	sizeh.min_width = sizeh.max_width = menu->w;
	sizeh.min_height = sizeh.max_height = menu->h;
	XSetWMProperties(dpy, menu->win, &wintitle, NULL, NULL, 0, &sizeh, NULL, classh);

	/* set WM protocols and ewmh window properties */
	XSetWMProtocols(dpy, menu->win, &wmdelete, 1);
	XChangeProperty(dpy, menu->win, netatom[NetWMName], utf8string, 8,
	                PropModeReplace, (unsigned char *)title, strlen(title));
	XChangeProperty(dpy, menu->win, netatom[NetWMWindowType], XA_ATOM, 32,
	                PropModeReplace,
	                (unsigned char *)&netatom[NetWMWindowTypePopupMenu], 1);

	/* calculate positions of submenus */
	for (item = menu->list; item != NULL; item = item->next) {
		if (item->submenu != NULL)
			setupmenu(item->submenu, classh);
	}
}

/* try to grab pointer, we may have to wait for another process to ungrab */
static void
grabpointer(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	for (i = 0; i < 1000; i++) {
		if (XGrabPointer(dpy, rootwin, True, ButtonPressMask,
		                 GrabModeAsync, GrabModeAsync, None,
		                 None, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	errx(1, "could not grab pointer");
}

/* try to grab keyboard, we may have to wait for another process to ungrab */
static void
grabkeyboard(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, rootwin, True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	errx(1, "could not grab keyboard");
}

/* load and scale icon */
static Imlib_Image
loadicon(const char *file)
{
	Imlib_Image icon;
	Imlib_Load_Error errcode;
	const char *errstr;
	int width;
	int height;
	int imgsize;

	icon = imlib_load_image_with_error_return(file, &errcode);
	if (*file == '\0') {
		warnx("could not load icon (file name is blank)");
		return NULL;
	} else if (icon == NULL) {
		switch (errcode) {
		case IMLIB_LOAD_ERROR_FILE_DOES_NOT_EXIST:
			errstr = "file does not exist";
			break;
		case IMLIB_LOAD_ERROR_FILE_IS_DIRECTORY:
			errstr = "file is directory";
			break;
		case IMLIB_LOAD_ERROR_PERMISSION_DENIED_TO_READ:
		case IMLIB_LOAD_ERROR_PERMISSION_DENIED_TO_WRITE:
			errstr = "permission denied";
			break;
		case IMLIB_LOAD_ERROR_NO_LOADER_FOR_FILE_FORMAT:
			errstr = "unknown file format";
			break;
		case IMLIB_LOAD_ERROR_PATH_TOO_LONG:
			errstr = "path too long";
			break;
		case IMLIB_LOAD_ERROR_PATH_COMPONENT_NON_EXISTANT:
		case IMLIB_LOAD_ERROR_PATH_COMPONENT_NOT_DIRECTORY:
		case IMLIB_LOAD_ERROR_PATH_POINTS_OUTSIDE_ADDRESS_SPACE:
			errstr = "improper path";
			break;
		case IMLIB_LOAD_ERROR_TOO_MANY_SYMBOLIC_LINKS:
			errstr = "too many symbolic links";
			break;
		case IMLIB_LOAD_ERROR_OUT_OF_MEMORY:
			errstr = "out of memory";
			break;
		case IMLIB_LOAD_ERROR_OUT_OF_FILE_DESCRIPTORS:
			errstr = "out of file descriptors";
			break;
		default:
			errstr = "unknown error";
			break;
		}
		warnx("could not load icon (%s): %s", errstr, file);
		return NULL;
	}

	imlib_context_set_image(icon);

	width = imlib_image_get_width();
	height = imlib_image_get_height();
	imgsize = MIN(width, height);

	icon = imlib_create_cropped_scaled_image(0, 0, imgsize, imgsize,
	                                         config.iconsize,
	                                         config.iconsize);

	return icon;
}

/* draw pixmap for the selected and unselected version of each item on menu */
static void
drawitems(struct Menu *menu)
{
	struct Item *item;

	for (item = menu->list; item != NULL; item = item->next) {
		XftDraw *dsel, *dunsel;
		int x, y;

		item->unsel = XCreatePixmap(dpy, menu->win, menu->w, item->h,
		                          DefaultDepth(dpy, screen));

		XSetForeground(dpy, dc.gc, dc.normal[ColorBG].pixel);
		XFillRectangle(dpy, item->unsel, dc.gc, 0, 0, menu->w, item->h);

		if (item->label == NULL) { /* item is separator */
			y = item->h/2;
			XSetForeground(dpy, dc.gc, dc.separator.pixel);
			XDrawLine(dpy, item->unsel, dc.gc, config.horzpadding, y,
			          menu->w - config.horzpadding, y);

			item->sel = item->unsel;
		} else {

			item->sel = XCreatePixmap(dpy, menu->win, menu->w, item->h,
			                          DefaultDepth(dpy, screen));
			XSetForeground(dpy, dc.gc, dc.selected[ColorBG].pixel);
			XFillRectangle(dpy, item->sel, dc.gc, 0, 0, menu->w, item->h);

			/* draw text */
			x = config.horzpadding;
			x += (iflag || !menu->hasicon) ? 0 : config.horzpadding + config.iconsize;
			dsel = XftDrawCreate(dpy, item->sel, visual, colormap);
			dunsel = XftDrawCreate(dpy, item->unsel, visual, colormap);
			XSetForeground(dpy, dc.gc, dc.selected[ColorFG].pixel);
			drawtext(dsel, &dc.selected[ColorFG], x, 0, item->h, item->label);
			XSetForeground(dpy, dc.gc, dc.normal[ColorFG].pixel);
			drawtext(dunsel, &dc.normal[ColorFG], x, 0, item->h, item->label);
			XftDrawDestroy(dsel);
			XftDrawDestroy(dunsel);

			/* draw triangle */
			if (item->submenu != NULL) {
				x = menu->w - config.triangle_width - config.horzpadding;
				y = (item->h - config.triangle_height + 1) / 2;

				XPoint triangle[] = {
					{x, y},
					{x + config.triangle_width, y + config.triangle_height/2},
					{x, y + config.triangle_height},
					{x, y}
				};

				XSetForeground(dpy, dc.gc, dc.selected[ColorFG].pixel);
				XFillPolygon(dpy, item->sel, dc.gc, triangle, LEN(triangle),
				             Convex, CoordModeOrigin);
				XSetForeground(dpy, dc.gc, dc.normal[ColorFG].pixel);
				XFillPolygon(dpy, item->unsel, dc.gc, triangle, LEN(triangle),
				             Convex, CoordModeOrigin);
			}

			/* try to load icon */
			if (item->file && !iflag) {
				item->icon = loadicon(item->file);
				free(item->file);
			}

			/* draw icon if properly loaded */
			if (item->icon) {
				imlib_context_set_image(item->icon);
				imlib_context_set_drawable(item->sel);
				imlib_render_image_on_drawable(config.horzpadding, config.iconpadding);
				imlib_context_set_drawable(item->unsel);
				imlib_render_image_on_drawable(config.horzpadding, config.iconpadding);
				imlib_context_set_image(item->icon);
				imlib_free_image();
			}
		}
	}
}

/* copy pixmaps of items of the current menu and of its ancestors into menu window */
static void
drawmenus(struct Menu *currmenu)
{
	struct Menu *menu;
	struct Item *item;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		if (!menu->drawn) {
			drawitems(menu);
			menu->drawn = 1;
		}
		for (item = menu->list; item != NULL; item = item->next) {
			if (item == menu->selected)
				XCopyArea(dpy, item->sel, menu->win, dc.gc, 0, 0,
				          menu->w, item->h, 0, item->y);
			else
				XCopyArea(dpy, item->unsel, menu->win, dc.gc, 0, 0,
				          menu->w, item->h, 0, item->y);
		}
	}
}

/* umap previous menus and map current menu and its parents */
static void
mapmenu(struct Menu *currmenu)
{
	static struct Menu *prevmenu = NULL;
	struct Menu *menu, *menu_;
	struct Menu *lcamenu;   /* lowest common ancestor menu */
	unsigned minlevel;      /* level of the closest to root menu */
	unsigned maxlevel;      /* level of the closest to root menu */

	/* do not remap current menu if it wasn't updated*/
	if (prevmenu == currmenu)
		return;

	/* if this is the first time mapping, skip calculations */
	if (prevmenu == NULL) {
		XMapWindow(dpy, currmenu->win);
		prevmenu = currmenu;
		return;
	}

	/* find lowest common ancestor menu */
	minlevel = MIN(currmenu->level, prevmenu->level);
	maxlevel = MAX(currmenu->level, prevmenu->level);
	if (currmenu->level == maxlevel) {
		menu = currmenu;
		menu_ = prevmenu;
	} else {
		menu = prevmenu;
		menu_ = currmenu;
	}
	while (menu->level > minlevel)
		menu = menu->parent;
	while (menu != menu_) {
		menu = menu->parent;
		menu_ = menu_->parent;
	}
	lcamenu = menu;

	/* unmap menus from currmenu (inclusive) until lcamenu (exclusive) */
	for (menu = prevmenu; menu != lcamenu; menu = menu->parent) {
		menu->selected = NULL;
		XUnmapWindow(dpy, menu->win);
	}

	/* map menus from currmenu (inclusive) until lcamenu (exclusive) */
	for (menu = currmenu; menu != lcamenu; menu = menu->parent) {

		if (wflag) {
			setupmenupos(menu);
			XMoveWindow(dpy, menu->win, menu->x, menu->y);
		}

		XMapWindow(dpy, menu->win);
	}

	prevmenu = currmenu;
}

/* get menu of given window */
static struct Menu *
getmenu(struct Menu *currmenu, Window win)
{
	struct Menu *menu;

	for (menu = currmenu; menu != NULL; menu = menu->parent)
		if (menu->win == win)
			return menu;

	return NULL;
}

/* get item of given menu and position */
static struct Item *
getitem(struct Menu *menu, int y)
{
	struct Item *item;

	if (menu == NULL)
		return NULL;

	for (item = menu->list; item != NULL; item = item->next)
		if (y >= item->y && y <= item->y + item->h)
			return item;

	return NULL;
}

/* cycle through the items; non-zero direction is next, zero is prev */
static struct Item *
itemcycle(struct Menu *currmenu, int direction)
{
	struct Item *item = NULL;
	struct Item *lastitem;

	for (lastitem = currmenu->list; lastitem && lastitem->next; lastitem = lastitem->next)
		;

	/* select item (either separator or labeled item) in given direction */
	switch (direction) {
	case ITEMNEXT:
		if (currmenu->selected == NULL)
			item = currmenu->list;
		else if (currmenu->selected->next != NULL)
			item = currmenu->selected->next;
		break;
	case ITEMPREV:
		if (currmenu->selected == NULL)
			item = lastitem;
		else if (currmenu->selected->prev != NULL)
			item = currmenu->selected->prev;
		break;
	case ITEMFIRST:
		item = currmenu->list;
		break;
	case ITEMLAST:
		item = lastitem;
		break;
	}

	/*
	 * the selected item can be a separator
	 * let's select the closest labeled item (ie., one that isn't a separator)
	 */
	switch (direction) {
	case ITEMNEXT:
	case ITEMFIRST:
		while (item != NULL && item->label == NULL)
			item = item->next;
		if (item == NULL)
			item = currmenu->list;
		break;
	case ITEMPREV:
	case ITEMLAST:
		while (item != NULL && item->label == NULL)
			item = item->prev;
		if (item == NULL)
			item = lastitem;
		break;
	}

	return item;
}

/* check if button is used to open a item on click */
static int
isclickbutton(unsigned int button)
{
	if (button == Button1)
		return 1;
	if (!rflag && button == Button3)
		return 1;
	return 0;
}

/* run event loop */
static void
run(struct Menu *currmenu)
{
	struct Menu *menu;
	struct Item *item;
	struct Item *previtem = NULL;
	struct Item *lastitem;
	KeySym ksym;
	XEvent ev;

	mapmenu(currmenu);

	while (!XNextEvent(dpy, &ev)) {
		switch(ev.type) {
		case Expose:
			if (ev.xexpose.count == 0)
				drawmenus(currmenu);
			break;
		case MotionNotify:
			menu = getmenu(currmenu, ev.xbutton.window);
			item = getitem(menu, ev.xbutton.y);
			if (menu == NULL || item == NULL || previtem == item)
				break;
			previtem = item;
			menu->selected = item;
			if (item->submenu != NULL) {
				currmenu = item->submenu;
				currmenu->selected = NULL;
			} else {
				currmenu = menu;
			}
			mapmenu(currmenu);
			drawmenus(currmenu);
			break;
		case ButtonRelease:
			if (!isclickbutton(ev.xbutton.button))
				break;
			menu = getmenu(currmenu, ev.xbutton.window);
			item = getitem(menu, ev.xbutton.y);
			if (menu == NULL || item == NULL)
				break;
selectitem:
			if (item->label == NULL)
				break;  /* ignore separators */
			if (item->submenu != NULL) {
				currmenu = item->submenu;
			} else {
				printf("%s\n", item->output);
				return;
			}
			mapmenu(currmenu);
			currmenu->selected = currmenu->list;
			drawmenus(currmenu);
			break;
		case ButtonPress:
			menu = getmenu(currmenu, ev.xbutton.window);
			if (menu == NULL)
				return;
			break;
		case KeyPress:
			ksym = XkbKeycodeToKeysym(dpy, ev.xkey.keycode, 0, 0);

			/* esc closes xmenu when current menu is the root menu */
			if (ksym == XK_Escape && currmenu->parent == NULL)
				return;

			/* Shift-Tab = ISO_Left_Tab */
			if (ksym == XK_Tab && (ev.xkey.state & ShiftMask))
				ksym = XK_ISO_Left_Tab;

			/* cycle through menu */
			item = NULL;
			if (ksym == XK_Home || ksym == KSYMFIRST) {
				item = itemcycle(currmenu, ITEMFIRST);
			} else if (ksym == XK_End || ksym == KSYMLAST) {
				item = itemcycle(currmenu, ITEMLAST);
			} else if (ksym == XK_ISO_Left_Tab || ksym == XK_Up || ksym == KSYMUP) {
				item = itemcycle(currmenu, ITEMPREV);
			} else if (ksym == XK_Tab || ksym == XK_Down || ksym == KSYMDOWN) {
				item = itemcycle(currmenu, ITEMNEXT);
			} else if (ksym >= XK_1 && ksym <= XK_9){
				item = itemcycle(currmenu, ITEMFIRST);
				lastitem = itemcycle(currmenu, ITEMLAST);
				for (int i = ksym - XK_1; i > 0 && item != lastitem; i--) {
					currmenu->selected = item;
					item = itemcycle(currmenu, ITEMNEXT);
				}
			} else if ((ksym == XK_Return || ksym == XK_Right || ksym == KSYMRIGHT) &&
			            currmenu->selected != NULL) {
				item = currmenu->selected;
				goto selectitem;
			} else if ((ksym == XK_Escape || ksym == XK_Left || ksym == KSYMLEFT) &&
			           currmenu->parent != NULL) {
				item = currmenu->parent->selected;
				currmenu = currmenu->parent;
				mapmenu(currmenu);
			} else
				break;
			currmenu->selected = item;
			drawmenus(currmenu);
			break;
		case LeaveNotify:
			previtem = NULL;
			currmenu->selected = NULL;
			drawmenus(currmenu);
			break;
		case ConfigureNotify:
			menu = getmenu(currmenu, ev.xconfigure.window);
			if (menu == NULL)
				break;
			menu->x = ev.xconfigure.x;
			menu->y = ev.xconfigure.y;
			break;
		case ClientMessage:
			if ((unsigned long) ev.xclient.data.l[0] != wmdelete)
				break;
			/* user closed window */
			menu = getmenu(currmenu, ev.xclient.window);
			if (menu->parent == NULL)
				return;     /* closing the root menu closes the program */
			currmenu = menu->parent;
			mapmenu(currmenu);
			break;
		}
	}
}

/* recursivelly free pixmaps and destroy windows */
static void
cleanmenu(struct Menu *menu)
{
	struct Item *item;
	struct Item *tmp;

	item = menu->list;
	while (item != NULL) {
		if (item->submenu != NULL)
			cleanmenu(item->submenu);
		tmp = item;
		if (menu->drawn) {
			XFreePixmap(dpy, item->unsel);
			if (tmp->label != NULL)
				XFreePixmap(dpy, item->sel);
		}
		if (tmp->label != tmp->output)
			free(tmp->label);
		free(tmp->output);
		item = item->next;
		free(tmp);
	}

	XDestroyWindow(dpy, menu->win);
	free(menu);
}

/* cleanup X and exit */
static void
cleanup(void)
{
	size_t i;

	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);

	XftColorFree(dpy, visual, colormap, &dc.normal[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.normal[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.separator);
	XftColorFree(dpy, visual, colormap, &dc.border);

	for (i = 0; i < dc.nfonts; i++)
		XftFontClose(dpy, dc.fonts[i]);

	XFreeGC(dpy, dc.gc);
	XCloseDisplay(dpy);
}

/* xmenu: generate menu from stdin and print selected entry to stdout */
int
main(int argc, char *argv[])
{
	struct Menu *rootmenu;
	XClassHint classh;
	int ch;

	while ((ch = getopt(argc, argv, "ip:rw")) != -1) {
		switch (ch) {
		case 'i':
			iflag = 1;
			break;
		case 'p':
			pflag = 1;
			parseposition(optarg);
			break;
		case 'r':
			rflag = 1;
			break;
		case 'w':
			wflag = 1;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 1)
		usage();

	/* open connection to server and set X variables */
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "could not open display");
	screen = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, screen);
	rootwin = RootWindow(dpy, screen);
	colormap = DefaultColormap(dpy, screen);

	/* imlib2 stuff */
	if (!iflag) {
		imlib_set_cache_size(2048 * 1024);
		imlib_context_set_dither(1);
		imlib_context_set_display(dpy);
		imlib_context_set_visual(visual);
		imlib_context_set_colormap(colormap);
	}

	/* initializers */
	initmonitor();
	initresources();
	initdc();
	initiconsize();
	initatoms();

	/* set window class */
	classh.res_class = PROGNAME;
	if (argc == 1)
		classh.res_name = *argv;
	else
		classh.res_name = PROGNAME;

	/* generate menus and set them up */
	rootmenu = parsestdin();
	if (rootmenu == NULL)
		errx(1, "no menu generated");
	setupmenu(rootmenu, &classh);

	/* grab mouse and keyboard */
	if (!wflag) {
		grabpointer();
		grabkeyboard();
	}

	/* run event loop */
	run(rootmenu);

	/* freeing stuff */
	cleanmenu(rootmenu);
	cleanup();

	return 0;
}
