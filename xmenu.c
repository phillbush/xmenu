#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* macros */
#define LEN(x) (sizeof (x) / sizeof (x[0]))
#define MAX(x,y) ((x)>(y)?(x):(y))
#define MIN(x,y) ((x)<(y)?(x):(y))

/* color enum */
enum {ColorFG, ColorBG, ColorLast};

/* draw context structure */
struct DC {
	unsigned long unpressed[ColorLast];
	unsigned long pressed[ColorLast];
	unsigned long decoration[ColorLast];

	Drawable d;
	GC gc;
	XFontStruct *font;
	int fonth;
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
	Window win;             /* menu window to map on the screen */
};

/* function declarations */
static unsigned long getcolor(const char *s);
static void setupdc(void);
static void setupgeom(void);
static void setupgrab(void);
static struct Item *allocitem(const char *label, const char *output);
static struct Menu *allocmenu(struct Menu *parent, struct Item *list, unsigned level);
static void getmenuitem(Window win, int y, struct Menu **menu_ret, struct Item **item_ret);
static void drawmenu(void);
static void calcscreengeom(void);
static void calcmenu(struct Menu *menu);
static void setcurrmenu(struct Menu *currmenu_new);
static void parsestdin(void);
static void run(void);
static void freewindow(struct Menu *menu);
static void cleanupexit(void);
static void usage(void);

/* X variables */
static Colormap colormap;
static Display *dpy;
static Window rootwin;
static int screen;
static struct DC dc;

/* menu variables */
static struct Menu *rootmenu = NULL;
static struct Menu *currmenu = NULL;

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

	/* open connection to server and set X variables */
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "cannot open display");
	screen = DefaultScreen(dpy);
	rootwin = RootWindow(dpy, screen);
	colormap = DefaultColormap(dpy, screen);

	/* setup */
	setupdc();
	setupgeom();
	setupgrab();

	/* generate menus and recalculate them */
	parsestdin();
	if (rootmenu == NULL)
		errx(1, "no menu generated");
	calcscreengeom();
	calcmenu(rootmenu);

	/* map root menu */
	currmenu = rootmenu;
	XMapWindow(dpy, rootmenu->win);

	/* run event loop */
	run();

	return 1;   /* UNREACHABLE */
}

/* get color from color string */
static unsigned long
getcolor(const char *s)
{
	XColor color;

	if(!XAllocNamedColor(dpy, colormap, s, &color, &color))
		errx(1, "cannot allocate color: %s", s);
	return color.pixel;
}

/* init draw context */
static void
setupdc(void)
{
	/* get color pixels */
	dc.unpressed[ColorBG] = getcolor(UNPRESSEDBG);
	dc.unpressed[ColorFG] = getcolor(UNPRESSEDFG);
	dc.pressed[ColorBG] = getcolor(PRESSEDBG);
	dc.pressed[ColorFG] = getcolor(PRESSEDFG);
	dc.decoration[ColorBG] = getcolor(DECORATIONBG);
	dc.decoration[ColorFG] = getcolor(DECORATIONFG);

	/* try to get font */
	if ((dc.font = XLoadQueryFont(dpy, FONT)) == NULL)
		errx(1, "cannot load font");
	dc.fonth = dc.font->ascent + dc.font->descent;

	/* create GC and set its font */
	dc.gc = XCreateGC(dpy, rootwin, 0, NULL);
	XSetFont(dpy, dc.gc, dc.font->fid);
}

/* init menu geometry values */
static void
setupgeom(void)
{
	geom.itemb = ITEMB;
	geom.itemh = dc.fonth + ITEMB * 2;
	geom.itemw = ITEMW;
	geom.border = BORDER;
	geom.separator = SEPARATOR;
}

/* grab pointer */
static void
setupgrab(void)
{
	XGrabPointer(dpy, rootwin, True, ButtonPressMask | ButtonReleaseMask,
	             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
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
	swa.background_pixel = dc.decoration[ColorBG];
	swa.border_pixel = dc.decoration[ColorFG];
	swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask
	               | PointerMotionMask | LeaveWindowMask;
	menu->win = XCreateWindow(dpy, rootwin, 0, 0, geom.itemw, geom.itemh, geom.border,
	                          CopyFromParent, CopyFromParent, CopyFromParent,
	                          CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
	                          &swa);

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
			prevmenu = menu;
		} else if (level == prevmenu->level) {  /* item is a continuation of current menu */
			for (item = prevmenu->list; item->next != NULL; item = item->next)
				;
			item->next = curritem;
		} else if (level > prevmenu->level) {   /* item begins a new menu */
			menu = allocmenu(prevmenu, curritem, level);

			for (item = prevmenu->list; item->next != NULL; item = item->next)
				;

			item->submenu = menu;
			menu->caller = item;

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
	XWindowChanges changes;
	XSizeHints sizeh;
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

		labelwidth = XTextWidth(dc.font, item->label, item->labellen) + dc.fonth * 2;
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
	} else {                    /* else, calculate in respect to parent menu */

		/* search for the item in parent menu that generates this menu */
		for (item = menu->parent->list; item->submenu != menu; item = item->next)
			;

		if (screengeom.screenw - (menu->parent->x + menu->parent->w) >= menu->w)
			menu->x = menu->parent->x + menu->parent->w;
		else if (menu->parent->x > menu->w)
			menu->x = menu->parent->x - menu->w;

		if (screengeom.screenh - (item->y + menu->parent->y) > menu->h)
			menu->y = item->y + menu->parent->y;
		else if (screengeom.screenh - menu->parent->y > menu->h)
			menu->y = menu->parent->y;
		else if (screengeom.screenh > menu->h)
			menu->y = screengeom.screenh - menu->h;
	}

	/* update menu geometry */
	changes.height = menu->h;
	changes.width = menu->w;
	changes.x = menu->x;
	changes.y = menu->y;
	XConfigureWindow(dpy, menu->win, CWWidth | CWHeight | CWX | CWY, &changes);

	/* set window manager size hints */
	sizeh.flags = PMaxSize | PMinSize;
	sizeh.min_width = sizeh.max_width = menu->w;
	sizeh.min_height = sizeh.max_height = menu->h;
	XSetWMNormalHints(dpy, menu->win, &sizeh);

	/* create pixmap */
	menu->pixmap = XCreatePixmap(dpy, menu->win, menu->w, menu->h,
	                             DefaultDepth(dpy, screen));

	/* calculate positions of submenus */
	for (item = menu->list; item != NULL; item = item->next) {
		if (item->submenu != NULL)
			calcmenu(item->submenu);
	}
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
		XUnmapWindow(dpy, menu->win);
	}

	currmenu = currmenu_new;

	/* map menus from currmenu (inclusive) until lcamenu (exclusive) */
	item = NULL;
	for (menu = currmenu; menu != lcamenu; menu = menu->parent) {
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
			unsigned long *color;
			int labelx, labely;

			/* determine item color */
			if (item->label == NULL)
				color = dc.decoration;
			else if (item == menu->selected)
				color = dc.pressed;
			else
				color = dc.unpressed;

			/* draw item box */
			XSetForeground(dpy, dc.gc, color[ColorBG]);
			XDrawRectangle(dpy, menu->pixmap, dc.gc, 0, item->y,
			               menu->w, item->h);
			XFillRectangle(dpy, menu->pixmap, dc.gc, 0, item->y,
			               menu->w, item->h);

			/* continue if item is a separator */
			if (item->label == NULL)
				continue;

			/* draw item label */
			labelx = 0 + dc.fonth;
			labely = item->y + dc.fonth + geom.itemb;
			XSetForeground(dpy, dc.gc, color[ColorFG]);
			XDrawString(dpy, menu->pixmap, dc.gc, labelx, labely,
			            item->label, item->labellen);

			/* draw triangle, if item contains a submenu */
			if (item->submenu != NULL) {
				int trianglex = menu->w - dc.fonth + geom.itemb - 1;
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

/* run event loop */
static void
run(void)
{
	struct Menu *menu;
	struct Item *item;
	struct Item *previtem = NULL;
	XEvent ev;

	setcurrmenu(rootmenu);

	while (!XNextEvent(dpy, &ev)) {
		switch(ev.type) {
		case Expose:
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
				if (item->label == NULL)
					break;  /* ignore separators */
				if (item->submenu != NULL) {
					setcurrmenu(item->submenu);
				} else {
					printf("%s\n", item->output);
					cleanupexit();
				}
				drawmenu();
			} else {
				cleanupexit();
			}
			break;
		case LeaveNotify:
			currmenu->selected = NULL;
			drawmenu();
			break;
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
	XDestroyWindow(dpy, menu->win);
}

/* cleanup and exit */
static void
cleanupexit(void)
{
	freewindow(rootmenu);
	XFreeFont(dpy, dc.font);
	XFreeGC(dpy, dc.gc);
	XCloseDisplay(dpy);
	exit(0);
}

/* show usage */
static void
usage(void)
{
	(void)fprintf(stderr, "usage: xmenu [-w] menuname\n");
	exit(1);
}
