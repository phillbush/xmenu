#define PROGNAME "xmenu"

/* enum for keyboard menu navigation */
enum { ITEMPREV, ITEMNEXT, ITEMFIRST, ITEMLAST };

/* macros */
#define LEN(x)              (sizeof (x) / sizeof (x[0]))
#define MAX(x,y)            ((x)>(y)?(x):(y))
#define MIN(x,y)            ((x)<(y)?(x):(y))
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))

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
	int separator_pixels;
	int gap_pixels;
	int triangle_width;
	int triangle_height;
	int iconpadding;
	int horzpadding;

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
	struct Item *selected;  /* item currently selected in the menu */
	int x, y, w, h;         /* menu geometry */
	int hasicon;            /* whether the menu has item with icons */
	int drawn;              /* whether the menu was already drawn */
	unsigned level;         /* menu level relative to root */
	Window win;             /* menu window to map on the screen */
};
