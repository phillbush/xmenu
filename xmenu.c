#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <Imlib2.h>

#include "ctrlfnt.h"

#define CLASS                   "XMenu"
#define NAME                    "xmenu"
#define LEN(a)                  (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b)               ((a)>(b)?(a):(b))
#define MIN(a, b)               ((a)<(b)?(a):(b))
#define FLAG(f, b)              (((f) & (b)) == (b))
#define MAXPATHS                32
#define RETURN_FAILURE          (-1)
#define RETURN_SUCCESS          0
#define PADDING                 4
#define INITIAL_DISPLACEMENT    2
#define MIN_HEIGHT              (16 + PADDING * 2)
#define TRIANGLE_HEIGHT         8
#define TRIANGLE_WIDTH          3
#define TRIANGLE_PAD            8
#define SCROLL_TIME             128
#define DASH_SIZE               8

#define ATOMS                                   \
	X(UTF8_STRING)                          \
	X(WM_DELETE_WINDOW)                     \
	X(_NET_WM_NAME)                         \
	X(_NET_WM_WINDOW_TYPE)                  \
	X(_NET_WM_WINDOW_TYPE_MENU)             \
	X(_NET_WM_WINDOW_TYPE_POPUP_MENU)

#define RESOURCES                                                         \
	/* deprecated resources                                        */ \
	X(_SELECT_BG,   "Selbackground",        "selbackground"         ) \
	X(_SELECT_FG,   "Selforeground",        "selforeground"         ) \
	X(_BORDER,      "Border",               "border"                ) \
	X(_SEPARAT,     "Separator",            "separator"             ) \
	X(_TEAROFF,     "TearOffModel",         "tearOffModel"          ) \
	/* current resources                                           */ \
	X(ALIGNMENT,    "Alignment",            "alignment"             ) \
	X(BORDER_CLR,   "BorderColor",          "borderColor"           ) \
	X(BORDER_WID,   "BorderWidth",          "borderWidth"           ) \
	X(FACE_NAME,    "FaceName",             "faceName"              ) \
	X(FACE_SIZE,    "FaceSize",             "faceSize"              ) \
	X(GAP_WID,      "Gap",                  "gap"                   ) \
	X(ICON_SIZE,    "IconSize",             "iconSize"              ) \
	X(MAX_ITEMS,    "MaxItems",             "maxItems"              ) \
	X(NORMAL_BG,    "Background",           "background"            ) \
	X(NORMAL_FG,    "Foreground",           "foreground"            ) \
	X(OPACITY,      "Opacity",              "opacity"               ) \
	X(SELECT_BG,    "ActiveBackground",     "activeBackground"      ) \
	X(SELECT_FG,    "ActiveForeground",     "activeForeground"      ) \
	X(SEPARAT_CLR,  "SeparatorColor",       "separatorColor"        ) \
	X(SHADOW_BOT,   "BottomShadowColor",    "bottomShadowColor"     ) \
	X(SHADOW_MID,   "MiddleShadowColor",    "middleShadowColor"     ) \
	X(SHADOW_TOP,   "TopShadowColor",       "topShadowColor"        ) \
	X(SHADOW_WID,   "ShadowThickness",      "shadowThickness"       ) \
	X(TEAROFF,      "TearOff",              "tearOff"               )

#define COLOR(r,g,b) (XRenderColor){.red=(r),.green=(g),.blue=(b),.alpha=0xFFFF}
#define DEF_COLOR_BG     COLOR(0x3100, 0x3100, 0x3100)
#define DEF_COLOR_FG     COLOR(0xFFFF, 0xFFFF, 0xFFFF)
#define DEF_COLOR_SELBG  COLOR(0x3400, 0x6500, 0xA400)
#define DEF_COLOR_SELFG  COLOR(0xFFFF, 0xFFFF, 0xFFFF)
#define DEF_COLOR_BRD    COLOR(0x0000, 0x0000, 0x0000)
#define DEF_COLOR_SHDTOP COLOR(0x7300, 0x7300, 0x7300)
#define DEF_COLOR_SHDBOT COLOR(0x1000, 0x1000, 0x1000)
#define DEF_BORDER      1
#define DEF_ICONSIZE    16
#define DEF_GAP         0
#define DEF_ALIGNMENT   ALIGN_LEFT

enum {
	SCHEME_NORMAL,
	SCHEME_SELECT,
	SCHEME_SHADOW,
	SCHEME_LAST,
};

enum {
	COLOR_BG = 0,
	COLOR_FG = 1,

	COLOR_TOP = 0,
	COLOR_BOT = 1,

	COLOR_LAST = 2,
};

enum {
	CANVAS_NORMAL,
	CANVAS_SELECT,
	CANVAS_FINAL,
	CANVAS_LAST
};

enum {
	LAYER_BG,
	LAYER_FG,
	LAYER_LAST
};

enum {
	DIR_UP,
	DIR_DOWN,
	DIR_LEFT,
	DIR_RIGHT
};

enum Atom {
#define X(atom) atom,
	ATOMS
	NATOMS
#undef  X
};

enum Resource {
#define X(res, s1, s2) res,
	RESOURCES
	NRESOURCES
#undef  X
};

typedef struct Item {
	char *label;            /* string to be drawed on menu */
	char *output;           /* string to be outputed when item is clicked */
	char *file;             /* filename of the icon */
	size_t labellen;
	struct Item *prev;
	struct Item *next;
	struct Item *parent;
	struct Item *children;
} Item;

typedef struct Menu {
	struct Menu *next;
	struct Item *caller, *items, *selected;
	struct Item *first, *last, *lastsave;
	XRectangle geometry;
	int selposition;
	Window window;
	struct Canvas {
		Pixmap pixmap;
		Picture picture;
	} canvas[CANVAS_LAST];
	size_t nicons;
	bool overflow;
	bool hasicon;
	bool hassubmenu;
} Menu;

typedef struct Options {
	Item *items;
	bool windowed;
	bool userplaced;
	bool xneg, yneg;
	bool monplaced;
	int monitor;
	int argc;
	char **argv;
	char *name;
	char *class;
	char *title;

	char *iconstring;
	char *iconpaths[MAXPATHS];
	size_t niconpaths;

	XRectangle geometry;
} Options;

typedef struct Widget {
	Display *display;
	Visual *visual;
	Atom atoms[NATOMS];
	Colormap colormap;
	int depth;
	int screen;
	int fd;
	Window rootwin;
	Window window;
	struct timespec lasttime;
	XRectangle monitor;
	XRenderPictFormat *xformat;
	XRenderPictFormat *alphaformat;
	CtrlFontSet *fontset;
	Cursor cursor;
	Menu *menus;
	unsigned int fonth;
	unsigned int itemh;
	int shadowwid, borderwid, iconsize, gap;
	int maxitems;
	bool initimlib;
	bool tearoff;
	enum {
		ALIGN_LEFT,
		ALIGN_CENTER,
		ALIGN_RIGHT,
	} alignment;

	struct Color {
		XRenderColor chans;
		Pixmap pix;
		Picture pict;
	} opacity, border, colors[SCHEME_LAST][COLOR_LAST];


	struct ResourcePair {
		XrmClass class;
		XrmName name;
	} application, resources[NRESOURCES];
} Widget;

static jmp_buf jmpenv;
static Options options = { 0 };
static Item tearoff = { .label = "tearoff" };
static Item scrollup = { .label = "scrollup" };
static Item scrolldown = { .label = "scrolldown" };

static void
usage(void)
{
	(void)fprintf(stderr, "usage: xmenu [-w] [-N name] "
	              "[-p position] [-x button]\n");
	exit(EXIT_FAILURE);
}

static void *
emalloc(size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		err(EXIT_FAILURE, "malloc");
	return p;
}

static char *
estrdup(const char *s)
{
	char *t;

	if ((t = strdup(s)) == NULL)
		err(EXIT_FAILURE, "strdup");
	return t;
}

static void
egettime(struct timespec *ts)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts) == RETURN_FAILURE) {
		err(EXIT_FAILURE, "clock_gettime");
	}
}

static pid_t
efork(void)
{
	pid_t pid;

	if ((pid = fork()) == -1)
		err(EXIT_FAILURE, "fork");
	return pid;
}

static void
setatoi(int *n, const char *s)
{
	char *endp;
	long l;

	if (s == NULL)
		return;
	l = strtol(s, &endp, 10);
	if (s[0] != '\0' && *endp == '\0' && l >= 0 && l < 100) {
		*n = l;
	}
}

static void
setatof(double *x, const char *s)
{
	char *endp;
	double d;

	if (s == NULL)
		return;
	d = strtod(s, &endp);
	if (s[0] != '\0' && *endp == '\0' && d >= 0.0 && d < 100.0) {
		*x = d;
	}
}

static void
parsegeometry(const char *str)
{
	char *geometry, *monitor;
	size_t span;
	unsigned int width, height;
	int flags, x, y;

	geometry = estrdup(str);
	span = strcspn(geometry, ":");
	monitor = &geometry[span];
	if (geometry[span] == ':')
		monitor++;
	geometry[span] = '\0';
	flags = XParseGeometry(geometry, &x, &y, &width, &height);
	options.monitor = -1;
	if (FLAG(flags, WidthValue|HeightValue)) {
		options.geometry.width = width;
		options.geometry.height = height;
	}
	if (FLAG(flags, XValue|YValue)) {
		if (FLAG(flags, XNegative))
			options.xneg = true;
		if (FLAG(flags, YNegative))
			options.yneg = true;
		options.geometry.x = x;
		options.geometry.y = y;
		options.userplaced = true;
	}
	setatoi(&options.monitor, monitor);
	free(geometry);
}

static void
parseiconpaths(char *s)
{
	if (s == NULL)
		return;
	options.iconstring = estrdup(s);
	options.niconpaths = 0;
	for (s = strtok(options.iconstring, ":"); s != NULL; s = strtok(NULL, ":")) {
		if (options.niconpaths < MAXPATHS) {
			options.iconpaths[options.niconpaths++] = s;
		}
	}
}

static void
parseoptions(int argc, char *argv[])
{
	int ch;

	options.argc = argc;
	options.argv = argv;
	options.class = CLASS;
	if (argv[0] != NULL && argv[0][0] != '\0') {
		options.name = strchr(argv[0], '/');
		if (options.name != NULL) {
			options.name++;
		} else {
			options.name = argv[0];
		}
	} else {
		options.name = NAME;
	}
	while ((ch = getopt(argc, argv, "iN:p:rtwx:X:")) != -1) switch (ch) {
	case 'N':
		options.name = optarg;
		break;
	case 'p':
		parsegeometry(optarg);
		break;
	case 'w':
		options.windowed = true;
		break;
	default:
		usage();
		break;

	/* options below are deprecated and ignored */
	case 'i':
	case 'r':
	case 'x':
	case 'X':
		break;
	}
	argc -= optind;
	argv += optind;
	switch (argc) {
	case 1:
		options.title = argv[0];
		break;
	case 0:
		options.title = CLASS;
		break;
	default:
		usage();
		exit(EXIT_FAILURE);
	}
	return;
}

static Window
createwindow(Widget *widget, XRectangle *geometry, long eventmask, bool override)
{
	return XCreateWindow(
		widget->display,
		widget->rootwin,
		geometry != NULL ? geometry->x : 0,
		geometry != NULL ? geometry->y : 0,
		geometry != NULL ? geometry->width : 1,
		geometry != NULL ? geometry->height : 1,
		widget->borderwid,
		widget->depth,
		InputOutput,
		widget->visual,
		CWBackPixel | CWEventMask | CWColormap |
		CWBorderPixel | CWOverrideRedirect | CWSaveUnder,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.background_pixel = 0,
			.colormap = widget->colormap,
			.event_mask = eventmask,
			.save_under = True,
			.override_redirect = override,
		}
	);
}

static Item *
allocitem(const char *label, const char *output, char *file)
{
	struct Item *item;

	item = emalloc(sizeof *item);
	if (label == NULL) {
		item->label = NULL;
		item->output = NULL;
		item->labellen = 0;
	} else {
		item->label = estrdup(label);
		item->labellen = strlen(item->label);
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
	item->prev = NULL;
	item->next = NULL;
	item->parent = NULL;
	item->children = NULL;
	return item;
}

static void
setcolor(Widget *widget, Picture picture, XRenderColor *chans, const char *colorname)
{
	XColor color;

	if (colorname == NULL)
		return;
	if (!XParseColor(widget->display, widget->colormap, colorname, &color)) {
		warnx("%s: unknown color name", colorname);
		return;
	}
	chans->red   = FLAG(color.flags, DoRed)   ? color.red   : 0x0000;
	chans->green = FLAG(color.flags, DoGreen) ? color.green : 0x0000;
	chans->blue  = FLAG(color.flags, DoBlue)  ? color.blue  : 0x0000;
	chans->alpha = 0xFFFF;
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		picture,
		chans,
		0, 0, 1, 1
	);
}

static void
setfont(Widget *widget, const char *facename, double facesize)
{
	CtrlFontSet *fontset;

	if (facename == NULL)
		facename = "xft:";
	fontset = ctrlfnt_open(
		widget->display,
		widget->screen,
		widget->visual,
		widget->colormap,
		facename,
		facesize
	);
	if (fontset == NULL)
		return;
	widget->fontset = fontset;
	widget->fonth = ctrlfnt_height(fontset);
	widget->itemh = widget->fonth + PADDING * 2;
	widget->itemh = MAX(widget->itemh, MIN_HEIGHT);
}

static void
setopacity(Widget *widget, const char *value)
{
	char *endp;
	double d;
	unsigned short opacity;

	d = strtod(value, &endp);
	if (endp == value || *endp != '\0' || d < 0.0 || d > 1.0) {
		warnx("%s: invalid opacity value", value);
		return;
	}
	opacity = d * 0xFFFF;
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->opacity.pict,
		&(XRenderColor){ .alpha = opacity },
		0, 0, 1, 1
	);
}

static char *
getresource(Widget *widget, XrmDatabase xdb, enum Resource resource)
{
	XrmQuark name[] = {
		widget->application.name,
		widget->resources[resource].name,
		NULLQUARK,
	};
	XrmQuark class[] = {
		widget->application.class,
		widget->resources[resource].class,
		NULLQUARK,
	};
	XrmRepresentation tmp;
	XrmValue xval;

	if (XrmQGetResource(xdb, name, class, &tmp, &xval))
		return xval.addr;
	return NULL;
}

static void
loadresources(Widget *widget, const char *str)
{
	XrmDatabase xdb;
	char *value;
	char *fontname = NULL;
	enum Resource resource;
	double fontsize = 0.0;
	bool changefont = false;

	if (str == NULL)
		return;
	xdb = XrmGetStringDatabase(str);
	if (xdb == NULL)
		return;
	for (resource = 0; resource < NRESOURCES; resource++) {
		value = getresource(widget, xdb, resource);
		if (value == NULL)
			continue;
		switch (resource) {
		case NORMAL_BG:
		case SHADOW_MID:
			setcolor(
				widget,
				widget->colors[SCHEME_NORMAL][COLOR_BG].pict,
				&widget->colors[SCHEME_NORMAL][COLOR_BG].chans,
				value
			);
			break;
		case NORMAL_FG:
			setcolor(
				widget,
				widget->colors[SCHEME_NORMAL][COLOR_FG].pict,
				&widget->colors[SCHEME_NORMAL][COLOR_FG].chans,
				value
			);
			break;
		case _SELECT_BG:
		case SELECT_BG:
			setcolor(
				widget,
				widget->colors[SCHEME_SELECT][COLOR_BG].pict,
				&widget->colors[SCHEME_SELECT][COLOR_BG].chans,
				value
			);
			break;
		case _SELECT_FG:
		case SELECT_FG:
			setcolor(
				widget,
				widget->colors[SCHEME_SELECT][COLOR_FG].pict,
				&widget->colors[SCHEME_SELECT][COLOR_FG].chans,
				value
			);
			break;
		case _BORDER:
		case BORDER_CLR:
			setcolor(
				widget,
				widget->border.pict,
				&widget->border.chans,
				value
			);
			break;
		case _SEPARAT:
		case SEPARAT_CLR:
			setcolor(
				widget,
				widget->colors[SCHEME_SHADOW][COLOR_BG].pict,
				&widget->colors[SCHEME_SHADOW][COLOR_BG].chans,
				value
			);
			setcolor(
				widget,
				widget->colors[SCHEME_SHADOW][COLOR_FG].pict,
				&widget->colors[SCHEME_SHADOW][COLOR_FG].chans,
				value
			);
			break;
		case SHADOW_TOP:
			setcolor(
				widget,
				widget->colors[SCHEME_SHADOW][COLOR_TOP].pict,
				&widget->colors[SCHEME_SHADOW][COLOR_TOP].chans,
				value
			);
			break;
		case SHADOW_BOT:
			setcolor(
				widget,
				widget->colors[SCHEME_SHADOW][COLOR_BOT].pict,
				&widget->colors[SCHEME_SHADOW][COLOR_BOT].chans,
				value
			);
			break;
		case ALIGNMENT:
			if (strcasecmp(value, "center") == 0)
				widget->alignment = ALIGN_CENTER;
			else if (strcasecmp(value, "left") == 0)
				widget->alignment = ALIGN_LEFT;
			else if (strcasecmp(value, "right") == 0)
				widget->alignment = ALIGN_RIGHT;
			break;
		case ICON_SIZE:
			setatoi(&widget->iconsize, value);
			break;
		case BORDER_WID:
			setatoi(&widget->borderwid, value);
			break;
		case FACE_NAME:
			fontname = value;
			changefont = true;
			break;
		case FACE_SIZE:
			setatof(&fontsize, value);
			changefont = true;
			break;
		case GAP_WID:
			setatoi(&widget->gap, value);
			break;
		case MAX_ITEMS:
			setatoi(&widget->maxitems, value);
			break;
		case OPACITY:
			setopacity(widget, value);
			break;
		case SHADOW_WID:
			setatoi(&widget->shadowwid, value);
			break;
		case _TEAROFF:
		case TEAROFF:
			widget->tearoff = (
				strcasecmp(value, "ON") == 0 ||
				strcasecmp(value, "TRUE") == 0 ||
				strcasecmp(value, "1") == 0 ||
				strcasestr(value, "ENABLED") != NULL
			);
		case NRESOURCES:
			break;
		}
	}
	if (changefont)
		setfont(widget, fontname, fontsize);
	XrmDestroyDatabase(xdb);
}

static int
createpicture(Widget *widget, Picture *picture, Pixmap *pixmap,
              XRenderColor *color, bool isalpha)
{
	*pixmap = XCreatePixmap(
		widget->display,
		widget->window,
		1, 1,
		isalpha ? 8 : widget->depth
	);
	if (*pixmap == None) {
		warnx("could not create pixmap");
		return RETURN_FAILURE;
	}
	*picture = XRenderCreatePicture(
		widget->display,
		*pixmap,
		isalpha ? widget->alphaformat : widget->xformat,
		CPRepeat,
		&(XRenderPictureAttributes){
			.repeat = RepeatNormal,
		}
	);
	if (*picture == None) {
		warnx("could not create picture");
		return RETURN_FAILURE;
	}
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		*picture,
		color,
		0, 0, 1, 1
	);
	return RETURN_SUCCESS;
}

static int
initxconn(Widget *widget)
{
	static char *atomnames[NATOMS] = {
#define X(atom) [atom] = #atom,
		ATOMS
#undef  X
	};

	ctrlfnt_init();
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("could not set locale");
	if ((widget->display = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		return RETURN_FAILURE;
	}
	widget->cursor = XCreateFontCursor(widget->display, XC_left_ptr);
	if (widget->cursor == None) {
		warnx("could create font cursor");
		return RETURN_FAILURE;
	}
	widget->fd = XConnectionNumber(widget->display);
	while (fcntl(widget->fd, F_SETFD, FD_CLOEXEC) == RETURN_FAILURE) {
		if (errno == EINTR)
			continue;
		warn("fcntl");
		return RETURN_FAILURE;
	}
	XInternAtoms(widget->display, atomnames, NATOMS, False, widget->atoms);
	widget->screen = DefaultScreen(widget->display);
	widget->rootwin = DefaultRootWindow(widget->display);
	return RETURN_SUCCESS;
}

static int
initvisual(Widget *widget)
{
	XVisualInfo vinfo;
	Colormap cmap = None;
	int success;

	success = XMatchVisualInfo(
		widget->display,
		widget->screen,
		32,
		TrueColor,
		&vinfo
	);
	if (success) {
		cmap = XCreateColormap(
			widget->display,
			widget->rootwin,
			vinfo.visual,
			AllocNone
		);
	}
	widget->colormap = DefaultColormap(widget->display, widget->screen);
	widget->visual = DefaultVisual(widget->display, widget->screen);
	widget->depth = DefaultDepth(widget->display, widget->screen);
	if (success && cmap != None) {
		widget->colormap = cmap;
		widget->visual = vinfo.visual;
		widget->depth = vinfo.depth;
	}
	widget->xformat = XRenderFindVisualFormat(
		widget->display,
		widget->visual
	);
	if (widget->xformat == NULL)
		goto error;
	widget->alphaformat = XRenderFindStandardFormat(
		widget->display,
		PictStandardA8
	);
	if (widget->alphaformat == NULL)
		goto error;
	widget->window = createwindow(widget, NULL, 0, True);
	if (widget->window == None) {
		warnx("could not create window");
		return RETURN_FAILURE;
	}
	return RETURN_SUCCESS;
error:
	warnx("could not find XRender visual format");
	return RETURN_FAILURE;
}

static int
initresources(Widget *widget)
{
	static struct {
		const char *class, *name;
	} resourceids[NRESOURCES] = {
#define X(res, s1, s2) [res] = { .class = s1, .name = s2 },
		RESOURCES
#undef  X
	};
	size_t i;

	XrmInitialize();
	widget->application = (struct ResourcePair){
		.class = XrmPermStringToQuark(options.class),
		.name = XrmPermStringToQuark(options.name),
	};
	for (i = 0; i < NRESOURCES; i++) {
		widget->resources[i] = (struct ResourcePair){
			.class = XrmPermStringToQuark(resourceids[i].class),
			.name = XrmPermStringToQuark(resourceids[i].name),
		};
	};
	return RETURN_SUCCESS;
}

static int
inittheme(Widget *widget)
{
	size_t i, j;
	char *resourcesdb;
	int retval;

	widget->colors[SCHEME_NORMAL][COLOR_BG].chans = DEF_COLOR_BG;
	widget->colors[SCHEME_NORMAL][COLOR_FG].chans = DEF_COLOR_FG;
	widget->colors[SCHEME_SELECT][COLOR_BG].chans = DEF_COLOR_SELBG;
	widget->colors[SCHEME_SELECT][COLOR_FG].chans = DEF_COLOR_SELFG;
	widget->colors[SCHEME_SHADOW][COLOR_TOP].chans = DEF_COLOR_SHDTOP;
	widget->colors[SCHEME_SHADOW][COLOR_BOT].chans = DEF_COLOR_SHDBOT;
	widget->border.chans = DEF_COLOR_BRD;
	widget->borderwid = DEF_BORDER;
	widget->iconsize = DEF_ICONSIZE;
	widget->gap = DEF_GAP;
	widget->alignment = DEF_ALIGNMENT;
	for (i = 0; i < SCHEME_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			retval = createpicture(
				widget,
				&widget->colors[i][j].pict,
				&widget->colors[i][j].pix,
				&widget->colors[i][j].chans,
				false
			);
			if (retval == RETURN_FAILURE) {
				return RETURN_FAILURE;
			}
		}
	}
	createpicture(
		widget,
		&widget->border.pict,
		&widget->border.pix,
		&widget->border.chans,
		false
	);
	createpicture(
		widget,
		&widget->opacity.pict,
		&widget->opacity.pix,
		&(XRenderColor){ .alpha = 0xFFFF },
		true
	);
	resourcesdb = XResourceManagerString(widget->display);
	loadresources(widget, resourcesdb);
	if (widget->fontset == NULL)
		setfont(widget, NULL, 0.0);
	if (widget->fontset == NULL) {
		warnx("could not load any font");
		return RETURN_FAILURE;
	}
	return RETURN_SUCCESS;
}

static Item *
parsestdin(void)
{
	Item *root = NULL;
	Item *prev = NULL;
	Item *item;
	char *s, buf[BUFSIZ];
	char *file, *label, *output;
	size_t level = 0;
	size_t prevlvl = 0;
	size_t i;

	while (fgets(buf, BUFSIZ, stdin) != NULL) {
		/* get the indentation level */
		level = strspn(buf, "\t");

		/* get the label */
		s = level + buf;
		label = strtok(s, "\t\n");
		if (label != NULL && strcmp(label, ":") == 0)
			label = NULL;

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

		item = allocitem(label, output, file);

		if (prev == NULL) {
			root = item;
		} else if (level < prevlvl) {
			for (i = level; i < prevlvl; i++){
				if (prev->parent == NULL)
					break;
				prev = prev->parent;
			}
			item->parent = prev->parent;
			item->prev = prev;
			prev->next = item;
		} else if (level == prevlvl) {
			item->parent = prev->parent;
			item->prev = prev;
			prev->next = item;
		} else if (level > prevlvl) {
			item->parent = prev;
			prev->children = item;
		}
		prev = item;
		prevlvl = level;
	}
	return root;
}

static void
cleanmenu(Item *item, Item *skip)
{
	Item *tmp;

	if (item == NULL)
		return;
	if (item == skip)
		return;
	while (item != NULL) {
		if (item->children != NULL)
			cleanmenu(item->children, skip);
		tmp = item;
		item = item->next;
		if (tmp->label != tmp->output)
			free(tmp->label);
		free(tmp->output);
		free(tmp);
	}
}

static void
cleanup(Widget *widget)
{
	size_t i, j;

	if (widget->fontset != NULL)
		ctrlfnt_free(widget->fontset);
	for (i = 0; i < SCHEME_LAST; i++) for (j = 0; j < COLOR_LAST; j++) {
		if (widget->colors[i][j].pict != None) {
			XRenderFreePicture(
				widget->display,
				widget->colors[i][j].pict
			);
		}
		if (widget->colors[i][j].pix != None) {
			XFreePixmap(
				widget->display,
				widget->colors[i][j].pix
			);
		}
	}
	if (widget->opacity.pict != None) {
		XRenderFreePicture(
			widget->display,
			widget->opacity.pict
		);
	}
	if (widget->opacity.pix != None) {
		XFreePixmap(
			widget->display,
			widget->opacity.pix
		);
	}
	if (widget->cursor != None)
		XFreeCursor(widget->display, widget->cursor);
	if (widget->colormap != None)
		XFreeColormap(widget->display, widget->colormap);
	if (widget->window != None)
		XDestroyWindow(widget->display, widget->window);
	if (widget->display != NULL)
		XCloseDisplay(widget->display);
}

static void
getposition(Widget *widget, XRectangle *geometry)
{
	XineramaScreenInfo *info = NULL;
	Window dw;          /* dummy variable */
	int di;             /* dummy variable */
	unsigned du;        /* dummy variable */
	int nmons;
	int i;
	int x, y;

	if (!options.userplaced) {
		XQueryPointer(
			widget->display,
			widget->rootwin,
			&dw, &dw,
			&x, &y,
			&di, &di,
			&du
		);
		geometry->width = geometry->height = 0;
		geometry->x = x;
		geometry->y = y;
	}
	widget->monitor.x = widget->monitor.y = 0;
	widget->monitor.width = DisplayWidth(widget->display, widget->screen);
	widget->monitor.height = DisplayHeight(widget->display, widget->screen);
	info = XineramaQueryScreens(widget->display, &nmons);
	if (info == NULL)
		return;
	for (i = 0; i < nmons; i++) {
		if (geometry->x < info[i].x_org)
			continue;
		if (geometry->y < info[i].x_org)
			continue;
		if (geometry->x >= info[i].x_org + info[i].width)
			continue;
		if (geometry->y >= info[i].x_org + info[i].width)
			continue;
		widget->monitor.x = info[i].x_org;
		widget->monitor.y = info[i].y_org;
		widget->monitor.width = info[i].width;
		widget->monitor.height = info[i].height;
		break;
	}
	XFree(info);
}

static int
grab(Widget *widget)
{
	int status;

	status = XGrabPointer(
		widget->display,
		widget->rootwin,
		True,
		ButtonPressMask,
		GrabModeAsync,
		GrabModeAsync,
		None,
		widget->cursor,
		CurrentTime
	);
	if (status != GrabSuccess)
		return RETURN_FAILURE;
	status = XGrabKeyboard(
		widget->display,
		widget->rootwin,
		True,
		GrabModeAsync,
		GrabModeAsync,
		CurrentTime
	);
	if (status != GrabSuccess)
		return RETURN_FAILURE;
	return RETURN_SUCCESS;
}

static void
drawshadows(Widget *widget, Picture picture, XRectangle *geometry)
{
	int i;

	for(i = 0; i < widget->shadowwid; i++) {
		/* draw light shadow */
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[SCHEME_SHADOW][COLOR_TOP].chans,
			i, i,
			1, geometry->height - (i * 2 + 1)
		);
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[SCHEME_SHADOW][COLOR_TOP].chans,
			i, i,
			geometry->width - (i * 2 + 1), 1
		);

		/* draw dark shadow */
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[SCHEME_SHADOW][COLOR_BOT].chans,
			geometry->width - 1 - i, i,
			1, geometry->height - i * 2
		);
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[SCHEME_SHADOW][COLOR_BOT].chans,
			i, geometry->height - 1 - i,
			geometry->width - i * 2, 1
		);
	}
}

static void
drawdashline(Widget *widget, Picture picture, int width, int y)
{
	XRectangle toprects[32];
	XRectangle botrects[32];
	size_t i, nrects;
	int x;
	int w;
	int maxw;

	x = widget->shadowwid + PADDING;
	y += widget->itemh / 2;
	w = 0;
	maxw = width - widget->shadowwid * 2 - PADDING * 2;
	while (w < maxw) {
		nrects = 0;
		for (i = 0; i < LEN(toprects); i++) {
			if (w >= maxw)
				break;
			toprects[i].x = x + w;
			toprects[i].y = y - 1;
			toprects[i].width = DASH_SIZE;
			toprects[i].height = 1;
			botrects[i].x = x + w;
			botrects[i].y = y;
			botrects[i].width = DASH_SIZE;
			botrects[i].height = 1;
			w += DASH_SIZE * 2;
			nrects++;
		}
		for (i = 0; i < nrects; i++) {
			XRenderFillRectangles(
				widget->display,
				PictOpSrc,
				picture,
				&widget->colors[SCHEME_SHADOW][COLOR_TOP].chans,
				toprects,
				nrects
			);
			XRenderFillRectangles(
				widget->display,
				PictOpSrc,
				picture,
				&widget->colors[SCHEME_SHADOW][COLOR_BOT].chans,
				botrects,
				nrects
			);
		}
	}
}

static void
drawseparator(Widget *widget, Picture picture, XRectangle *rect)
{
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		picture,
		&widget->colors[SCHEME_SHADOW][COLOR_BOT].chans,
		widget->shadowwid + PADDING,
		rect->y + widget->itemh / 2 - 1,
		rect->width - widget->shadowwid * 2 - PADDING * 2,
		1
	);
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		picture,
		&widget->colors[SCHEME_SHADOW][COLOR_TOP].chans,
		widget->shadowwid + PADDING,
		rect->y + widget->itemh / 2,
		rect->width - widget->shadowwid * 2 - PADDING * 2,
		1
	);
}

static void
drawtriangle(Widget *widget, Picture picture, Picture src, int x, int y, int direction)
{
	XTriangle triangle;

	if (direction == DIR_UP) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_WIDTH)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT / 2),
			XDoubleToFixed(y)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT),
			XDoubleToFixed(y + TRIANGLE_WIDTH)
		};
	} else if (direction == DIR_DOWN) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT / 2),
			XDoubleToFixed(y + TRIANGLE_WIDTH)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT),
			XDoubleToFixed(y)
		};
	} else if (direction == DIR_LEFT) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_WIDTH),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x - TRIANGLE_WIDTH),
			XDoubleToFixed(y + TRIANGLE_HEIGHT / 2)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_HEIGHT)
		};
	} else {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_WIDTH),
			XDoubleToFixed(y + TRIANGLE_HEIGHT / 2)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_HEIGHT)
		};
	}
	XRenderCompositeTriangles(
		widget->display,
		PictOpOver,
		src,
		picture,
		widget->xformat,
		0, 0,
		&triangle,
		1
	);
}

static bool
cantearoff(Widget *widget, Menu *menu)
{
	return (menu->next != NULL || !options.windowed) && widget->tearoff;
}

static int
firstitempos(Widget *widget, Menu *menu)
{
	int y;

	y = widget->shadowwid;
	if (menu->overflow)
		y += widget->itemh;
	if (cantearoff(widget, menu))
		y += widget->itemh;
	return y;
}

static void
initimlib(Widget *widget)
{
	if (widget->initimlib)
		return;
	widget->initimlib = true;
	imlib_set_cache_size(2048 * 1024);
	imlib_context_set_dither(1);
	imlib_context_set_blend(0);
	imlib_context_set_display(widget->display);
	imlib_context_set_visual(widget->visual);
	imlib_context_set_colormap(widget->colormap);
}

static bool
isabsolute(const char *s)
{
	if (s[0] == '/')
		return true;
	if (s[0] == '.' && (s[1] == '/' || (s[1] == '.' && s[2] == '/')))
		return true;
	return false;
}

static Imlib_Image
loadicon(Widget *widget, const char *file, int size, int *width_ret, int *height_ret)
{
	Imlib_Image icon = NULL;
	Imlib_Load_Error errcode;
	char path[PATH_MAX];
	const char *errstr;
	int width;
	int height;
	size_t i;

	if (file == NULL)
		return NULL;
	if (*file == '\0') {
		warnx("could not load icon (file name is blank)");
		return NULL;
	}
	initimlib(widget);
	if (isabsolute(file))
		icon = imlib_load_image_with_error_return(file, &errcode);
	else {
		for (i = 0; i < options.niconpaths; i++) {
			snprintf(path, sizeof(path), "%s/%s", options.iconpaths[i], file);
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

	if (width > height) {
		*width_ret = size;
		*height_ret = (height * size) / width;
	} else {
		*width_ret = (width * size) / height;
		*height_ret = size;
	}

	icon = imlib_create_cropped_scaled_image(
		0, 0,
		width, height,
		*width_ret, *height_ret
	);
	return icon;
}

static void
drawmenu(Widget *widget)
{
	Menu *menu;
	Item *item;
	size_t i, j;
	Imlib_Image image;
	XRectangle rect;
	int iconw, iconh, y;
	struct Canvas canvas[CANVAS_FINAL][LAYER_LAST];

	if ((menu = widget->menus) == NULL)
		return;
	for (i = 0; i < CANVAS_LAST; i++) {
		if (menu->canvas[i].picture != None) {
			XRenderFreePicture(
				widget->display,
				menu->canvas[i].picture
			);
		}
		if (menu->canvas[i].pixmap != None)
			XFreePixmap(widget->display, menu->canvas[i].pixmap);
		menu->canvas[i].pixmap = XCreatePixmap(
			widget->display,
			menu->window,
			menu->geometry.width,
			menu->geometry.height,
			widget->depth
		);
		menu->canvas[i].picture = XRenderCreatePicture(
			widget->display,
			menu->canvas[i].pixmap,
			widget->xformat,
			0,
			NULL
		);
		XRenderFillRectangle(
			widget->display,
			PictOpClear,
			menu->canvas[i].picture,
			&(XRenderColor){ 0 },
			0, 0,
			menu->geometry.width,
			menu->geometry.height
		);
	}
	for (i = 0; i < CANVAS_FINAL; i++) for (j = 0; j < LAYER_LAST; j++) {
		canvas[i][j].pixmap = XCreatePixmap(
			widget->display,
			menu->window,
			menu->geometry.width,
			menu->geometry.height,
			widget->depth
		);
		canvas[i][j].picture = XRenderCreatePicture(
			widget->display,
			canvas[i][j].pixmap,
			widget->xformat,
			0,
			NULL
		);
		XRenderFillRectangle(
			widget->display,
			PictOpClear,
			canvas[i][j].picture,
			&(XRenderColor){ 0 },
			0, 0,
			menu->geometry.width,
			menu->geometry.height
		);
	}
	for (i = 0; i < CANVAS_FINAL; i++) {
		drawshadows(widget, canvas[i][LAYER_BG].picture, &menu->geometry);
		XRenderComposite(
			widget->display,
			PictOpSrc,
			widget->colors[i][COLOR_BG].pict,
			widget->opacity.pict,
			canvas[i][LAYER_BG].picture,
			0, 0,
			0, 0,
			widget->shadowwid, widget->shadowwid,
			menu->geometry.width - widget->shadowwid * 2,
			menu->geometry.height - widget->shadowwid * 2
		);
	}
	rect.x = widget->shadowwid + PADDING;
	if (menu->hasicon)
		rect.x += widget->iconsize + PADDING;
	rect.y = firstitempos(widget, menu);
	rect.width = menu->geometry.width;
	for (item = menu->first; item != NULL; item = item->next) {
		if (item->label == NULL) {
			drawseparator(
				widget,
				canvas[CANVAS_NORMAL][LAYER_FG].picture,
				&rect
			);
			drawseparator(
				widget,
				canvas[CANVAS_SELECT][LAYER_FG].picture,
				&rect
			);
			goto next;
		}
		rect.height = widget->itemh;
		image = loadicon(
			widget,
			item->file,
			widget->iconsize,
			&iconw,
			&iconh
		);
		if (image != NULL) {
			imlib_context_set_image(image);
			imlib_context_set_drawable(
				canvas[CANVAS_NORMAL][LAYER_FG].pixmap
			);
			imlib_render_image_on_drawable(
				widget->shadowwid + PADDING
				+ (widget->iconsize - iconw) / 2,
				rect.y + (widget->itemh - iconh) / 2
			);
			imlib_context_set_drawable(
				canvas[CANVAS_SELECT][LAYER_FG].pixmap
			);
			imlib_render_image_on_drawable(
				widget->shadowwid + PADDING
				+ (widget->iconsize - iconw) / 2,
				rect.y + (widget->itemh - iconh) / 2
			);
			imlib_free_image();
		}
		if (item->children != NULL) {
			drawtriangle(
				widget,
				canvas[CANVAS_NORMAL][LAYER_FG].picture,
				widget->colors[SCHEME_NORMAL][COLOR_FG].pict,
				rect.width - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD/2,
				rect.y + widget->itemh/2 - TRIANGLE_HEIGHT/2,
				DIR_RIGHT
			);
			drawtriangle(
				widget,
				canvas[CANVAS_SELECT][LAYER_FG].picture,
				widget->colors[SCHEME_SELECT][COLOR_FG].pict,
				rect.width - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD/2,
				rect.y + widget->itemh/2 - TRIANGLE_HEIGHT/2,
				DIR_RIGHT
			);
		}
		ctrlfnt_draw(
			widget->fontset,
			canvas[CANVAS_NORMAL][LAYER_FG].picture,
			widget->colors[SCHEME_NORMAL][COLOR_FG].pict,
			rect,
			item->label,
			item->labellen
		);
		ctrlfnt_draw(
			widget->fontset,
			canvas[CANVAS_SELECT][LAYER_FG].picture,
			widget->colors[SCHEME_SELECT][COLOR_FG].pict,
			rect,
			item->label,
			item->labellen
		);
next:
		rect.y += rect.height;
		if (menu->overflow &&
		    rect.y + widget->itemh * 2 >=
		    menu->geometry.height) {
			break;
		}
	}
	for (i = 0; i < CANVAS_FINAL; i++) {
		y = widget->shadowwid;
		if (menu->overflow) {
			drawtriangle(
				widget,
				canvas[i][LAYER_FG].picture,
				widget->colors[i][COLOR_FG].pict,
				menu->geometry.width / 2 - TRIANGLE_HEIGHT / 2,
				y + widget->itemh /2 - TRIANGLE_WIDTH / 2,
				DIR_UP
			);
			drawtriangle(
				widget,
				canvas[i][LAYER_FG].picture,
				widget->colors[i][COLOR_FG].pict,
				menu->geometry.width / 2 - TRIANGLE_HEIGHT / 2,
				menu->geometry.height - widget->itemh /2
				- TRIANGLE_WIDTH / 2,
				DIR_DOWN
			);
			y += widget->itemh;
		}
		if (cantearoff(widget, menu)) {
			drawdashline(
				widget,
				canvas[CANVAS_NORMAL][LAYER_FG].picture,
				menu->geometry.width, y
			);
			drawdashline(
				widget,
				canvas[CANVAS_SELECT][LAYER_FG].picture,
				menu->geometry.width, y
			);
		}
		XRenderComposite(
			widget->display,
			PictOpOver,
			canvas[i][LAYER_FG].picture,
			None,
			canvas[i][LAYER_BG].picture,
			0, 0,
			0, 0,
			0, 0,
			menu->geometry.width,
			menu->geometry.height
		);
		XRenderComposite(
			widget->display,
			PictOpOver,
			canvas[i][LAYER_BG].picture,
			None,
			menu->canvas[i].picture,
			0, 0,
			0, 0,
			0, 0,
			menu->geometry.width,
			menu->geometry.height
		);
	}
	XSetWindowBackgroundPixmap(
		widget->display,
		menu->window,
		menu->canvas[LAYER_BG].pixmap
	);
	XSetWindowBorderPixmap(widget->display, menu->window, widget->border.pix);
	XClearWindow(widget->display, menu->window);
	for (i = 0; i < CANVAS_FINAL; i++) for (j = 0; j < LAYER_LAST; j++) {
		XRenderFreePicture(widget->display, canvas[i][j].picture);
		XFreePixmap(widget->display, canvas[i][j].pixmap);
	}
}

static void
drawselection(Widget *widget, Menu *menu, int ypos)
{
	menu->selposition = ypos;
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		menu->canvas[CANVAS_FINAL].picture,
		&(XRenderColor){ 0 },
		0, 0,
		menu->geometry.width,
		menu->geometry.height
	);
	XRenderComposite(
		widget->display,
		PictOpOver,
		menu->canvas[CANVAS_NORMAL].picture,
		None,
		menu->canvas[CANVAS_FINAL].picture,
		0, 0,
		0, 0,
		0, 0,
		menu->geometry.width,
		menu->geometry.height
	);
	XRenderComposite(
		widget->display,
		PictOpOver,
		menu->canvas[CANVAS_SELECT].picture,
		None,
		menu->canvas[CANVAS_FINAL].picture,
		0, ypos,
		0, 0,
		0, ypos,
		menu->geometry.width,
		widget->itemh
	);
	XSetWindowBackgroundPixmap(
		widget->display,
		menu->window,
		menu->canvas[CANVAS_FINAL].pixmap
	);
	XClearWindow(widget->display, menu->window);
}

static void
popupmenu(Widget *widget, XRectangle *basis)
{
	XRectangle *monitor = &widget->monitor;
	Menu *menu;
	Item *caller;
	Atom type;
	Item *items, *item;
	size_t nitems;
	unsigned int textw, menuh;
	int xgap, ygap;
	char *name;
	bool override_redirect, tearoff;

	type = widget->atoms[_NET_WM_WINDOW_TYPE_POPUP_MENU];
	override_redirect = true;
	if (widget->menus != NULL) {
		if (widget->menus->selected == NULL)
			return;         /* no item selected */
		if (widget->menus->selected->children == NULL)
			return;         /* item creates no submenu */
		caller = widget->menus->selected;
		name = caller->output;
		items = caller->children;
		xgap = widget->gap;
		ygap = -widget->shadowwid;
		tearoff = widget->tearoff;
		if (tearoff)
			ygap -= widget->itemh;
		(void)grab(widget);
	} else {
		caller = NULL;
		name = options.title;
		items = options.items;
		xgap = ygap = INITIAL_DISPLACEMENT;
		tearoff = false;
		if (options.windowed) {
			override_redirect = false;
			type = widget->atoms[_NET_WM_WINDOW_TYPE_MENU];
		}
	}
	menu = emalloc(sizeof(*menu));
	*menu = (Menu){
		.items = items,
		.first = items,
		.last = NULL,
		.selected = NULL,
		.caller = caller,
	};
	menu->next = widget->menus;
	widget->menus = menu;
	menuh = widget->shadowwid * 2;
	if (tearoff)
		menuh += widget->itemh;
	menu->geometry.height = widget->shadowwid * 2;
	nitems = 0;
	menu->nicons = 0;
	for (item = menu->items; item != NULL; item = item->next) {
		nitems++;
		if (item->label != NULL) {
			menuh += widget->itemh;
			textw = ctrlfnt_width(
				widget->fontset,
				item->label,
				item->labellen
			);
		} else {
			textw = 0;
			menuh += widget->itemh;
		}
		if (item->file != NULL) {
			menu->hasicon = true;
			menu->nicons++;
		}
		if (item->children != NULL)
			menu->hassubmenu = true;
		menu->geometry.width = MAX(menu->geometry.width, textw);
		if (menu->overflow)
			continue;
		menu->lastsave = menu->last = item->prev;
		if (widget->maxitems > 0 && nitems > (unsigned)widget->maxitems) {
			menu->lastsave = menu->last = item->prev;
			menu->overflow = true;
		}
		if (menuh + widget->itemh * 2 <= monitor->height) {
			menu->geometry.height = menuh;
		} else {
			menu->lastsave = menu->last = item->prev;
			menu->overflow = true;
			menu->geometry.height = monitor->height;
		}
	}

	menu->geometry.width += PADDING * 2 + widget->shadowwid * 2;
	if (menu->hasicon)
		menu->geometry.width += widget->iconsize + PADDING;
	if (menu->hassubmenu)
		menu->geometry.width += TRIANGLE_WIDTH + TRIANGLE_PAD + PADDING;
	menu->geometry.width = MIN(menu->geometry.width, monitor->width/2);

	/* place menu to align with parent menu and be visible on monitor */
	if (menu->overflow && widget->menus != NULL)
		ygap -= widget->itemh;
	menu->geometry.x = monitor->x;
	menu->geometry.y = monitor->y;
	if (monitor->x + monitor->width - (basis->x + basis->width + xgap)
	    >= menu->geometry.width) {
		menu->geometry.x = basis->x + basis->width + xgap;
	} else if (basis->x > (long)menu->geometry.width + xgap) {
		menu->geometry.x = basis->x - menu->geometry.width - xgap;
	}
	if (monitor->y + monitor->height - (basis->y + ygap)
	    >= menu->geometry.height) {
		menu->geometry.y = basis->y + ygap;
	} else if (monitor->y + monitor->height > menu->geometry.height + ygap) {
		menu->geometry.y = monitor->y + monitor->height;
		menu->geometry.y -= menu->geometry.height + ygap;
	}

	menu->window = createwindow(
		widget, &menu->geometry,
		KeyPressMask | StructureNotifyMask | LeaveWindowMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		override_redirect
	);
	(void)XSetWMProtocols(
		widget->display,
		menu->window,
		&widget->atoms[WM_DELETE_WINDOW],
		1
	);
	(void)XmbSetWMProperties(
		widget->display,
		menu->window,
		name, name,
		options.argv,
		options.argc,
		&(XSizeHints){ .flags = USPosition|USSize },
		NULL,
		&(XClassHint){
			.res_class = options.class,
			.res_name = options.name,
		}
	);
	(void)XChangeProperty(
		widget->display,
		menu->window,
		widget->atoms[_NET_WM_NAME],
		widget->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)name,
		strlen(name)
	);
	(void)XChangeProperty(
		widget->display,
		menu->window,
		widget->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM,
		32,
		PropModeReplace,
		(unsigned char *)&type,
		1
	);

	drawmenu(widget);
	XMapRaised(widget->display, menu->window);
}

static void
delmenu(Widget *widget)
{
	Menu *menu;
	size_t i;

	if ((menu = widget->menus) == NULL)
		return;
	widget->menus = menu->next;
	XDestroyWindow(widget->display, menu->window);
	for (i = 0; i < CANVAS_LAST; i++) {
		XRenderFreePicture(widget->display, menu->canvas[i].picture);
		XFreePixmap(widget->display, menu->canvas[i].pixmap);
	}
}

static void
ungrab(Widget *widget)
{
	XUngrabPointer(widget->display, CurrentTime);
	XUngrabKeyboard(widget->display, CurrentTime);
}

static void
closewidget(Widget *widget)
{
	while (widget->menus != NULL) {
		if (options.windowed && widget->menus->next == NULL) {
			ungrab(widget);
			break;
		}
		delmenu(widget);
	}
}

static Item *
getitem(Widget *widget, Menu *menu, long y, int *ypos)
{
	long h;
	Item *item;

	h = widget->shadowwid;
	if (menu->overflow && y >= h && y < h + widget->itemh) {
		item = &scrollup;
		goto done;
	}
	if (menu->overflow && y >= menu->geometry.height - widget->itemh &&
	    y < menu->geometry.height) {
		h = menu->geometry.height - widget->itemh;
		item = &scrolldown;
		goto done;
	}
	if (menu->overflow)
		h += widget->itemh;
	if (cantearoff(widget, menu)) {
		if (y < h + widget->itemh) {
			item = &tearoff;
			goto done;
		}
		h += widget->itemh;
	}
	for (item = menu->first; item != NULL; item = item->next) {
		if (item->label == NULL) {
			h += widget->itemh;
			continue;
		}
		if (y >= h && y < (long)h + widget->itemh)
			break;
		h += widget->itemh;
		if (menu->overflow &&
		    h + widget->itemh + widget->itemh + widget->shadowwid >=
		    menu->geometry.height) {
			return NULL;
		}
	}
done:
	if (ypos != NULL)
		*ypos = h;
	return item;
}

static void
scroll(Widget *widget, bool down)
{
	XRectangle rect;
	Menu *menu;
	time_t elapsed;
	struct timespec ts;
	int ret;
	struct pollfd pfd = {
		.fd = widget->fd,
		.events = POLLIN,
	};

	if ((menu = widget->menus) == NULL)
		return;
	if (down)
		rect.y = menu->geometry.height - widget->itemh - widget->shadowwid;
	else
		rect.y = widget->shadowwid;
	rect.x = 0;
	rect.width = menu->geometry.width;
	rect.height = widget->itemh;
	for (;;) {
		if (XPending(widget->display) > 0)
			break;
		switch ((ret = poll(&pfd, 1, SCROLL_TIME))) {
		case -1:
			if (errno == EINTR)
				continue;
			return;
		default:
			egettime(&ts);
			elapsed = (ts.tv_sec - widget->lasttime.tv_sec) * 1000;
			elapsed += ts.tv_nsec / 1000000;
			elapsed -= widget->lasttime.tv_nsec / 1000000;
			if (elapsed < SCROLL_TIME)
				return;
			/* fallthrough */
		case 0:
			if (down) {
				if (menu->last->next == NULL)
					break;
				menu->first = menu->first->next;
				menu->last = menu->last->next;
			} else {
				if (menu->first->prev == NULL)
					break;
				menu->first = menu->first->prev;
				menu->last = menu->last->prev;
			}
			drawmenu(widget);
			drawselection(widget, menu, rect.y);
			XFlush(widget->display);
			if (ret == 0)
				break;
			egettime(&widget->lasttime);
			break;
		}
	}
}

static void
forkandtearoff(Widget *widget, Menu *menu)
{
	if (efork() == 0) {
		/* child */
		ctrlfnt__free(widget->fontset);
		while (close(widget->fd) == -1) {
			if (errno == EINTR)
				continue;
			err(EXIT_FAILURE, "close");
		}
		*widget = (Widget){ 0 };
		cleanmenu(options.items, menu->first);
		options.items = menu->first;
		options.userplaced = true;
		options.windowed = true;
		options.geometry.x = menu->geometry.x;
		options.geometry.y = menu->geometry.y;
		options.geometry.width = 0;
		options.geometry.height = 0;
		longjmp(jmpenv, 1);
		exit(EXIT_FAILURE);
	}
	closewidget(widget);
}

static void
openitem(Widget *widget, Item *item, int ypos)
{
	XRectangle rect;
	Menu *menu;

	if (item == NULL || item == &tearoff)
		return;
	menu = widget->menus;
	menu->selected = item;
	rect = menu->geometry;
	rect.y += ypos;
	if (item == &scrollup || item == &scrolldown)
		return;
	if (item->children != NULL) {
		popupmenu(widget, &rect);
	} else if (item->output != NULL) {
		(void)printf("%s\n", item->output);
		(void)fflush(stdout);
		closewidget(widget);
	}
}

static Menu *
getmenu(Widget *widget, Window window)
{
	Menu *menu;

	for (menu = widget->menus; menu != NULL; menu = menu->next)
		if (window == menu->window)
			return menu;
	return NULL;
}

static void
xbuttonrelease(Widget *widget, XEvent *xev)
{
	XButtonEvent *xevent;
	Item *item;
	Menu *menu;
	int ypos;

	xevent = (XButtonEvent *)xev;
	if (xevent->button != Button1 && xevent->button != Button3)
		return;
	menu = getmenu(widget, xevent->window);
	if (menu == NULL)
		return;
	item = getitem(widget, menu, xevent->y, &ypos);
	if (item == NULL)
		return;
	if (item->children != NULL)
		return;
	while (widget->menus != menu)
		delmenu(widget);
	if (item == &tearoff) {
		forkandtearoff(widget, menu);
	} else {
		openitem(widget, item, ypos);
	}
}

static void
xbuttonpress(Widget *widget, XEvent *xev)
{
	XButtonEvent *xevent;
	Menu *menu;
	Item *item;
	int ypos;

	xevent = (XButtonEvent *)xev;
	menu = getmenu(widget, xevent->window);
	if (menu == NULL) {
		closewidget(widget);
		return;
	} else if ((item = getitem(widget, menu, xevent->y, &ypos)) != NULL) {
		while (widget->menus != menu)
			delmenu(widget);
		if (item->children != NULL) {
			openitem(widget, item, ypos);
		}
	}
}

static void
xconfigurenotify(Widget *widget, XEvent *xev)
{
	XConfigureEvent *xevent;
	Menu *menu;
	int width, height;

	xevent = (XConfigureEvent *)xev;
	for (menu = widget->menus; menu != NULL; menu = menu->next)
		if (menu->window == xevent->window)
			break;
	if (menu == NULL)
		return;
	width = menu->geometry.width;
	height = menu->geometry.height;
	menu->geometry.x = xevent->x;
	menu->geometry.y = xevent->y;
	menu->geometry.width = xevent->width;
	menu->geometry.height = xevent->height;
	if (width == menu->geometry.width && height == menu->geometry.height)
		return;
	drawmenu(widget);
	if (menu->selected != NULL) {
		drawselection(widget, menu, menu->selposition);
	}
}

static void
xclientmessage(Widget *widget, XEvent *xev)
{
	XClientMessageEvent *xevent;

	xevent = (XClientMessageEvent *)xev;
	if ((Atom)(xevent->data.l[0]) != widget->atoms[WM_DELETE_WINDOW])
		return;
	closewidget(widget);
	delmenu(widget);
}

static void
xkeypress(Widget *widget, XEvent *xev)
{
	XKeyEvent *xevent;
	KeySym ksym;
	Menu *menu;
	Item *item, *last, *first, *start, *prev;
	int ypos, prevpos;
	bool secondtry = false;

	if ((menu = widget->menus) == NULL)
		return;
	xevent = (XKeyEvent *)xev;
	ksym = XkbKeycodeToKeysym(widget->display, xevent->keycode, 0, 0);
	if (ksym == XK_Tab && FLAG(xevent->state, ShiftMask))
		ksym = XK_ISO_Left_Tab;
	switch (ksym) {
	case XK_KP_Enter:       ksym = XK_Return;       break;
	case XK_KP_Home:        ksym = XK_Home;         break;
	case XK_KP_End:         ksym = XK_End;          break;
	case XK_KP_Left:        ksym = XK_Left;         break;
	case XK_KP_Right:       ksym = XK_Right;        break;
	case XK_KP_Up:          ksym = XK_Up;           break;
	case XK_KP_Down:        ksym = XK_Down;         break;
	}
	switch (ksym) {
	case XK_Down:
	case XK_Tab:
		first = menu->first;
		last = menu->last;
		start = menu->selected;
		ypos = menu->selposition;
gohome:
		for (item = start; item != NULL; item = item->next) {
			if (item->label != NULL && item != menu->selected) {
				menu->selected = item;
				if (menu->first != first) {
					menu->first = first;
					menu->last = last;
					drawmenu(widget);
				}
				drawselection(widget, menu, ypos);
				return;
			}
			if (menu->overflow && item == last) {
				first = first->next;
				last = last->next;
			} else {
				ypos += widget->itemh;
			}
		}
		/* fallthrough */
	case XK_Home:
		if (secondtry)
			break;
		menu->selected = NULL;
		first = menu->items;
		last = menu->lastsave;
		secondtry = true;
		start = menu->items;
		ypos = firstitempos(widget, menu);
		goto gohome;
	case XK_ISO_Left_Tab:
	case XK_Up:
		first = menu->first;
		last = menu->last;
		ypos = menu->selposition;
		for (item = menu->selected; item != NULL; item = item->prev) {
			if (item->label != NULL && item != menu->selected) {
				menu->selected = item;
				if (menu->first != first) {
					menu->first = first;
					menu->last = last;
					drawmenu(widget);
				}
				drawselection(widget, menu, ypos);
				return;
			}
			if (menu->overflow && item == first) {
				first = first->prev;
				last = last->prev;
				ypos = firstitempos(widget, menu);
			} else {
				ypos -= widget->itemh;
			}
		}
		/* fallthrough */
	case XK_End:
		first = menu->items;
		last = menu->lastsave;
		prevpos = ypos = firstitempos(widget, menu);
		prev = NULL;
		for (item = menu->items; item != NULL; item = item->next) {
			if (item->label != NULL && item != menu->selected) {
				prev = item;
				prevpos = ypos;
				menu->first = first;
				menu->last = last;
			}
			if (last->next == NULL)
				break;
			if (menu->overflow && item == last) {
				first = first->next;
				last = last->next;
			} else {
				ypos += widget->itemh;
			}
		}
		if (prev != NULL) {
			menu->selected = prev;
			drawmenu(widget);
			drawselection(widget, menu, prevpos);
		}
		break;
	case XK_Left:
		if (menu->next == NULL)
			break;
		/* fallthrough */
	case XK_Escape:
		delmenu(widget);
		break;
	case XK_Right:
	case XK_Return:
		if (menu->selected == NULL)
			break;
		openitem(widget, menu->selected, menu->selposition);
		break;
	}
}

static void
xmotion(Widget *widget, XEvent *xev)
{
	XMotionEvent *xevent;
	Menu *menu;
	Item *item;
	int ypos;

	xevent = (XMotionEvent *)xev;
	menu = getmenu(widget, xevent->window);
	if (menu == NULL)
		return;
	item = getitem(widget, menu, xevent->y, &ypos);
	if (item == &scrollup || item == &scrolldown) {
		scroll(widget, item == &scrolldown);
	}
	if (item == NULL || item == menu->selected)
		return;
	menu->selected = item;
	drawselection(widget, menu, ypos);
	if (xevent->state & (Button1Mask|Button3Mask)) {
		while (widget->menus != menu)
			delmenu(widget);
		if (item->children != NULL) {
			openitem(widget, item, ypos);
		}
	}
}

static void
run(Widget *widget, XRectangle *geometry)
{
	XEvent xev;
	static void (*xevents[LASTEvent])(Widget *, XEvent *) = {
		[ButtonPress]           = xbuttonpress,
		[ButtonRelease]         = xbuttonrelease,
		[ConfigureNotify]       = xconfigurenotify,
		[ClientMessage]         = xclientmessage,
		[KeyPress]              = xkeypress,
		[MotionNotify]          = xmotion,
	};

	getposition(widget, geometry);
	if (!options.windowed)
		if (grab(widget) == RETURN_FAILURE)
			return;
	popupmenu(widget, geometry);
	while (!XNextEvent(widget->display, &xev)) {
		if (xev.type < LASTEvent && xevents[xev.type] != NULL) {
			(*xevents[xev.type])(widget, &xev);
		}
		if (widget->menus == NULL) {
			break;
		}
	}
}

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	Widget widget = { 0 };
	XRectangle geometry = { 0 };
	int (*initsteps[])(Widget *) = {
		initxconn,
		initvisual,
		initresources,
		inittheme,
	};
	size_t i;

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		err(EXIT_FAILURE, "sigaction");
	parseiconpaths(getenv("ICONPATH"));
	parseoptions(argc, argv);
	if ((options.items = parsestdin()) == NULL) {
		warnx("no menu generated");
		goto error;
	}
	(void)setjmp(jmpenv);
	if (options.userplaced)
		geometry = options.geometry;
	for (i = 0; i < LEN(initsteps); i++)
		if ((*initsteps[i])(&widget) == RETURN_FAILURE)
			goto error;
	run(&widget, &geometry);
error:
	cleanup(&widget);
	free(options.iconstring);
	cleanmenu(options.items, NULL);
	return EXIT_SUCCESS;
}
