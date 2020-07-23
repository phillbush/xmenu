#define PROGNAME "xmenu"

/* macros for keyboard menu navigation */
#define ITEMPREV 0
#define ITEMNEXT 1

/* macros */
#define LEN(x) (sizeof (x) / sizeof (x[0]))
#define MAX(x,y) ((x)>(y)?(x):(y))
#define MIN(x,y) ((x)<(y)?(x):(y))

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

	/* the values below are computed by xmenu */
	int iconsize;
	int posx, posy;           /* cursor position */
	int screenw, screenh;       /* screen width and height */
};

/* draw context structure */
struct DC {
	XftColor normal[ColorLast];
	XftColor selected[ColorLast];
	XftColor border;
	XftColor separator;

	GC gc;
	XftFont *font;
};

/* menu item structure */
struct Item {
	char *label;            /* string to be drawed on menu */
	char *output;           /* string to be outputed when item is clicked */
	char *file;             /* filename of the icon */
	int y;                  /* item y position relative to menu */
	int h;                  /* item height */
	size_t labellen;        /* strlen(label) */
	struct Item *prev;      /* previous item */
	struct Item *next;      /* next item */
	struct Menu *submenu;   /* submenu spawned by clicking on item */
	Imlib_Image icon;
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
