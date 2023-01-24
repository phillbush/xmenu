#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <locale.h>
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

/* macros */
#define MAXPATHS 128            /* maximal number of paths to look for icons */
#define ICONPATH "ICONPATH"     /* environment variable name */
#define CLASS               "XMenu"
#define LEN(x)              (sizeof (x) / sizeof (x[0]))
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))
#define GETNUM(n, s) { \
	unsigned long __TMP__; \
	if ((__TMP__ = strtoul((s), NULL, 10)) < INT_MAX) \
		(n) = __TMP__; \
	}

/* Actions for the main loop */
enum {
	ACTION_NOP    = 0,
	ACTION_CLEAR  = 1<<0,     /* clear text */
	ACTION_SELECT = 1<<1,     /* select item */
	ACTION_MAP    = 1<<2,     /* remap menu windows */
	ACTION_DRAW   = 1<<3,     /* redraw menu windows */
	ACTION_WARP   = 1<<4,     /* warp the pointer */
};

/* enum for keyboard menu navigation */
enum { ITEMPREV, ITEMNEXT, ITEMFIRST, ITEMLAST };

/* enum for text alignment */
enum {LeftAlignment, CenterAlignment, RightAlignment};

/* color enum */
enum {ColorFG, ColorBG, ColorLast};

/* EWMH atoms */
enum {NetWMName, NetWMWindowType, NetWMWindowTypePopupMenu, NetLast};

/* configuration structure */
struct Config {
	/* the values below are set by config.h */
	const char *font;
	const char *background_color;
	const char *foreground_color;
	const char *selbackground_color;
	const char *selforeground_color;
	const char *separator_color;
	const char *border_color;
	int width_pixels;
	int height_pixels;
	int border_pixels;
	int max_items;
	int separator_pixels;
	int gap_pixels;
	int triangle_width;
	int triangle_height;
	int iconpadding;
	int horzpadding;
	int alignment;

	/* the values below are set by options */
	int monitor;
	int posx, posy;         /* rootmenu position */

	/* the value below is computed by xmenu */
	int iconsize;
};

/* draw context structure */
struct DC {
	XftColor normal[ColorLast];
	XftColor selected[ColorLast];
	XftColor border;
	XftColor separator;

	GC gc;

	FcPattern *pattern;
	XftFont **fonts;
	size_t nfonts;
};

/* menu item structure */
struct Item {
	char *label;            /* string to be drawed on menu */
	char *output;           /* string to be outputed when item is clicked */
	char *file;             /* filename of the icon */
	int y;                  /* item y position relative to menu */
	int h;                  /* item height */
	int textw;              /* text width */
	struct Item *prev;      /* previous item */
	struct Item *next;      /* next item */
	struct Menu *submenu;   /* submenu spawned by clicking on item */
	Drawable sel, unsel;    /* pixmap for selected and unselected item */
	Imlib_Image icon;
};

/* monitor geometry structure */
struct Monitor {
	int x, y, w, h;         /* monitor geometry */
};

/* menu structure */
struct Menu {
	struct Menu *parent;    /* parent menu */
	struct Item *caller;    /* item that spawned the menu */
	struct Item *list;      /* list of items contained by the menu */
	struct Item *first;     /* first item displayed on the menu */
	struct Item *selected;  /* item currently selected in the menu */
	int x, y, w, h;         /* menu geometry */
	int hasicon;            /* whether the menu has item with icons */
	int drawn;              /* whether the menu was already drawn */
	int maxtextw;           /* maximum text width */
	int level;              /* menu level relative to root */
	int overflow;           /* whether the menu is higher than the monitor */
	Window win;             /* menu window to map on the screen */
	XIC xic;                /* input context */
};

/* X stuff */
static Display *dpy;
static Visual *visual;
static Window rootwin;
static Colormap colormap;
static XrmDatabase xdb;
static XClassHint classh;
static char *xrm;
static struct DC dc;
static Atom utf8string;
static Atom wmdelete;
static Atom netatom[NetLast];
static int screen;
static int depth;
static XIM xim;

/* flags */
static int iflag = 0;                   /* whether to disable icons */
static int rflag = 0;                   /* whether to disable right-click */
static int mflag = 0;                   /* whether the user specified a monitor with -p */
static int pflag = 0;                   /* whether the user specified a position with -p */
static int wflag = 0;                   /* whether to let the window manager control XMenu */
static int rootmodeflag = 0;            /* wheter to run in root mode */
static int passclickflag = 0;           /* whether to pass click to root window */
static int firsttime = 1;               /* set to 0 after first run */

/* arguments */
static unsigned int button = 0;         /* button to trigger pmenu in root mode */
static unsigned int modifier = 0;       /* modifier to trigger pmenu */

/* icons paths */
static char *iconstring = NULL;         /* string read from getenv */
static char *iconpaths[MAXPATHS];       /* paths to icon directories */
static int niconpaths = 0;              /* number of paths to icon directories */

/* include config variable */
#include "config.h"

/* show usage */
static void
usage(void)
{

	(void)fprintf(stderr, "usage: xmenu [-irw] [-p position] [(-x|-X) [modifier-]button] [title]\n");
	exit(1);
}

/* maximum int */
static int
max(int x, int y)
{
	return x > y ? x : y;
}

/* minimum int */
static int
min(int x, int y)
{
	return x < y ? x : y;
}

/* call malloc checking for error */
static void *
emalloc(size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		err(1, "malloc");
	return p;
}

/* call strdup checking for error */
static char *
estrdup(const char *s)
{
	char *t;

	if ((t = strdup(s)) == NULL)
		err(1, "strdup");
	return t;
}

/* parse position string from -p, put results on config.posx, config.posy, and config.monitor */
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

/* get configuration from X resources */
static void
getresources(void)
{
	char *type;
	XrmValue xval;

	if (xrm == NULL || xdb == NULL)
		return;

	if (XrmGetResource(xdb, "xmenu.borderWidth", "*", &type, &xval) == True)
		GETNUM(config.border_pixels, xval.addr)
	if (XrmGetResource(xdb, "xmenu.separatorWidth", "*", &type, &xval) == True)
		GETNUM(config.separator_pixels, xval.addr)
	if (XrmGetResource(xdb, "xmenu.height", "*", &type, &xval) == True)
		GETNUM(config.height_pixels, xval.addr)
	if (XrmGetResource(xdb, "xmenu.width", "*", &type, &xval) == True)
		GETNUM(config.width_pixels, xval.addr)
	if (XrmGetResource(xdb, "xmenu.gap", "*", &type, &xval) == True)
		GETNUM(config.gap_pixels, xval.addr)
	if (XrmGetResource(xdb, "xmenu.maxItems", "*", &type, &xval) == True)
		GETNUM(config.max_items, xval.addr)
	if (XrmGetResource(xdb, "xmenu.background", "*", &type, &xval) == True)
		config.background_color = xval.addr;
	if (XrmGetResource(xdb, "xmenu.foreground", "*", &type, &xval) == True)
		config.foreground_color = xval.addr;
	if (XrmGetResource(xdb, "xmenu.selbackground", "*", &type, &xval) == True)
		config.selbackground_color = xval.addr;
	if (XrmGetResource(xdb, "xmenu.selforeground", "*", &type, &xval) == True)
		config.selforeground_color = xval.addr;
	if (XrmGetResource(xdb, "xmenu.separator", "*", &type, &xval) == True)
		config.separator_color = xval.addr;
	if (XrmGetResource(xdb, "xmenu.border", "*", &type, &xval) == True)
		config.border_color = xval.addr;
	if (XrmGetResource(xdb, "xmenu.font", "*", &type, &xval) == True)
		config.font = xval.addr;
	if (XrmGetResource(xdb, "xmenu.alignment", "*", &type, &xval) == True) {
		if (strcasecmp(xval.addr, "center") == 0)
			config.alignment = CenterAlignment;
		else if (strcasecmp(xval.addr, "left") == 0)
			config.alignment = LeftAlignment;
		else if (strcasecmp(xval.addr, "right") == 0)
			config.alignment = RightAlignment;
	}
}

/* set button global variable */
static void
setbutton(char *s)
{
	size_t len;

	if ((len = strlen(s)) < 1)
		return;
	switch (s[len-1]) {
	case '1': button = Button1; break;
	case '2': button = Button2; break;
	case '3': button = Button3; break;
	default:  button = atoi(&s[len-1]); break;
	}
}

/* set modifier global variable */
static void
setmodifier(char *s)
{
	size_t len;

	if ((len = strlen(s)) < 1)
		return;
	switch (s[len-1]) {
	case '1': modifier = Mod1Mask; break;
	case '2': modifier = Mod2Mask; break;
	case '3': modifier = Mod3Mask; break;
	case '4': modifier = Mod4Mask; break;
	case '5': modifier = Mod5Mask; break;
	default:
		if (strcasecmp(s, "Alt") == 0) {
			modifier = Mod1Mask;
		} else if (strcasecmp(s, "Super") == 0) {
			modifier = Mod4Mask;
		}
		break;
	}
}

/* parse icon path string */
static void
parseiconpaths(char *s)
{
	if (s == NULL)
		return;
	free(iconstring);
	iconstring = estrdup(s);
	niconpaths = 0;
	for (s = strtok(iconstring, ":"); s != NULL; s = strtok(NULL, ":")) {
		if (niconpaths < MAXPATHS) {
			iconpaths[niconpaths++] = s;
		}
	}
}

/* get configuration from command-line options */
static void
getoptions(int argc, char *argv[])
{
	int ch;
	char *s, *t;

	classh.res_class = CLASS;
	classh.res_name = argv[0];
	if ((s = strrchr(argv[0], '/')) != NULL)
		classh.res_name = s + 1;
	parseiconpaths(getenv(ICONPATH));
	while ((ch = getopt(argc, argv, "ip:rwx:X:")) != -1) {
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
		case 'X':
			passclickflag = 1;
			/* PASSTHROUGH */
		case 'x':
			rootmodeflag = 1;
			s = optarg;
			setbutton(s);
			if ((t = strchr(s, '-')) == NULL)
				return;
			*t = '\0';
			setmodifier(s);
			break;
		default:
			usage();
			break;
		}
	}
	if (rootmodeflag)
		wflag = 0;
	argc -= optind;
	argv += optind;
	if (argc != 0)
		usage();
	return;
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
getmonitor(struct Monitor *mon)
{
	XineramaScreenInfo *info = NULL;
	Window dw;          /* dummy variable */
	int di;             /* dummy variable */
	unsigned du;        /* dummy variable */
	int cursx, cursy;   /* cursor position */
	int nmons;
	int i;

	XQueryPointer(dpy, rootwin, &dw, &dw, &cursx, &cursy, &di, &di, &du);

	mon->x = mon->y = 0;
	mon->w = DisplayWidth(dpy, screen);
	mon->h = DisplayHeight(dpy, screen);

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

		mon->x = info[selmon].x_org;
		mon->y = info[selmon].y_org;
		mon->w = info[selmon].width;
		mon->h = info[selmon].height;

		XFree(info);
	}

	if (!pflag) {
		config.posx = cursx;
		config.posy = cursy;
	} else if (mflag) {
		config.posx += mon->x;
		config.posy += mon->y;
	}
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

	item = emalloc(sizeof *item);
	if (label == NULL) {
		item->label = NULL;
		item->output = NULL;
	} else {
		item->label = estrdup(label);
		if (label == output) {
			item->output = item->label;
		} else {
			item->output = estrdup(output);
		}
	}
	if (file == NULL) {
		item->file = NULL;
	} else {
		item->file = estrdup(file);
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
allocmenu(struct Menu *parent, struct Item *list, int level)
{
	XSetWindowAttributes swa;
	struct Menu *menu;

	menu = emalloc(sizeof *menu);
	*menu = (struct Menu){
		.parent = parent,
		.list = list,
		.level = level,
		.first = NULL,
	};

	swa.override_redirect = (wflag) ? False : True;
	swa.background_pixel = dc.normal[ColorBG].pixel;
	swa.border_pixel = dc.border.pixel;
	swa.save_under = True;  /* pop-up windows should save_under*/
	swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask
	               | PointerMotionMask | LeaveWindowMask;
	if (wflag)
		swa.event_mask |= StructureNotifyMask;
	menu->win = XCreateWindow(dpy, rootwin, 0, 0, 1, 1, config.border_pixels,
	                          CopyFromParent, CopyFromParent, CopyFromParent,
	                          CWOverrideRedirect | CWBackPixel |
	                          CWBorderPixel | CWEventMask | CWSaveUnder,
	                          &swa);

	menu->xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                      XNClientWindow, menu->win, XNFocusWindow, menu->win, NULL);
	if (menu->xic == NULL)
		errx(1, "XCreateIC: could not obtain input method");

	return menu;
}

/* build the menu tree */
static struct Menu *
buildmenutree(int level, const char *label, const char *output, char *file)
{
	static struct Menu *prevmenu = NULL;    /* menu the previous item was added to */
	static struct Menu *rootmenu = NULL;    /* menu to be returned */
	struct Item *curritem = NULL;           /* item currently being read */
	struct Item *item;                      /* dummy item for loops */
	struct Menu *menu;                      /* dummy menu for loops */
	int i;

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

		/* a separator is no valid root for a submenu */
		if (!item->label)
			errx(1, "a separator is no valid root for a submenu");

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
	int level = 0;

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
setupitems(struct Menu *menu, struct Monitor *mon)
{
	Pixmap pix;
	struct Item *item;
	int itemwidth;
	int menuh;
	int maxh;

	menu->first = menu->list;
	menu->w = config.width_pixels;
	menu->maxtextw = 0;
	menuh = 0;
	maxh = config.max_items > 3 ? (2 + config.max_items) * config.height_pixels : mon->h;
	for (item = menu->list; item != NULL; item = item->next) {
		item->y = menuh;
		if (item->label == NULL)   /* height for separator item */
			item->h = config.separator_pixels;
		else
			item->h = config.height_pixels;
		menuh += item->h;
		if (!menu->overflow) {
			if (menu->h + config.height_pixels * 2 < maxh - config.border_pixels * 2) {
				menu->h = menuh;
			} else {
				menu->overflow = 1;
				menu->h += config.height_pixels * 2;
			}
		}
		if (item->label)
			item->textw = drawtext(NULL, NULL, 0, 0, 0, item->label);
		else
			item->textw = 0;

		/*
		 * set menu width
		 *
		 * the item width depends on the size of its label (item->textw),
		 * and it is only used to calculate the width of the menu (which
		 * is equal to the width of the largest item).
		 *
		 * the horizontal padding appears 4 times through the width of a
		 * item: before and after its icon, and before and after its triangle.
		 * if the iflag is set (icons are disabled) then the horizontal
		 * padding appears 3 times: before the label and around the triangle.
		 */
		itemwidth = item->textw + config.triangle_width + config.horzpadding * 3;
		itemwidth += (iflag || !menu->hasicon) ? 0 : config.iconsize + config.horzpadding;
		menu->w = max(menu->w, itemwidth);
		menu->maxtextw = max(menu->maxtextw, item->textw);
	}
	if (!menu->overflow) {
		XSetWindowBackground(dpy, menu->win, dc.normal[ColorBG].pixel);
	} else {
		pix = XCreatePixmap(dpy, menu->win, menu->w, menu->h, depth);
		XSetForeground(dpy, dc.gc, dc.normal[ColorBG].pixel);
		XFillRectangle(dpy, pix, dc.gc, 0, 0, menu->w, menu->h);
		XSetForeground(dpy, dc.gc, dc.normal[ColorFG].pixel);
		XFillPolygon(
			dpy, pix, dc.gc,
			(XPoint []){
				{menu->w / 2 - 3, config.height_pixels / 2 + 2},
				{3, -4},
				{3, 4},
				{-6, 0},
			},
			4, Convex, CoordModePrevious
		);
		XFillPolygon(
			dpy, pix, dc.gc,
			(XPoint []){
				{menu->w / 2 - 3, menu->h - config.height_pixels / 2 - 2},
				{3, 4},
				{3, -4},
				{-6, 0},
			},
			4, Convex, CoordModePrevious
		);
		XSetWindowBackgroundPixmap(dpy, menu->win, pix);
		XFreePixmap(dpy, pix);
	}
}

/* setup the position of a menu */
static void
setupmenupos(struct Menu *menu, struct Monitor *mon)
{
	int width, height;

	width = menu->w + config.border_pixels * 2;
	height = menu->h + config.border_pixels * 2;
	if (menu->parent == NULL) { /* if root menu, calculate in respect to cursor */
		if (pflag || (config.posx >= mon->x && mon->x + mon->w - config.posx >= width))
			menu->x = config.posx;
		else if (config.posx > width)
			menu->x = config.posx - width;

		if (pflag || (config.posy >= mon->y && mon->y + mon->h - config.posy >= height))
			menu->y = config.posy;
		else if (mon->y + mon->h > height)
			menu->y = mon->y + mon->h - height;
	} else {                    /* else, calculate in respect to parent menu */
		int parentwidth;

		parentwidth = menu->parent->x + menu->parent->w + config.border_pixels + config.gap_pixels;

		if (mon->x + mon->w - parentwidth >= width)
			menu->x = parentwidth;
		else if (menu->parent->x > menu->w + config.border_pixels + config.gap_pixels)
			menu->x = menu->parent->x - menu->w - config.border_pixels - config.gap_pixels;

		if (mon->y + mon->h - (menu->caller->y + menu->parent->y) >= height)
			menu->y = menu->caller->y + menu->parent->y;
		else if (mon->y + mon->h > height)
			menu->y = mon->y + mon->h - height;
	}
}

/* recursivelly setup menu configuration and its pixmap */
static void
setupmenu(struct Menu *menu, struct Monitor *mon)
{
	char *title;
	struct Item *item;
	XWindowChanges changes;
	XSizeHints sizeh;
	XTextProperty wintitle;

	/* setup size and position of menus */
	setupitems(menu, mon);
	setupmenupos(menu, mon);

	/* update menu geometry */
	changes.height = menu->h;
	changes.width = menu->w;
	changes.x = menu->x;
	changes.y = menu->y;
	XConfigureWindow(dpy, menu->win, CWWidth | CWHeight | CWX | CWY, &changes);

	if (firsttime) {
		/* set window title (used if wflag is on) */
		if (menu->parent == NULL) {
			title = classh.res_name;
		} else if (menu->caller->output) {
			title = menu->caller->output;
		} else {
			title = "\0";
		}
		XStringListToTextProperty(&title, 1, &wintitle);

		/* set window manager hints */
		sizeh.flags = USPosition | PMaxSize | PMinSize;
		sizeh.min_width = sizeh.max_width = menu->w;
		sizeh.min_height = sizeh.max_height = menu->h;
		XSetWMProperties(dpy, menu->win, &wintitle, NULL, NULL, 0, &sizeh, NULL, &classh);

		/* set WM protocols and ewmh window properties */
		XSetWMProtocols(dpy, menu->win, &wmdelete, 1);
		XChangeProperty(dpy, menu->win, netatom[NetWMName], utf8string, 8,
	                	PropModeReplace, (unsigned char *)title, strlen(title));
		XChangeProperty(dpy, menu->win, netatom[NetWMWindowType], XA_ATOM, 32,
	                	PropModeReplace,
	                	(unsigned char *)&netatom[NetWMWindowTypePopupMenu], 1);
	}

	/* calculate positions of submenus */
	for (item = menu->list; item != NULL; item = item->next) {
		if (item->submenu != NULL)
			setupmenu(item->submenu, mon);
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

/* try to grab focus, we may have to wait for another process to ungrab */
static void
grabfocus(Window win)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000  };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(dpy, &focuswin, &revertwin);
		if (focuswin == win)
			return;
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	errx(1, "cannot grab focus");
}

/* ungrab pointer and keyboard */
static void
ungrab(void)
{
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
}

/* check if path is absolute or relative to current directory */
static int
isabsolute(const char *s)
{
	return s[0] == '/' || (s[0] == '.' && (s[1] == '/' || (s[1] == '.' && s[2] == '/')));
}

/* load and scale icon */
static Imlib_Image
loadicon(const char *file)
{
	Imlib_Image icon;
	Imlib_Load_Error errcode;
	char path[PATH_MAX];
	const char *errstr;
	int width;
	int height;
	int imgsize;
	int i;

	if (*file == '\0') {
		warnx("could not load icon (file name is blank)");
		return NULL;
	}
	if (isabsolute(file))
		icon = imlib_load_image_with_error_return(file, &errcode);
	else {
		for (i = 0; i < niconpaths; i++) {
			snprintf(path, sizeof(path), "%s/%s", iconpaths[i], file);
			icon = imlib_load_image_with_error_return(path, &errcode);
			if (icon != NULL || errcode != IMLIB_LOAD_ERROR_FILE_DOES_NOT_EXIST) {
				break;
			}
		}
	}
	if (icon == NULL) {
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
	imgsize = min(width, height);

	icon = imlib_create_cropped_scaled_image(0, 0, imgsize, imgsize,
	                                         config.iconsize,
	                                         config.iconsize);

	return icon;
}

/* draw pixmap for the selected and unselected version of each item on menu */
static void
drawitems(struct Menu *menu)
{
	XftDraw *dsel, *dunsel;
	struct Item *item;
	int textx;
	int x, y;

	for (item = menu->list; item != NULL; item = item->next) {
		item->unsel = XCreatePixmap(dpy, menu->win, menu->w, item->h, depth);

		XSetForeground(dpy, dc.gc, dc.normal[ColorBG].pixel);
		XFillRectangle(dpy, item->unsel, dc.gc, 0, 0, menu->w, item->h);

		if (item->label == NULL) { /* item is separator */
			y = item->h / 2;
			XSetForeground(dpy, dc.gc, dc.separator.pixel);
			XDrawLine(dpy, item->unsel, dc.gc, config.horzpadding, y,
			          menu->w - config.horzpadding, y);

			item->sel = item->unsel;
		} else {
			item->sel = XCreatePixmap(dpy, menu->win, menu->w, item->h, depth);
			XSetForeground(dpy, dc.gc, dc.selected[ColorBG].pixel);
			XFillRectangle(dpy, item->sel, dc.gc, 0, 0, menu->w, item->h);

			/* draw text */
			textx = config.horzpadding;
			textx += (iflag || !menu->hasicon) ? 0 : config.horzpadding + config.iconsize;
			switch (config.alignment) {
			case CenterAlignment:
				textx += (menu->maxtextw - item->textw) / 2;
				break;
			case RightAlignment:
				textx += menu->maxtextw - item->textw;
				break;
			default:
				break;
			}
			dsel = XftDrawCreate(dpy, item->sel, visual, colormap);
			dunsel = XftDrawCreate(dpy, item->unsel, visual, colormap);
			XSetForeground(dpy, dc.gc, dc.selected[ColorFG].pixel);
			drawtext(dsel, &dc.selected[ColorFG], textx, 0, item->h, item->label);
			XSetForeground(dpy, dc.gc, dc.normal[ColorFG].pixel);
			drawtext(dunsel, &dc.normal[ColorFG], textx, 0, item->h, item->label);
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
	int y0, y;
	int maxh;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		if (!menu->drawn) {
			drawitems(menu);
			menu->drawn = 1;
		}
		if (menu->overflow && menu->selected != NULL) {
			maxh = menu->h - config.height_pixels * 2;
			while (menu->first->next != NULL &&
			       menu->selected->y >= menu->first->y + maxh) {
				menu->first = menu->first->next;
			}
			while (menu->first->prev != NULL &&
			       menu->selected->y < menu->first->y) {
				menu->first = menu->first->prev;
			}
		}
		y = menu->first->y;
		y0 = menu->overflow ? config.height_pixels : 0;
		for (item = menu->first; item != NULL; item = item->next) {
			if (menu->overflow && item->y - y + item->h > menu->h - config.height_pixels * 2)
				break;
			if (item == menu->selected) {
				XCopyArea(dpy, item->sel, menu->win, dc.gc, 0, 0, menu->w, item->h, 0, y0 + item->y - y);
			} else {
				XCopyArea(dpy, item->unsel, menu->win, dc.gc, 0, 0, menu->w, item->h, 0, y0 + item->y - y);
			}
		}
	}
}

/* unmap current menu and its parents */
static void
unmapmenu(struct Menu *currmenu)
{
	struct Menu *menu;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		menu->selected = NULL;
		XUnmapWindow(dpy, menu->win);
	}
}

/* umap previous menus and map current menu and its parents */
static struct Menu *
mapmenu(struct Menu *currmenu, struct Menu *prevmenu, struct Monitor *mon)
{
	struct Menu *menu, *menu_;
	struct Menu *lcamenu;   /* lowest common ancestor menu */
	int minlevel;           /* level of the closest to root menu */
	int maxlevel;           /* level of the closest to root menu */

	/* do not remap current menu if it wasn't updated*/
	if (prevmenu == currmenu)
		goto done;

	/* if this is the first time mapping, skip calculations */
	if (prevmenu == NULL) {
		XMapWindow(dpy, currmenu->win);
		goto done;
	}

	/* find lowest common ancestor menu */
	minlevel = min(currmenu->level, prevmenu->level);
	maxlevel = max(currmenu->level, prevmenu->level);
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
			setupmenupos(menu, mon);
			XMoveWindow(dpy, menu->win, menu->x, menu->y);
		}
		XMapWindow(dpy, menu->win);
	}

	grabfocus(currmenu->win);
done:
	return currmenu;
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

/* get in *ret the item in given menu and position; return 1 if position is on a scroll triangle */
static int
getitem(struct Menu *menu, struct Item **ret, int y)
{
	struct Item *item;
	int y0;

	*ret = NULL;
	if (menu == NULL)
		return 0;
	if (menu->overflow) {
		if (y < config.height_pixels) {
			*ret = menu->first->prev;
			return 1;
		} else if (y > menu->h - config.height_pixels) {
			y0 = menu->overflow ? config.height_pixels : 0;
			y = menu->h - y0 + menu->first->y;
			for (item = menu->first; item != NULL; item = item->next)
				if (y >= item->y && y <= item->y + item->h)
					break;
			if (item != NULL)
				*ret = menu->first->next;
			return 1;
		}
	}
	y0 = menu->overflow ? config.height_pixels : 0;
	y -= y0 - menu->first->y;
	for (item = menu->first; item != NULL; item = item->next) {
		if (y >= item->y && y <= item->y + item->h) {
			*ret = item;
			break;
		}
	}
	return 0;
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

/* check if button is used to scroll */
static int
isscrollbutton(unsigned int button)
{
	if (button == Button4 || button == Button5)
		return 1;
	return 0;
}

/* check if button is used to open a item on click */
static int
isclickbutton(unsigned int button)
{
	if (button == Button1 || button == Button2)
		return 1;
	if (!rflag && button == Button3)
		return 1;
	return 0;
}

/* warp pointer to center of selected item */
static void
warppointer(struct Menu *menu, struct Item *item)
{
	if (menu == NULL || item == NULL)
		return;
	if (menu->selected) {
		XWarpPointer(dpy, None, menu->win, 0, 0, 0, 0, menu->w / 2, item->y + item->h / 2);
	}
}

/* append buf into text */
static int
append(char *text, char *buf, size_t textsize, size_t buflen)
{
	size_t textlen;

	textlen = strlen(text);
	if (iscntrl(*buf))
		return 0;
	if (textlen + buflen > textsize - 1)
		return 0;
	if (buflen < 1)
		return 0;
	memcpy(text + textlen, buf, buflen);
	text[textlen + buflen] = '\0';
	return 1;
}

/* get item in menu matching text from given direction (or from beginning, if dir = 0) */
static struct Item *
matchitem(struct Menu *menu, char *text, int dir)
{
	struct Item *item, *lastitem;
	char *s;
	size_t textlen;

	for (lastitem = menu->list; lastitem && lastitem->next; lastitem = lastitem->next)
		;
	textlen = strlen(text);
	if (dir < 0) {
		if (menu->selected && menu->selected->prev)
			item = menu->selected->prev;
		else
			item = lastitem;
	} else if (dir > 0) {
		if (menu->selected && menu->selected->next)
			item = menu->selected->next;
		else
			item = menu->list;
	} else {
		item = menu->list;
	}
	/* find next item from selected item */
	for ( ; item; item = (dir < 0) ? item->prev : item->next)
		for (s = item->label; s && *s; s++)
			if (strncasecmp(s, text, textlen) == 0)
				return item;
	/* if not found, try to find from the beginning/end of list */
	if (dir > 0) {
		for (item = menu->list ; item; item = item->next) {
			for (s = item->label; s && *s; s++) {
				if (strncasecmp(s, text, textlen) == 0) {
					return item;
				}
			}
		}
	} else {
		for (item = lastitem ; item; item = item->prev) {
			for (s = item->label; s && *s; s++) {
				if (strncasecmp(s, text, textlen) == 0) {
					return item;
				}
			}
		}
	}
	return NULL;
}

/* check keysyms defined on config.h */
static KeySym
normalizeksym(KeySym ksym)
{
	if (ksym == KSYMFIRST)
		return XK_Home;
	if (ksym == KSYMLAST)
		return XK_End;
	if (ksym == KSYMUP)
		return XK_Up;
	if (ksym == KSYMDOWN)
		return XK_Down;
	if (ksym == KSYMLEFT)
		return XK_Left;
	if (ksym == KSYMRIGHT)
		return XK_Right;
	return ksym;
}

/* run event loop */
static void
run(struct Menu *currmenu, struct Monitor *mon)
{
	char text[BUFSIZ];
	char buf[32];
	struct Menu *menu, *prevmenu;
	struct Item *item;
	struct Item *previtem = NULL;
	struct Item *lastitem, *select;
	KeySym ksym;
	Status status;
	XEvent ev;
	int warped = 0;
	int action;
	int len;
	int i;

	text[0] = '\0';
	prevmenu = currmenu;
	while (!XNextEvent(dpy, &ev)) {
		if (XFilterEvent(&ev, None))
			continue;
		action = ACTION_NOP;
		switch(ev.type) {
		case Expose:
			if (ev.xexpose.count == 0)
				action = ACTION_DRAW;
			break;
		case MotionNotify:
			if (!warped) {
				menu = getmenu(currmenu, ev.xbutton.window);
				if (getitem(menu, &item, ev.xbutton.y))
					break;
				if (menu == NULL || item == NULL || previtem == item)
					break;
				previtem = item;
				select = menu->selected = item;
				if (item->submenu != NULL) {
					currmenu = item->submenu;
					select = NULL;
				} else {
					currmenu = menu;
				}
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_MAP | ACTION_DRAW;
			}
			warped = 0;
			break;
		case ButtonRelease:
			if (isscrollbutton(ev.xbutton.button)) {
				if (ev.xbutton.button == Button4)
					select = itemcycle(currmenu, ITEMPREV);
				else
					select = itemcycle(currmenu, ITEMNEXT);
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW | ACTION_WARP;
			} else if (isclickbutton(ev.xbutton.button)) {
				menu = getmenu(currmenu, ev.xbutton.window);
				if (getitem(menu, &item, ev.xbutton.y) && item != NULL) {
					select = NULL;
					menu->first = item;
					action = ACTION_CLEAR | ACTION_SELECT | ACTION_MAP | ACTION_DRAW;
					break;
				}
				if (menu == NULL || item == NULL)
					break;
enteritem:
				if (item->label == NULL)
					break;  /* ignore separators */
				if (item->submenu != NULL) {
					currmenu = item->submenu;
				} else {
					printf("%s\n", item->output);
					goto done;
				}
				select = currmenu->list;
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_MAP | ACTION_DRAW;
				if (ev.xbutton.button == Button2) {
					action |= ACTION_WARP;
				}
			}
			break;
		case ButtonPress:
			menu = getmenu(currmenu, ev.xbutton.window);
			if (menu == NULL)
				goto done;
			break;
		case KeyPress:
			len = XmbLookupString(currmenu->xic, &ev.xkey, buf, sizeof buf, &ksym, &status);
			switch(status) {
			default:                /* XLookupNone, XBufferOverflow */
				continue;
			case XLookupChars:
				goto append;
			case XLookupKeySym:     /* FALLTHROUGH */
			case XLookupBoth:
				break;
			}

			/* esc closes xmenu when current menu is the root menu */
			if (ksym == XK_Escape && currmenu->parent == NULL)
				goto done;

			/* Shift-Tab = ISO_Left_Tab */
			if (ksym == XK_Tab && (ev.xkey.state & ShiftMask))
				ksym = XK_ISO_Left_Tab;

			/* cycle through menu */
			select = NULL;
			ksym = normalizeksym(ksym);
			switch (ksym) {
			case XK_Home:
				select = itemcycle(currmenu, ITEMFIRST);
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW;
				break;
			case XK_End:
				select = itemcycle(currmenu, ITEMLAST);
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW;
				break;
			case XK_ISO_Left_Tab:
				if (*text) {
					select = matchitem(currmenu, text, -1);
					action = ACTION_SELECT | ACTION_DRAW;
					break;
				}
				/* FALLTHROUGH */
			case XK_Up:
				select = itemcycle(currmenu, ITEMPREV);
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW;
				break;
			case XK_Tab:
				if (*text) {
					select = matchitem(currmenu, text, 1);
					action = ACTION_SELECT | ACTION_DRAW;
					break;
				}
				/* FALLTHROUGH */
			case XK_Down:
				select = itemcycle(currmenu, ITEMNEXT);
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW;
				break;
			case XK_1: case XK_2: case XK_3: case XK_4: case XK_5: case XK_6: case XK_7: case XK_8: case XK_9:
				item = itemcycle(currmenu, ITEMFIRST);
				lastitem = itemcycle(currmenu, ITEMLAST);
				for (int i = ksym - XK_1; i > 0 && item != lastitem; i--) {
					currmenu->selected = item;
					item = itemcycle(currmenu, ITEMNEXT);
				}
				select = item;
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW;
				break;
			case XK_Return: case XK_Right:
				if (currmenu->selected) {
					item = currmenu->selected;
					goto enteritem;
				}
				break;
			case XK_Escape: case XK_Left:
				if (currmenu->parent) {
					select = currmenu->parent->selected;
					currmenu = currmenu->parent;
					action = ACTION_CLEAR | ACTION_MAP | ACTION_SELECT | ACTION_DRAW;
				}
				break;
			case XK_BackSpace: case XK_Clear: case XK_Delete:
				action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW;
				break;
			default:
append:
				if (*buf == '\0' || iscntrl(*buf))
					break;
				for (i = 0; i < 2; i++) {
					append(text, buf, sizeof text, len);
					if ((select = matchitem(currmenu, text, 0)))
						break;
					text[0] = '\0';
				}
				action = ACTION_SELECT | ACTION_DRAW;
				break;
			}
			break;
		case LeaveNotify:
			previtem = NULL;
			select = NULL;
			action = ACTION_CLEAR | ACTION_SELECT | ACTION_DRAW;
			break;
		case ConfigureNotify:
			menu = getmenu(currmenu, ev.xconfigure.window);
			if (menu == NULL)
				break;
			menu->x = ev.xconfigure.x;
			menu->y = ev.xconfigure.y;
			break;
		case ClientMessage:
			if ((unsigned long)ev.xclient.data.l[0] != wmdelete)
				break;
			/* user closed window */
			menu = getmenu(currmenu, ev.xclient.window);
			if (menu->parent == NULL)
				goto done;  /* closing the root menu closes the program */
			currmenu = menu->parent;
			action = ACTION_MAP;
			break;
		}
		if (action & ACTION_CLEAR)
			text[0] = '\0';
		if (action & ACTION_SELECT)
			currmenu->selected = select;
		if (action & ACTION_MAP)
			prevmenu = mapmenu(currmenu, prevmenu, mon);
		if (action & ACTION_DRAW)
			drawmenus(currmenu);
		if (action & ACTION_WARP) {
			warppointer(currmenu, select);
			warped = 1;
		}
	}
done:
	unmapmenu(currmenu);
	ungrab();
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

/* cleanup draw context */
static void
cleandc(void)
{
	size_t i;

	XftColorFree(dpy, visual, colormap, &dc.normal[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.normal[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.separator);
	XftColorFree(dpy, visual, colormap, &dc.border);
	for (i = 0; i < dc.nfonts; i++)
		XftFontClose(dpy, dc.fonts[i]);
	XFreeGC(dpy, dc.gc);
}

/* xmenu: generate menu from stdin and print selected entry to stdout */
int
main(int argc, char *argv[])
{
	struct Monitor mon;
	struct Menu *rootmenu;
	XEvent ev;

	/* open connection to server and set X variables */
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support");
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "could not open display");
	screen = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, screen);
	rootwin = RootWindow(dpy, screen);
	colormap = DefaultColormap(dpy, screen);
	depth = DefaultDepth(dpy, screen);
	XrmInitialize();
	if ((xrm = XResourceManagerString(dpy)) != NULL)
		xdb = XrmGetStringDatabase(xrm);
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		errx(1, "XOpenIM: could not open input device");

	/* process configuration and window class */
	getresources();
	getoptions(argc, argv);

	/* imlib2 stuff */
	if (!iflag) {
		imlib_set_cache_size(2048 * 1024);
		imlib_context_set_dither(1);
		imlib_context_set_display(dpy);
		imlib_context_set_visual(visual);
		imlib_context_set_colormap(colormap);
	}

	/* initializers */
	initdc();
	initiconsize();
	initatoms();

	/* if running in root mode, get button presses from root window */
	if (rootmodeflag)
		XGrabButton(dpy, button, AnyModifier, rootwin, False, ButtonPressMask, GrabModeSync, GrabModeSync, None, None);

	/* generate menus */
	rootmenu = parsestdin();
	if (rootmenu == NULL)
		errx(1, "no menu generated");

	/* run event loop */
	do {
		if (rootmodeflag)
			XNextEvent(dpy, &ev);
		if (!rootmodeflag ||
		    (ev.type == ButtonPress &&
		     ((modifier != 0 && (ev.xbutton.state & modifier)) ||
		      (ev.xbutton.subwindow == None)))) {
			if (rootmodeflag && passclickflag) {
				XAllowEvents(dpy, ReplayPointer, CurrentTime);
			}
			getmonitor(&mon);
			if (!wflag) {
				grabpointer();
				grabkeyboard();
			}
			setupmenu(rootmenu, &mon);
			mapmenu(rootmenu, NULL, &mon);
			XFlush(dpy);
			run(rootmenu, &mon);
			firsttime = 0;
		} else {
			XAllowEvents(dpy, ReplayPointer, CurrentTime);
		}
	} while (rootmodeflag);

	/* clean stuff */
	cleanmenu(rootmenu);
	cleandc();
	XrmDestroyDatabase(xdb);
	XCloseDisplay(dpy);

	return 0;
}
