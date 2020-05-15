#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* macros */
#define LEN(x) (sizeof (x) / sizeof (x[0]))

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
	int border;     /* window border */
};

/* screen geometry structure */
struct ScreenGeometry {
	int cursx, cursy;       /* cursor position */
	int screenw, screenh;   /* screen width and height */
};

/* menu item structure */
struct Item {
	char *label;
	char *output;
	int x, y;
	struct Item *next;
	struct Menu *submenu;
};

/* menu structure */
struct Menu {
	struct Menu *parent;
	struct Item *list;
	struct Item *selected;
	int x, y, w, h;
	unsigned level;
	unsigned nitems;
	Window win;
};

/* function declarations */
static unsigned long getcolor(const char *s);
static void setupdc(void);
static void setupgeom(void);
static void setupgrab(void);
static struct Item *allocitem(size_t count, const char *label, const char *output);
static struct Menu *allocmenu(struct Menu *parent, struct Item *list, unsigned level);
static void getmenuitem(Window win, int x, int y,
                        struct Menu **menu_ret, struct Item **item_ret);
static void drawmenu(void);
static void calcscreengeom(void);
static void calcmenu(struct Menu *menu);
static void setcurrmenu(struct Menu *currmenu_new);
static void parsestdin(void);
static void run(void);
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
static struct ScreenGeometry sgeom;

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
allocitem(size_t count, const char *label, const char *output)
{
	struct Item *item;

	if ((item = malloc(sizeof *item)) == NULL)
		err(1, "malloc");
	if ((item->label = strdup(label)) == NULL)
		err(1, "strdup");
	if ((item->output = strdup(output)) == NULL)
		err(1, "strdup");
	item->x = 0;
	item->y = count * geom.itemh;
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
	menu->selected = NULL;
	menu->x = 0;
	menu->y = 0;
	menu->w = geom.itemw;
	menu->h = geom.itemh;
	menu->level = level;
	menu->nitems = 0;

	swa.override_redirect = override_redirect;
	swa.background_pixel = dc.decoration[ColorBG];
	swa.border_pixel = dc.decoration[ColorFG];
	swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask
	               | PointerMotionMask;
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
	struct Item *item, *p;
	struct Menu *menu;
 	struct Menu *prevmenu = NULL;
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

		item = allocitem(count, label, output);

		if (prevmenu == NULL) {                 /* there is no menu yet */
			 menu = allocmenu(NULL, item, level);
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

			for (p = menu->list; p->next != NULL; p = p->next)
				;

			p->next = item;
			prevmenu = menu;
		} else if (level == prevmenu->level) {  /* item is a continuation of current menu */
			for (p = prevmenu->list; p->next != NULL; p = p->next)
				;
			p->next = item;
		} else if (level > prevmenu->level) {   /* item begins a new menu */
			menu = allocmenu(prevmenu, item, level);

			for (p = prevmenu->list; p->next != NULL; p = p->next)
				;

			p->submenu = menu;

			prevmenu = menu;
		}
	}
}

/* calculate screen geometry */
static void
calcscreengeom(void)
{
	Window w1, w2;  /* unused variables */
	int a, b;       /* unused variables */
	unsigned mask;  /* unused variable */

	XQueryPointer(dpy, rootwin, &w1, &w2, &sgeom.cursx, &sgeom.cursy, &a, &b, &mask);
	sgeom.screenw = DisplayWidth(dpy, screen);
	sgeom.screenh = DisplayHeight(dpy, screen);
}

/* recursivelly calculate height and position of the menus */
static void
calcmenu(struct Menu *menu)
{
	XWindowChanges changes;
	struct Item *item, *p;
	size_t i;

	/* calculate number of items */
	i = 0;
	for (item = menu->list; item != NULL; item = item->next)
		i++;
	menu->nitems = i;
	menu->h = geom.itemh * i;

	/* calculate menu's x and y positions */
	if (menu->parent == NULL) { /* if root menu, calculate in respect to cursor */
		if (sgeom.screenw - sgeom.cursx >= menu->w)
			menu->x = sgeom.cursx;
		else if (sgeom.cursx > menu->w)
			menu->x = sgeom.cursx - menu->w;

		if (sgeom.screenh - sgeom.cursy >= menu->h)
			menu->y = sgeom.cursy;
		else if (sgeom.screenh > menu->h)
			menu->y = sgeom.screenh - menu->h;
	} else {                    /* else, calculate in respect to parent menu */

		/* search for the item in parent menu that generates this menu */
		for (p = menu->parent->list; p->submenu != menu; p = p->next)
			;

		if (sgeom.screenw - (menu->parent->x + menu->parent->w) >= menu->w)
			menu->x = menu->parent->x + menu->parent->w;
		else if (menu->parent->x > menu->w)
			menu->x = menu->parent->x - menu->w;

		if (sgeom.screenh - p->y > menu->h)
			menu->y = p->y;
		else if (sgeom.screenh - menu->parent->y > menu->h)
			menu->y = menu->parent->y;
		else if (sgeom.screenh > menu->h)
			menu->y = sgeom.screenh - menu->h;
	}

	/* calculate position of each item in the menu */
	for (i = 0, item = menu->list; item != NULL; item = item->next, i++) {
		item->x = menu->x;
		item->y = menu->y + i * geom.itemh;
	}

	/* update menu geometry */
	changes.height = menu->h;
	changes.x = menu->x;
	changes.y = menu->y;
	XConfigureWindow(dpy, menu->win, CWHeight | CWX | CWY, &changes);

	for (item = menu->list; item != NULL; item = item->next) {
		if (item->submenu != NULL)
			calcmenu(item->submenu);
	}
}

/* get menu and item of given window and position */
static void
getmenuitem(Window win, int x, int y,
            struct Menu **menu_ret, struct Item **item_ret)
{
	struct Menu *menu = NULL;
	struct Item *item = NULL;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		if (menu->win == win) {
			for (item = menu->list; item != NULL; item = item->next) {
				if (x >= item->x && x <= item->x + geom.itemw &&
				    y >= item->y && y <= item->y + geom.itemh) {
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
	struct Menu *menu;

	if (currmenu_new == currmenu)
		return;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		XUnmapWindow(dpy, menu->win);
	}

	currmenu = currmenu_new;

	for (menu = currmenu; menu != NULL; menu = menu->parent)
		XMapWindow(dpy, menu->win);
}

/* draw items of the current menu and of its ancestors */
static void
drawmenu(void)
{
	struct Menu *menu;
	struct Item *item;

	for (menu = currmenu; menu != NULL; menu = menu->parent) {
		size_t nitems;      /* number of items before current item */

		nitems = 0;
		for (item = menu->list; item != NULL; item = item->next) {
			unsigned long *color;
			size_t labellen;
			int labelx, labely;
			int y;

			/* determine item color */
			if (item == menu->selected)
				color = dc.pressed;
			else
				color = dc.unpressed;

			/* calculate item's y position */
			y = nitems * geom.itemh;

			/* draw item box */
			XSetForeground(dpy, dc.gc, color[ColorBG]);
			XFillRectangle(dpy, menu->win, dc.gc, 0, y,
			               geom.itemw, geom.itemh);

			/* draw item label */
			labellen = strlen(item->label);
			labelx = 0 + dc.fonth;
			labely = y + dc.fonth + geom.itemb;
			XSetForeground(dpy, dc.gc, color[ColorFG]);
			XDrawString(dpy, menu->win, dc.gc, labelx, labely, item->label, labellen);

			/* draw triangle, if item contains a submenu */
			if (item->submenu != NULL) {
				int trianglex = geom.itemw - (geom.itemb + dc.fonth);
				int triangley = y + geom.itemb;

				XPoint triangle[] = {
					{trianglex, triangley},
					{trianglex + dc.fonth, triangley + dc.fonth/2},
					{trianglex, triangley + dc.fonth},
					{trianglex, triangley}
				};

				XFillPolygon(dpy, menu->win, dc.gc, triangle, LEN(triangle),
				             Convex, CoordModeOrigin);
			}

			nitems++;
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
			getmenuitem(ev.xbutton.window, ev.xbutton.x_root, ev.xbutton.y_root,
			            &menu, &item);
			if (menu != NULL && item != NULL) {
				if (previtem != item) {
					if (item->submenu != NULL)
						setcurrmenu(item->submenu);
					else
						setcurrmenu(menu);
					previtem = item;
				} else if (menu->selected != item)
					menu->selected = item;
			}
			drawmenu();
			break;
		case ButtonRelease:
			getmenuitem(ev.xbutton.window, ev.xbutton.x_root, ev.xbutton.y_root,
			            &menu, &item);
			if (menu != NULL && item != NULL) {
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
		}
	}
}

/* cleanup and exit */
static void
cleanupexit(void)
{
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
