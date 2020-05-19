#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>

#define PROGNAME "xmenu"
#define ITEMPREV 0
#define ITEMNEXT 1

/* macros */
#define LEN(x) (sizeof (x) / sizeof (x[0]))
#define MAX(x,y) ((x)>(y)?(x):(y))
#define MIN(x,y) ((x)<(y)?(x):(y))

/* color enum */
enum {ColorFG, ColorBG, ColorLast};

/* draw context structure */
struct DC {
	XftColor normal[ColorLast];
	XftColor selected[ColorLast];
	XftColor decoration[ColorLast];

	Drawable d;
	GC gc;
	XftFont *font;
};

/* menu geometry structure */
struct Geometry {
	int itemb;      /* item border */
	int itemw;      /* item width */
	int itemh;      /* item height */
	int border;     /* window border width */
	int separator;  /* menu separator width */
};

/* screen geometry structure */
struct ScreenGeometry {
	int cursx, cursy;       /* cursor position */
	int screenw, screenh;   /* screen width and height */
};

/* menu item structure */
struct Item {
	char *label;            /* string to be drawed on menu */
	char *output;           /* string to be outputed when item is clicked */
	int y;                  /* item y position relative to menu */
	int h;                  /* item height */
	size_t labellen;        /* strlen(label) */
	struct Item *prev;      /* previous item */
	struct Item *next;      /* next item */
	struct Menu *submenu;   /* submenu spawned by clicking on item */
};

/* menu structure */
struct Menu {
	struct Menu *parent;    /* parent menu */
	struct Item *caller;    /* item that spawned the menu */
	struct Item *list;      /* list of items contained by the menu */
	struct Item *selected;  /* item currently selected in the menu */
	int x, y, w, h;         /* menu geometry */
	unsigned level;         /* menu level relative to root */
	Drawable pixmap;        /* pixmap to draw the menu on */
	XftDraw *draw;
	Window win;             /* menu window to map on the screen */
};

/* function declarations */
static void getcolor(const char *s, XftColor *color);
static void getresources(void);
static void setupdc(void);
static void setupgeom(void);
static struct Item *allocitem(const char *label, const char *output);
static struct Menu *allocmenu(struct Menu *parent, struct Item *list, unsigned level);
static void getmenuitem(Window win, int y, struct Menu **menu_ret, struct Item **item_ret);
static void drawmenu(void);
static void calcscreengeom(void);
static void calcmenu(struct Menu *menu);
static void recalcmenu(struct Menu *menu);
static void grabpointer(void);
static void grabkeyboard(void);
static void setcurrmenu(struct Menu *currmenu_new);
static void parsestdin(void);
static void run(void);
static void freewindow(struct Menu *menu);
static void cleanup(void);
static void usage(void);

/* X variables */
static Colormap colormap;
static Display *dpy;
static Visual *visual;
static Window rootwin;
static int screen;
static struct DC dc;
static Atom wmdelete;

/* menu variables */
static struct Menu *rootmenu = NULL;
static struct Menu *currmenu = NULL;
static char **menutitle;
static int menutitlecount;

/* geometry variables */
static struct Geometry geom;
static struct ScreenGeometry screengeom;

/* flag variables */
static Bool override_redirect = True;

#include "config.h"

int
main(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "w")) != -1) {
		switch (ch) {
		case 'w':
			override_redirect = False;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	menutitle = argv;
	menutitlecount = argc;

	/* open connection to server and set X variables */
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "cannot open display");
	screen = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, screen);
	rootwin = RootWindow(dpy, screen);
	colormap = DefaultColormap(dpy, screen);
	wmdelete=XInternAtom(dpy, "WM_DELETE_WINDOW", True);

	/* setup */
	getresources();
	setupdc();
	setupgeom();

	/* generate menus and recalculate them */
	parsestdin();
	if (rootmenu == NULL)
		errx(1, "no menu generated");
	calcscreengeom();
	calcmenu(rootmenu);

	/* grab mouse and keyboard */
	if (override_redirect) {
		grabpointer();
		grabkeyboard();
	}

	/* map root menu */
	currmenu = rootmenu;
	XMapWindow(dpy, rootmenu->win);

	/* run event loop */
	run();

	cleanup();

	return 0;
}

/* read xrdb for configuration options */
static void
getresources(void)
{
	char *xrm;
	long n;

	XrmInitialize();
	if ((xrm = XResourceManagerString(dpy))) {
		char *type;
		XrmDatabase xdb;
		XrmValue xval;

		xdb = XrmGetStringDatabase(xrm);

		if (XrmGetResource(xdb, "xmenu.menuborder", "*", &type, &xval) == True)
			if ((n = strtol(xval.addr, NULL, 10)) > 0)
				menuborder = n;
		if (XrmGetResource(xdb, "xmenu.separatorsize", "*", &type, &xval) == True)
			if ((n = strtol(xval.addr, NULL, 10)) > 0)
				separatorsize = n;
		if (XrmGetResource(xdb, "xmenu.itemborder", "*", &type, &xval) == True)
			if ((n = strtol(xval.addr, NULL, 10)) > 0)
				itemborder = n;
		if (XrmGetResource(xdb, "xmenu.width", "*", &type, &xval) == True)
			if ((n = strtol(xval.addr, NULL, 10)) > 0)
				width = n;
		if (XrmGetResource(xdb, "xmenu.background", "*", &type, &xval) == True)
			background = strdup(xval.addr);
		if (XrmGetResource(xdb, "xmenu.foreground", "*", &type, &xval) == True)
			foreground = strdup(xval.addr);
		if (XrmGetResource(xdb, "xmenu.selbackground", "*", &type, &xval) == True)
			selbackground = strdup(xval.addr);
		if (XrmGetResource(xdb, "xmenu.selforeground", "*", &type, &xval) == True)
			selforeground = strdup(xval.addr);
		if (XrmGetResource(xdb, "xmenu.separator", "*", &type, &xval) == True)
			separator = strdup(xval.addr);
		if (XrmGetResource(xdb, "xmenu.border", "*", &type, &xval) == True)
			border = strdup(xval.addr);
		if (XrmGetResource(xdb, "xmenu.font", "*", &type, &xval) == True)
			font = strdup(xval.addr);

		XrmDestroyDatabase(xdb);
	}
}

/* get color from color string */
static void
getcolor(const char *s, XftColor *color)
{
	if(!XftColorAllocName(dpy, visual, colormap, s, color))
		errx(1, "cannot allocate color: %s", s);
}

/* init draw context */
static void
setupdc(void)
{
	/* get color pixels */
	getcolor(background,    &dc.normal[ColorBG]);
	getcolor(foreground,    &dc.normal[ColorFG]);
	getcolor(selbackground, &dc.selected[ColorBG]);
	getcolor(selforeground, &dc.selected[ColorFG]);
	getcolor(separator,     &dc.decoration[ColorBG]);
	getcolor(border,        &dc.decoration[ColorFG]);

	/* try to get font */
	if ((dc.font = XftFontOpenName(dpy, screen, font)) == NULL)
		errx(1, "cannot load font");

	/* create GC */
	dc.gc = XCreateGC(dpy, rootwin, 0, NULL);
}

/* init menu geometry values */
static void
setupgeom(void)
{
	geom.itemb = itemborder;
	geom.itemh = dc.font->height + itemborder * 2;
	geom.itemw = width;
	geom.border = menuborder;
	geom.separator = separatorsize;
}

/* allocate an item */
static struct Item *
allocitem(const char *label, const char *output)
{
	struct Item *item;

	if ((item = malloc(sizeof *item)) == NULL)
		err(1, "malloc");
	if (*label == '\0') {
		item->label = NULL;
		item->output = NULL;
	} else {
		if ((item->label = strdup(label)) == NULL)
			err(1, "strdup");
		if ((item->output = strdup(output)) == NULL)
			err(1, "strdup");
	}
	item->y = 0;
	item->h = item->label ? geom.itemh : geom.separator;
	if (item->label == NULL)
		item->labellen = 0;
	else
		item->labellen = strlen(item->label);
	item->next = NULL;
	item->submenu = NULL;

	return item;
}

/* allocate a menu */
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
	menu->w = geom.itemw;
	menu->h = 0;    /* calculated by calcmenu() */
	menu->x = 0;    /* calculated by calcmenu() */
	menu->y = 0;    /* calculated by calcmenu() */
	menu->level = level;

	swa.override_redirect = override_redirect;
	swa.background_pixel = dc.decoration[ColorBG].pixel;
	swa.border_pixel = dc.decoration[ColorFG].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask
	               | PointerMotionMask | LeaveWindowMask;
	menu->win = XCreateWindow(dpy, rootwin, 0, 0, geom.itemw, geom.itemh, geom.border,
	                          CopyFromParent, CopyFromParent, CopyFromParent,
	                          CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
	                          &swa);

	XSetWMProtocols(dpy, menu->win, &wmdelete, 1);

	return menu;
}

/* create menus and items from the stdin */
static void
parsestdin(void)
{
	char *s, buf[BUFSIZ];
	char *label, *output;
	unsigned level = 0;
	unsigned i;
	struct Item *curritem = NULL;   /* item currently being read */
 	struct Menu *prevmenu = NULL;   /* menu the previous item was added to */
	struct Item *item;              /* dummy item for for loops */
	struct Menu *menu;              /* dummy menu for for loops */
 	size_t count = 0;   /* number of items in the current menu */

	while (fgets(buf, BUFSIZ, stdin) != NULL) {
		level = 0;
		s = buf;

		while (*s == '\t') {
			level++;
			s++;
		}

		label = output = s;

		while (*s != '\0' && *s != '\t' && *s != '\n')
			s++;

		while (*s == '\t')
			*s++ = '\0';

		if (*s != '\0' && *s != '\n')
			output = s;

		while (*s != '\0' && *s != '\n')
			s++;

		if (*s == '\n')
			*s = '\0';

		curritem = allocitem(label, output);

		if (prevmenu == NULL) {                 /* there is no menu yet */
			 menu = allocmenu(NULL, curritem, level);
			 rootmenu = menu;
			 prevmenu = menu;
			 count = 1;
			 curritem->prev = NULL;
			 curritem->next = NULL;
		} else if (level < prevmenu->level) {   /* item is continuation of a parent menu*/
			for (menu = prevmenu, i = level;
			      menu != NULL && i < prevmenu->level;
			      menu = menu->parent, i++)
				;

			if (menu == NULL)
				errx(1, "reached NULL menu");

			for (item = menu->list; item->next != NULL; item = item->next)
				;

			item->next = curritem;

			curritem->prev = item;
			curritem->next = NULL;

			prevmenu = menu;
		} else if (level == prevmenu->level) {  /* item is a continuation of current menu */
			for (item = prevmenu->list; item->next != NULL; item = item->next)
				;
			item->next = curritem;

			curritem->prev = item;
			curritem->next = NULL;

		} else if (level > prevmenu->level) {   /* item begins a new menu */
			menu = allocmenu(prevmenu, curritem, level);

			for (item = prevmenu->list; item->next != NULL; item = item->next)
				;

			item->submenu = menu;
			menu->caller = item;

			curritem->prev = NULL;
			curritem->next = NULL;

			prevmenu = menu;
		}
		count++;
	}
}

/* calculate screen geometry */
static void
calcscreengeom(void)
{
	Window w1, w2;  /* unused variables */
	int a, b;       /* unused variables */
	unsigned mask;  /* unused variable */

	XQueryPointer(dpy, rootwin, &w1, &w2, &screengeom.cursx, &screengeom.cursy, &a, &b, &mask);
	screengeom.screenw = DisplayWidth(dpy, screen);
	screengeom.screenh = DisplayHeight(dpy, screen);
}

/* recursivelly calculate height and position of the menus */
static void
calcmenu(struct Menu *menu)
{
	static XClassHint classh = {PROGNAME, PROGNAME};
	XWindowChanges changes;
	XTextProperty textprop;
	XSizeHints sizeh;
	XGlyphInfo ext;
	struct Item *item;
	int labelwidth;

	/* calculate items positions and menu width and height */
	menu->w = geom.itemw;
	for (item = menu->list; item != NULL; item = item->next) {
		item->y = menu->h;
		if (item->label == NULL)   /* height for separator item */
			menu->h += geom.separator;
		else
			menu->h += geom.itemh;

		XftTextExtentsUtf8(dpy, dc.font, (XftChar8 *)item->label,
		                   item->labellen, &ext);
		labelwidth = ext.xOff + dc.font->height * 2;
		menu->w = MAX(menu->w, labelwidth);
	}

	/* calculate menu's x and y positions */
	if (menu->parent == NULL) { /* if root menu, calculate in respect to cursor */
		if (screengeom.screenw - screengeom.cursx >= menu->w)
			menu->x = screengeom.cursx;
		else if (screengeom.cursx > menu->w)
			menu->x = screengeom.cursx - menu->w;

		if (screengeom.screenh - screengeom.cursy >= menu->h)
			menu->y = screengeom.cursy;
		else if (screengeom.screenh > menu->h)
			menu->y = screengeom.screenh - menu->h;

		XStringListToTextProperty(menutitle, menutitlecount, &textprop);
	} else {                    /* else, calculate in respect to parent menu */
		if (screengeom.screenw - (menu->parent->x + menu->parent->w + geom.border) >= menu->w)
			menu->x = menu->parent->x + menu->parent->w + geom.border;
		else if (menu->parent->x > menu->w + geom.border)
			menu->x = menu->parent->x - menu->w - geom.border;

		if (screengeom.screenh - (menu->caller->y + menu->parent->y) > menu->h)
			menu->y = menu->caller->y + menu->parent->y;
		else if (screengeom.screenh - menu->parent->y > menu->h)
			menu->y = menu->parent->y;
		else if (screengeom.screenh > menu->h)
			menu->y = screengeom.screenh - menu->h;

		XStringListToTextProperty(&(menu->caller->output), 1, &textprop);
	}

	/* update menu geometry */
	changes.height = menu->h;
	changes.width = menu->w;
	changes.x = menu->x;
	changes.y = menu->y;
	XConfigureWindow(dpy, menu->win, CWWidth | CWHeight | CWX | CWY, &changes);

	/* set window manager hints */
	sizeh.flags = PMaxSize | PMinSize;
	sizeh.min_width = sizeh.max_width = menu->w;
	sizeh.min_height = sizeh.max_height = menu->h;
	XSetWMProperties(dpy, menu->win, &textprop, NULL, NULL, 0, &sizeh,
	                 NULL, &classh);

	/* create pixmap and XftDraw */
	menu->pixmap = XCreatePixmap(dpy, menu->win, menu->w, menu->h,
	                             DefaultDepth(dpy, screen));
	menu->draw = XftDrawCreate(dpy, menu->pixmap, visual, colormap);

	/* calculate positions of submenus */
	for (item = menu->list; item != NULL; item = item->next) {
		if (item->submenu != NULL)
			calcmenu(item->submenu);
	}
}

/* recalculate menu position in respect to its parent */
static void
recalcmenu(struct Menu *menu)
{
	XWindowAttributes parentwin;

	if (menu->parent == NULL)
		return;

	XGetWindowAttributes(dpy, menu->parent->win, &parentwin);

	if (screengeom.screenw - (parentwin.x + menu->parent->w + geom.border) >= menu->w)
		menu->x = parentwin.x + menu->parent->w + geom.border;
	else if (parentwin.x > menu->w + geom.border)
		menu->x = parentwin.x - menu->w - geom.border;

	if (screengeom.screenh - (menu->caller->y + parentwin.y) > menu->h)
		menu->y = menu->caller->y + parentwin.y;
	else if (screengeom.screenh - parentwin.y > menu->h)
		menu->y = parentwin.y;
	else if (screengeom.screenh > menu->h)
		menu->y = screengeom.screenh - menu->h;

	XMoveWindow(dpy, menu->win, menu->x, menu->y);
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
	errx(1, "cannot grab keyboard");
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
	errx(1, "cannot grab keyboard");
}

/* get menu and item of given window and position */
static void
getmenuitem(Window win, int y,
            struct Menu **menu_ret, struct Item **item_ret)
{
	struct Menu *menu = NULL;
	struct Item *item = NULL;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		if (menu->win == win) {
			for (item = menu->list; item != NULL; item = item->next) {
				if (y >= item->y && y <= item->y + item->h) {
					goto done;
				}
			}
		}
	}


done:
	*menu_ret = menu;
	*item_ret = item;
}

/* set currentmenu to menu, umap previous menus and map current menu and its parents */
static void
setcurrmenu(struct Menu *currmenu_new)
{
	struct Menu *menu, *menu_;
	struct Item *item;
	struct Menu *lcamenu;   /* lowest common ancestor menu */
	unsigned minlevel;      /* level of the closest to root menu */
	unsigned maxlevel;      /* level of the closest to root menu */

	if (currmenu_new == currmenu)
		return;

	/* find lowest common ancestor menu */
	lcamenu = rootmenu;
	if (currmenu != NULL) {
		minlevel = MIN(currmenu_new->level, currmenu->level);
		maxlevel = MAX(currmenu_new->level, currmenu->level);
		if (currmenu_new->level == maxlevel) {
			menu = currmenu_new;
			menu_ = currmenu;
		} else {
			menu = currmenu;
			menu_ = currmenu_new;
		}
		while (menu->level > minlevel)
			menu = menu->parent;

		while (menu != menu_) {
			menu = menu->parent;
			menu_ = menu_->parent;
		}
		lcamenu = menu;
	}

	/* unmap menus from currmenu (inclusive) until lcamenu (exclusive) */
	for (menu = currmenu; menu != lcamenu; menu = menu->parent) {
		menu->selected = NULL;
		XUnmapWindow(dpy, menu->win);
	}

	currmenu = currmenu_new;

	/* map menus from currmenu (inclusive) until lcamenu (exclusive) */
	item = NULL;
	for (menu = currmenu; menu != lcamenu; menu = menu->parent) {
		if (override_redirect == False)
			recalcmenu(menu);
		XMapWindow(dpy, menu->win);
		if (item != NULL)
			menu->selected = item;
		item = menu->caller;
	}
}

/* draw items of the current menu and of its ancestors */
static void
drawmenu(void)
{
	struct Menu *menu;
	struct Item *item;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		for (item = menu->list; item != NULL; item = item->next) {
			XftColor *color;
			int labelx, labely;

			/* determine item color */
			if (item == menu->selected)
				color = dc.selected;
			else
				color = dc.normal;

			/* continue if item is a separator */
			if (item->label == NULL)
				continue;

			/* draw item box */
			XSetForeground(dpy, dc.gc, color[ColorBG].pixel);
			XDrawRectangle(dpy, menu->pixmap, dc.gc, 0, item->y,
			               menu->w, item->h);
			XFillRectangle(dpy, menu->pixmap, dc.gc, 0, item->y,
			               menu->w, item->h);

			/* draw item label */
			labelx = 0 + dc.font->height;
			labely = item->y + dc.font->height + geom.itemb / 2;
			XSetForeground(dpy, dc.gc, color[ColorFG].pixel);
			XftDrawStringUtf8(menu->draw, &color[ColorFG], dc.font,
			                  labelx, labely, item->label,
			                  item->labellen);

			/* draw triangle, if item contains a submenu */
			if (item->submenu != NULL) {
				int trianglex = menu->w - dc.font->height + geom.itemb - 1;
				int triangley = item->y + (3 * item->h)/8 -1;

				XPoint triangle[] = {
					{trianglex, triangley},
					{trianglex + item->h/8 + 1, item->y + item->h/2},
					{trianglex, triangley + item->h/4 + 2},
					{trianglex, triangley}
				};

				XFillPolygon(dpy, menu->pixmap, dc.gc, triangle, LEN(triangle),
				             Convex, CoordModeOrigin);
			}

			XCopyArea(dpy, menu->pixmap, menu->win, dc.gc, 0, item->y,
			          menu->w, item->h, 0, item->y);
		}
	}
}

/* cycle through the items; non-zero direction is next, zero is prev */
static struct Item *
itemcycle(int direction)
{
	struct Item *item;
	struct Item *lastitem;

	item = NULL;

	if (direction == ITEMNEXT) {
		if (currmenu->selected == NULL)
			item = currmenu->list;
		else if (currmenu->selected->next != NULL)
			item = currmenu->selected->next;

		while (item != NULL && item->label == NULL)
			item = item->next;

		if (item == NULL)
			item = currmenu->list;
	} else {
		for (lastitem = currmenu->list;
		     lastitem != NULL && lastitem->next != NULL;
		     lastitem = lastitem->next)
			;

		if (currmenu->selected == NULL)
			item = lastitem;
		else if (currmenu->selected->prev != NULL)
			item = currmenu->selected->prev;

		while (item != NULL && item->label == NULL)
			item = item->prev;

		if (item == NULL)
			item = lastitem;
	}

	return item;
}

/* run event loop */
static void
run(void)
{
	struct Menu *menu;
	struct Item *item;
	struct Item *previtem = NULL;
	KeySym ksym;
	XEvent ev;

	while (!XNextEvent(dpy, &ev)) {
		switch(ev.type) {
		case Expose:
			if (ev.xexpose.count == 0)
				drawmenu();
			break;
		case MotionNotify:
			getmenuitem(ev.xbutton.window, ev.xbutton.y, &menu, &item);
			if (menu != NULL && item != NULL) {
				if (previtem != item) {
					if (item->submenu != NULL)
						setcurrmenu(item->submenu);
					else
						setcurrmenu(menu);
					previtem = item;
					drawmenu();
				} else if (menu->selected != item) {
					menu->selected = item;
					drawmenu();
				}
			}
			break;
		case ButtonRelease:
			getmenuitem(ev.xbutton.window, ev.xbutton.y, &menu, &item);
			if (menu != NULL && item != NULL) {
selectitem:
				if (item->label == NULL)
					break;  /* ignore separators */
				if (item->submenu != NULL) {
					setcurrmenu(item->submenu);
				} else {
					printf("%s\n", item->output);
					return;
				}
				currmenu->selected = currmenu->list;
				drawmenu();
				break;
			} else {
				return;
			}
		case ButtonPress:
			getmenuitem(ev.xbutton.window, ev.xbutton.y, &menu, &item);
			if (menu == NULL || item == NULL)
				return;
			break;
		case KeyPress:
			ksym = XkbKeycodeToKeysym(dpy, ev.xkey.keycode, 0, 0);

			if (ksym == XK_Escape && currmenu == rootmenu)
				return;

			/* Shift-Tab = ISO_Left_Tab */
			if (ksym == XK_Tab && (ev.xkey.state & ShiftMask))
				ksym = XK_ISO_Left_Tab;

			/* cycle through menu */
			item = NULL;
			if (ksym == XK_ISO_Left_Tab || ksym == XK_Up) {
				item = itemcycle(ITEMPREV);
			} else if (ksym == XK_Tab || ksym == XK_Down) {
				item = itemcycle(ITEMNEXT);
			} else if ((ksym == XK_Return || ksym == XK_Right) &&
			           currmenu->selected != NULL) {
				item = currmenu->selected;
				goto selectitem;
			} else if ((ksym == XK_Escape || ksym == XK_Left) &&
			           currmenu->parent != NULL) {
				item = currmenu->parent->selected;
				setcurrmenu(currmenu->parent);
			} else
				break;
			currmenu->selected = item;
			drawmenu();
			break;
		case LeaveNotify:
			currmenu->selected = NULL;
			drawmenu();
			break;
		case ClientMessage:     /* user closed a window */
			return;
		}
	}
}

/* recursivelly free a pixmap */
static void
freewindow(struct Menu *menu)
{
	struct Item *item;

	for (item = menu->list; item != NULL; item = item->next)
		if (item->submenu != NULL)
			freewindow(item->submenu);

	XFreePixmap(dpy, menu->pixmap);
	XftDrawDestroy(menu->draw);
	XDestroyWindow(dpy, menu->win);
}

/* cleanup and exit */
static void
cleanup(void)
{
	freewindow(rootmenu);

	XftColorFree(dpy, visual, colormap, &dc.normal[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.normal[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.decoration[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.decoration[ColorFG]);

	XFreeGC(dpy, dc.gc);
	XCloseDisplay(dpy);
}

/* show usage */
static void
usage(void)
{
	(void)fprintf(stderr, "usage: xmenu [-w] title...\n");
	exit(1);
}
