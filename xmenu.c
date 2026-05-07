#include <sys/types.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
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
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <Imlib2.h>

#include "control/font.h"
#include "control/resources.h"

#include "color.h"

#define LEN(a)                  (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b)               ((a)>(b)?(a):(b))
#define MIN(a, b)               ((a)<(b)?(a):(b))
#define FLAG(f, b)              (((f) & (b)) == (b))
#define RETURN_FAILURE          (-1)
#define RETURN_SUCCESS          0
#define MAXPATHS                32
#define PADDING                 4
#define INITIAL_DISPLACEMENT    2
#define MIN_HEIGHT              (16 + PADDING * 2)
#define TRIANGLE_HEIGHT         8
#define TRIANGLE_WIDTH          3
#define TRIANGLE_PAD            8

enum {
	SEL_FIRST,
	SEL_PREV,
	SEL_NEXT,
	SEL_LAST,
};

enum {
	COLOR_BG,
	COLOR_FG,
	COLOR_TOP,
	COLOR_BOT,
	COLOR_SELBG,
	COLOR_SELFG,
	COLOR_LAST,
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

struct item {
	char *label;            /* string to be drawed on menu */
	char *output;           /* string to be outputed when item is clicked */
	char *altoutput;        /* string to be outputed when item is clicked with alt button */
	char *file;             /* filename of the icon */
	size_t labellen;
	struct item *prev;
	struct item *next;
	struct item *parent;
	struct item *children;
};

struct menu {
	struct menu *next;
	struct item *items, *selected;
	struct item *first, *last, *lastsave;
	char *title;
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
	bool directory;
};

struct Options {
	struct item *items;
	bool windowed;
	bool rootmode;
	bool userplaced;
	bool xneg, yneg, xpercent, ypercent;
	bool monplaced;
	bool filebrowse;
	bool freetitle;
	bool use_monitor;
	int monitor;
	int argc;
	char **argv;
	char *name;
	char *class;
	char *title;

	unsigned int button;
	unsigned int modifier;
	Window client;

	char *iconstring;
	char *iconpaths[MAXPATHS];
	size_t niconpaths;

	XRectangle geometry;
} options = {
	.name = "xmenu",
	.class = "XMenu",
};

struct widget {
	Display *display;
	Visual *visual;
	Colormap colormap;
	int depth;
	int screen;
	Window rootwin;
	Window window;
	struct timespec lasttime;
	XRectangle monitor;
	ctrlfnt *fontset;
	struct menu *menus;
	unsigned int fonth;
	unsigned int itemh, separatorh;
	int shadowwid, iconsize, borderwid, gap;
	int maxitems;
	bool initimlib;
	bool tearoff;
	enum alignment {
		ALIGN_LEFT,
		ALIGN_CENTER,
		ALIGN_RIGHT,
	} alignment;

	struct color {
		XRenderColor chans;
		Pixmap pix;
		Picture pict;
	} opacity, border, colors[COLOR_LAST];
};

static jmp_buf jmpenv;
static struct item tearoff = { .label = "tearoff" };
static struct item scrollup = { .label = "scrollup" };
static struct item scrolldown = { .label = "scrolldown" };

static void
usage(void)
{
	(void)fprintf(
		stderr,
		"usage: xmenu [-fw] [-N name] "
		"[-p position] [-t window] [-x button]\n"
	);
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
parsegeometry(char *geomspec)
{
	char *str;

	str = geomspec;
	if (str == NULL || *str == '\0') return;
	if (*str == '=') str++;

	if (*str >= '0' && *str <= '9') {
		options.geometry.width = strtoul(str, &str, 10);
		if (*str != 'X' && *str != 'x')
			goto error;
		if (options.geometry.width == 0 || options.geometry.width > USHRT_MAX)
			goto error;
		str++;
		if (*str < '0' || *str > '9')
			goto error;
		options.geometry.height = strtoul(str, &str, 10);
		if (options.geometry.height == 0 || options.geometry.height > USHRT_MAX)
			goto error;
	}
	if (*str == '-' || *str == '+') {
		options.xneg = *(str++) == '-';
		if (*str < '0' || *str > '9')
			goto error;
		options.geometry.x = strtoul(str, &str, 10);
		if (*str == '%')
			options.xpercent = *(str++);
		if (options.xpercent && options.geometry.x > 100)
			goto error;
		if (!options.xpercent && options.geometry.x > SHRT_MAX)
			goto error;

		if (*str != '-' && *str != '+')
			goto error;
		options.yneg = *(str++) == '-';
		if (*str < '0' || *str > '9')
			goto error;
		options.geometry.y = strtoul(str, &str, 10);
		if (*str == '%')
			options.ypercent = *(str++);
		if (options.ypercent && options.geometry.y > 100)
			goto error;
		if (!options.ypercent && options.geometry.y > SHRT_MAX)
			goto error;
		options.userplaced = true;
	}
	if (*str == '@' || *str == ':') {
		str++;
		options.use_monitor = true;
		if (*str >= '0' && *str <= '9') {
			options.monitor = strtoul(str, &str, 10);
		} else if (*str == 'c') {
			options.monitor = -1;
			return;
		} else {
			goto error;
		}
	}
	if (*str == '\0')
		return;
error:
	errx(EXIT_FAILURE, "%s: invalid geometry specification", geomspec);
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

static bool
setbutton(const char *s)
{
	size_t len;
	char c;

	if ((len = strlen(s)) < 1)
		return false;
	c = s[len - 1];
	if (c >= '1' && c <= '9') {
		options.button = c - '0';
		return true;
	}
	return false;
}

static void
setmodifier(const char *s)
{
	size_t span;

	if ((span = strcspn(s, "-")) < 1)
		return;
	switch (s[span - 1]) {
	case '1': options.modifier = Mod1Mask; break;
	case '2': options.modifier = Mod2Mask; break;
	case '3': options.modifier = Mod3Mask; break;
	case '4': options.modifier = Mod4Mask; break;
	case '5': options.modifier = Mod5Mask; break;
	default:
		if (strncasecmp(s, "Alt", 3) == 0) {
			options.modifier = Mod1Mask;
		} else if (strncasecmp(s, "Super", 5) == 0) {
			options.modifier = Mod4Mask;
		}
		break;
	}
}

static void
parseoptions(int argc, char *argv[])
{
	int ch;

	options.argc = argc;
	options.argv = argv;
	if (argv[0] != NULL && argv[0][0] != '\0') {
		options.name = strchr(argv[0], '/');
		if (options.name != NULL) {
			options.name++;
		} else {
			options.name = argv[0];
		}
	}
	while ((ch = getopt(argc, argv, "ifN:p:rt:wx:X:")) != -1) switch (ch) {
	case 'N':
		options.name = optarg;
		break;
	case 'f':
		options.filebrowse = true;
		break;
	case 'p':
		parsegeometry(optarg);
		break;
	case 't':
		options.client = strtoul(optarg, NULL, 0);
		break;
	case 'w':
		options.windowed = true;
		break;
	case 'x':
		if (setbutton(optarg))
			options.rootmode = true;
		setmodifier(optarg);
		break;
	default:
		usage();
		break;

	/* options below are deprecated and ignored */
	case 'i':
	case 'r':
	case 'X':
		break;
	}
	if (options.rootmode) {
		options.client = None;
		options.windowed = false;
	} else if (options.windowed) {
		options.rootmode = false;
	}
	argc -= optind;
	argv += optind;
	switch (argc) {
	case 1:
		options.title = argv[0];
		break;
	case 0:
		options.title = options.class;
		break;
	default:
		usage();
		exit(EXIT_FAILURE);
	}
	return;
}

static Window
createwindow(struct widget *widget, XRectangle *geometry, long eventmask, bool override)
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

static struct item *
allocitem(const char *label, const char *output, char *file)
{
	struct item *item;

	item = emalloc(sizeof *item);
	*item = (struct item){ 0 };
	if (label == NULL) {
		item->label = NULL;
		item->output = NULL;
		item->labellen = 0;
	} else {
		item->label = estrdup(label);
		item->labellen = strlen(item->label);
		if (label == output) {
			item->output = item->label;
		} else if (output != NULL) {
			item->output = estrdup(output);
		}
	}
	if (file != NULL)
		item->file = estrdup(file);
	return item;
}

static char *
gettextprop(Display *dpy, Window win, Atom atom)
{
	XTextProperty tprop = { .value = NULL };
	int count;
	char **list = NULL;
	char *s = NULL;

	if (!XGetTextProperty(dpy, win, &tprop, atom))
		goto error;
	if (tprop.nitems == 0)
		goto error;
	if (XmbTextPropertyToTextList(dpy, &tprop, &list, &count) != Success)
		goto error;
	if (count < 1 || list == NULL || *list == NULL)
		goto error;
	s = strdup(list[0]);
error:
	XFreeStringList(list);
	XFree(tprop.value);
	return s;
}

static Bool
parse_alignment(void *, void *addr, const char *value)
{
	enum alignment *dst = addr;
	if (strcasecmp(value, "center") == 0)
		*dst = ALIGN_CENTER;
	else if (strcasecmp(value, "left") == 0)
		*dst = ALIGN_LEFT;
	else if (strcasecmp(value, "right") == 0)
		*dst = ALIGN_RIGHT;
	else
		return false;
	return true;
}

static void
setfont(struct widget *widget, const char *facename, double facesize)
{
	ctrlfnt *fontset;

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
	widget->separatorh = widget->itemh / 2;
}

static Bool
parse_width(void *, void *addr, char const *value)
{
	int *dst = addr;
	long n;

	n = strtol(value, NULL, 10);
	if (n < 0 || n > 1024)
		return false;
	*dst = n;
	return true;
}

static void
setcolor(Display *display, struct color *color, struct rgb rgb)
{
	color->chans = (XRenderColor){
		.red   = rgb.red,
		.green = rgb.green,
		.blue  = rgb.blue,
		.alpha = 0xFFFF,
	};
	XRenderFillRectangle(
		display,
		PictOpSrc,
		color->pict,
		&color->chans,
		0, 0, 1, 1
	);
}

static Bool
parse_bg(void *ctx, void *, char const *value)
{
	struct widget *widget = ctx;
	struct rgb rgb;
	struct hsy hsy;
	XColor xcolor;

	if (value == NULL)
		return false;
	if (!XParseColor(widget->display, widget->colormap, value, &xcolor)) {
		warnx("%s: unkwnown color name", value);
		return false;
	}
	hsy = to_hsy(rgb = (struct rgb){
		.red = xcolor.red,
		.green = xcolor.green,
		.blue = xcolor.blue,
	});

	setcolor(widget->display, &widget->colors[COLOR_BG], rgb);
	if (hsy.luma < 0.5) {
		/* dark background; make text white */
		setcolor(widget->display, &widget->colors[COLOR_FG], (struct rgb){1,1,1});
	} else {
		/* light background; make text black */
		setcolor(widget->display, &widget->colors[COLOR_FG], (struct rgb){0,0,0});
	}
	setcolor(widget->display, &widget->colors[COLOR_TOP], to_rgb((struct hsy){
		hsy.hue, hsy.saturation, hsy.luma*0.5 + 0.5
	}));
	setcolor(widget->display, &widget->colors[COLOR_BOT], to_rgb((struct hsy){
		hsy.hue, hsy.saturation, hsy.luma*0.5
	}));
	return true;
}

static Bool
parse_color(void *ctx, void *addr, char const *value)
{
	struct widget *widget = ctx;
	struct color *color = addr;
	XColor xcolor;

	if (value == NULL)
		return false;
	if (!XParseColor(widget->display, widget->colormap, value, &xcolor))
		return false;
	setcolor(widget->display, color, (struct rgb){
		.red = xcolor.red,
		.green = xcolor.green,
		.blue = xcolor.blue,
	});
	return true;
}

static Bool
parse_double(void *, void *addr, char const *value)
{
	char *endp;
	double *dst = addr;
	double d;

	d = strtod(value, &endp);
	if (endp == value || *endp != '\0')
		return false;
	*dst = d;
	return true;
}

static Bool
parse_opacity(void *ctx, void *, const char *value)
{
	struct widget *widget = ctx;
	double d;

	if (!parse_double(NULL, &d, value) || d < 0.0 || d > 1.0)
		return false;
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->opacity.pict,
		&(XRenderColor){ .alpha = d * 0xFFFF},
		0, 0, 1, 1
	);
	return true;
}

static Bool
parse_tearoff(void *, void *addr, char const *value)
{
	bool *dst = addr;
	if (dst != NULL && value != NULL)
		*dst = ctrlres_istrue(value) || strcasestr(value, "ENABLED");
	return true;
}

static void
loadresources(struct widget *widget, const char *str)
{
	XrmDatabase xdb = NULL;
	char const *fontspec = NULL;
	double fontsize = 0.0;

	if (str != NULL)
		xdb = XrmGetStringDatabase(str);
#define MK_RES(n, c, p, fun) (struct ctrlres){\
	.name_path  = (char *[]){options.name, n},\
	.class_path = (char *[]){options.class, c},\
	.nnodes = 2, .addr = p, .setter = fun}
	ctrlres_parsexrdb(widget, (XrmDatabase[]){xdb}, 1, (struct ctrlres[]){
		MK_RES("alignment", "Alignment", &widget->alignment, parse_alignment),
		MK_RES("font", "Font", &fontspec, CTRLRES_SETSTR),
		MK_RES("faceName", "FaceName", &fontspec, CTRLRES_SETSTR),
		MK_RES("fontSize", "FontSize", &fontsize, parse_double),
		MK_RES("gap", "Gap", &widget->gap, parse_width),
		MK_RES("maxItems", "MaxItems", &widget->maxitems, parse_width),
		MK_RES("background", "Background", NULL, parse_bg),
		MK_RES("foreground", "Foreground", &widget->colors[COLOR_FG], parse_color),
		MK_RES("activeBackground", "ActiveBackground", &widget->colors[COLOR_SELBG], parse_color),
		MK_RES("activeForeground", "ActiveForeground", &widget->colors[COLOR_SELFG], parse_color),
		MK_RES("opacity", "Opacity", NULL, parse_opacity),
		MK_RES("tearOff", "TearOff", &widget->tearoff, CTRLRES_SETBOOL),

		/* undocumented; for compatibility with X11/Motif's TearOffModel */
		MK_RES("tearOffModel", "TearOffModel", &widget->tearoff, parse_tearoff),
	}, 13);
	if (fontspec != NULL)
		setfont(widget, fontspec, fontsize);
	XrmDestroyDatabase(xdb);
}

static int
createpicture(struct widget *widget, Picture *picture, Pixmap *pixmap,
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
		isalpha
			? XRenderFindStandardFormat(widget->display, PictStandardA8)
			: XRenderFindVisualFormat(widget->display, widget->visual),
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

static struct widget *
setup(Display *display, int screen, Visual *visual, Colormap colormap, int depth)
{
	struct widget *widget;

	if ((widget = malloc(sizeof(*widget))) == NULL)
		return NULL;
	*widget = (struct widget){
		.display = display,
		.screen = screen,
		.rootwin = RootWindow(display, screen),
		.visual = visual,
		.depth = depth,
		.colormap = colormap,
	};
	XSelectInput(widget->display, widget->rootwin, PropertyChangeMask);

	widget->window = createwindow(widget, NULL, 0, True);
	if (widget->window == None) {
		XCloseDisplay(widget->display);
		errx(EXIT_FAILURE, "cannot create window");
	}

#define COLOR(r,g,b) (XRenderColor){.red=(r),.green=(g),.blue=(b),.alpha=0xFFFF}

	/* default hardcoded values (overriddable at runtime) */
	widget->colors[COLOR_BG].chans = COLOR(0x3100, 0x3100, 0x3100);
	widget->colors[COLOR_FG].chans = COLOR(0xFFFF, 0xFFFF, 0xFFFF);
	widget->colors[COLOR_SELBG].chans = COLOR(0x3400, 0x6500, 0xA400);
	widget->colors[COLOR_SELFG].chans = COLOR(0xFFFF, 0xFFFF, 0xFFFF);
	widget->colors[COLOR_TOP].chans = COLOR(0x7300, 0x7300, 0x7300);
	widget->colors[COLOR_BOT].chans = COLOR(0x1000, 0x1000, 0x1000);
	widget->border.chans = COLOR(0x0000, 0x0000, 0x0000);
	widget->borderwid = 1;
	widget->iconsize = 16;
	widget->gap = 0;
	widget->alignment = ALIGN_LEFT;

	for (size_t color = 0; color < COLOR_LAST; color++) {
		int retval;
		retval = createpicture(
			widget,
			&widget->colors[color].pict,
			&widget->colors[color].pix,
			&widget->colors[color].chans,
			false
		);
		if (retval == RETURN_FAILURE) {
			XDestroyWindow(widget->display, widget->window);
			XCloseDisplay(widget->display);
			errx(EXIT_FAILURE, "cannot create picture");
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
	loadresources(widget, XResourceManagerString(widget->display));
	if (widget->fontset == NULL)
		setfont(widget, NULL, 0.0);
	if (widget->fontset == NULL) {
		XDestroyWindow(widget->display, widget->window);
		XCloseDisplay(widget->display);
		errx(EXIT_FAILURE, "cannot load any font");
	}
	return widget;
}

static struct item *
parsestdin(void)
{
	struct item *root = NULL;
	struct item *prev = NULL;
	struct item *item;
	char *s, buf[BUFSIZ];
	char *file, *label, *output;
	size_t level = 0;
	size_t prevlvl = 0;

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
		if (output != NULL && strcmp(output, ":") == 0)
			output = NULL;

		if (label != NULL && prev != NULL && prev->label != NULL &&
		    level == prevlvl && strcmp(label, "''") == 0) {
			free(prev->altoutput);
			prev->altoutput = estrdup(output);
			continue;
		}

		item = allocitem(label, output, file);
		if (prev == NULL) {
			root = item;
		} else if (level < prevlvl) {
			for (size_t i = level; i < prevlvl; i++){
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
cleanitems(struct item *item, struct item *skip)
{
	struct item *tmp;

	if (item == NULL)
		return;
	if (item == skip)
		return;
	while (item != NULL) {
		if (item->children != NULL)
			cleanitems(item->children, skip);
		tmp = item;
		item = item->next;
		if (tmp->label != tmp->output)
			free(tmp->label);
		free(tmp->altoutput);
		free(tmp->output);
		free(tmp);
	}
}

static void
cleanup(struct widget *widget)
{
	if (widget == NULL)
		return;
	if (widget->fontset != NULL)
		ctrlfnt_free(widget->fontset);
	for (int color = 0; color < COLOR_LAST; color++) {
		if (widget->colors[color].pict != None) {
			XRenderFreePicture(
				widget->display,
				widget->colors[color].pict
			);
		}
		if (widget->colors[color].pix != None) {
			XFreePixmap(
				widget->display,
				widget->colors[color].pix
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
	if (widget->colormap != None)
		XFreeColormap(widget->display, widget->colormap);
	if (widget->window != None)
		XDestroyWindow(widget->display, widget->window);
}

static void
getposition(struct widget *widget, XRectangle *geometry)
{
	XineramaScreenInfo *info = NULL;
	Window dw;          /* dummy variable */
	int di;             /* dummy variable */
	unsigned du;        /* dummy variable */
	int nmons;
	int x, y;

	XQueryPointer(
		widget->display,
		widget->rootwin,
		&dw, &dw,
		&x, &y,
		&di, &di,
		&du
	);
	if (!options.userplaced) {
		geometry->x = x;
		geometry->y = y;
	}
	widget->monitor.x = widget->monitor.y = 0;
	widget->monitor.width = DisplayWidth(widget->display, widget->screen);
	widget->monitor.height = DisplayHeight(widget->display, widget->screen);
	info = XineramaQueryScreens(widget->display, &nmons);
	if (info == NULL)
		return;
	if (options.use_monitor && options.monitor >= 0 && options.monitor < nmons) {
		widget->monitor.x = info[options.monitor].x_org;
		widget->monitor.y = info[options.monitor].y_org;
		widget->monitor.width = info[options.monitor].width;
		widget->monitor.height = info[options.monitor].height;
	} else for (int i = 0; i < nmons; i++) {
		if (x < info[i].x_org)
			continue;
		if (y < info[i].y_org)
			continue;
		if (x >= info[i].x_org + info[i].width)
			continue;
		if (y >= info[i].y_org + info[i].height)
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
grabpointer(struct widget *widget)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };

	/* we may have to wait for another process to ungrab */
	for (int i = 0; i < 1000; i++) {
		if (XGrabPointer(
			widget->display, widget->rootwin, True,
			ButtonPressMask, GrabModeAsync, GrabModeAsync,
			None, None, CurrentTime
		) == GrabSuccess) return RETURN_SUCCESS;
		nanosleep(&ts, NULL);
	}
	warnx("cannot grab pointer");
	return RETURN_FAILURE;
}

static int
grabkeyboard(struct widget *widget)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };

	/* we may have to wait for another process to ungrab */
	for (int i = 0; i < 1000; i++) {
		if (XGrabKeyboard(
			widget->display, widget->rootwin, True,
			GrabModeAsync, GrabModeAsync,
			CurrentTime
		) == GrabSuccess) return RETURN_SUCCESS;
		nanosleep(&ts, NULL);
	}
	warnx("cannot grab keyboard");
	return RETURN_FAILURE;
}

static void
drawshadows(struct widget *widget, Picture picture, XRectangle *geometry)
{
	int i;

	for(i = 0; i < widget->shadowwid; i++) {
		/* draw light shadow */
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_TOP].chans,
			i, i,
			1, geometry->height - (i * 2 + 1)
		);
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_TOP].chans,
			i, i,
			geometry->width - (i * 2 + 1), 1
		);

		/* draw dark shadow */
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_BOT].chans,
			geometry->width - 1 - i, i,
			1, geometry->height - i * 2
		);
		XRenderFillRectangle(
			widget->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_BOT].chans,
			i, geometry->height - 1 - i,
			geometry->width - i * 2, 1
		);
	}
}

static void
drawdashline(struct widget *widget, Picture picture, int width, int y)
{
	XRectangle toprects[32];
	XRectangle botrects[32];
	size_t nrects;
	int x;
	int w;
	int maxw;

	x = widget->shadowwid + PADDING;
	y += widget->separatorh / 2;
	w = 0;
	maxw = width - widget->shadowwid * 2 - PADDING * 2;
	while (w < maxw) {
		nrects = 0;
		for (size_t i = 0; i < LEN(toprects); i++) {
			static int const DASH_SIZE = 8;
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
		for (size_t i = 0; i < nrects; i++) {
			XRenderFillRectangles(
				widget->display,
				PictOpSrc,
				picture,
				&widget->colors[COLOR_TOP].chans,
				toprects,
				nrects
			);
			XRenderFillRectangles(
				widget->display,
				PictOpSrc,
				picture,
				&widget->colors[COLOR_BOT].chans,
				botrects,
				nrects
			);
		}
	}
}

static void
drawseparator(struct widget *widget, Picture picture, XRectangle *rect)
{
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		picture,
		&widget->colors[COLOR_BOT].chans,
		widget->shadowwid + PADDING,
		rect->y + widget->separatorh / 2 - 1,
		rect->width - widget->shadowwid * 2 - PADDING * 2,
		1
	);
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		picture,
		&widget->colors[COLOR_TOP].chans,
		widget->shadowwid + PADDING,
		rect->y + widget->separatorh / 2,
		rect->width - widget->shadowwid * 2 - PADDING * 2,
		1
	);
}

static void
drawtriangle(struct widget *widget, Picture picture, Picture src, int x, int y, int direction)
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
		XRenderFindStandardFormat(widget->display, PictStandardA8),
		0, 0,
		&triangle,
		1
	);
}

static bool
cantearoff(struct widget *widget, struct menu *menu)
{
	return (menu->next != NULL || !options.windowed) && widget->tearoff;
}

static int
firstitempos(struct widget *widget, struct menu *menu)
{
	int y;

	y = widget->shadowwid;
	if (cantearoff(widget, menu))
		y += widget->separatorh;
	if (menu->overflow)
		y += widget->separatorh;
	return y;
}

static void
initimlib(struct widget *widget)
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
loadicon(struct widget *widget, const char *file, int size, int *width_ret, int *height_ret)
{
	Imlib_Image icon = NULL;
	Imlib_Load_Error errcode;
	char path[PATH_MAX];
	const char *errstr;
	int width;
	int height;

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
		for (size_t i = 0; i < options.niconpaths; i++) {
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

static bool
openssubmenu(struct item *item)
{
	if (item->children != NULL)
		return true;
	if (options.filebrowse && item->output != NULL && item->output[0] == '/')
		return true;
	return false;
}

static void
drawmenu(struct widget *widget, struct menu *menu)
{
	struct item *item;
	Imlib_Image image;
	XRectangle rect;
	int textx, textw, iconw, iconh, y;
	struct Canvas canvas[CANVAS_FINAL][LAYER_LAST];

	for (size_t i = 0; i < CANVAS_LAST; i++) {
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
			XRenderFindVisualFormat(widget->display, widget->visual),
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
	for (size_t i = 0; i < CANVAS_FINAL; i++) for (size_t j = 0; j < LAYER_LAST; j++) {
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
			XRenderFindVisualFormat(widget->display, widget->visual),
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
	for (size_t i = 0; i < CANVAS_FINAL; i++) {
		drawshadows(widget, canvas[i][LAYER_BG].picture, &menu->geometry);
		XRenderComposite(
			widget->display,
			PictOpSrc,
			widget->colors[i==CANVAS_SELECT ?  COLOR_SELBG : COLOR_BG].pict,
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
			rect.height = widget->separatorh;
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
		if (openssubmenu(item)) {
			drawtriangle(
				widget,
				canvas[CANVAS_NORMAL][LAYER_FG].picture,
				widget->colors[COLOR_FG].pict,
				rect.width - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD/2,
				rect.y + widget->itemh/2 - TRIANGLE_HEIGHT/2,
				DIR_RIGHT
			);
			drawtriangle(
				widget,
				canvas[CANVAS_SELECT][LAYER_FG].picture,
				widget->colors[COLOR_SELFG].pict,
				rect.width - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD/2,
				rect.y + widget->itemh/2 - TRIANGLE_HEIGHT/2,
				DIR_RIGHT
			);
		}
		textw = ctrlfnt_width(
			widget->fontset,
			item->label,
			item->labellen
		);
		if (widget->alignment == ALIGN_RIGHT && menu->hassubmenu)
			textx = rect.width - textw - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD;
		else if (widget->alignment == ALIGN_RIGHT)
			textx = rect.width - textw - PADDING;
		else if (widget->alignment == ALIGN_CENTER)
			textx = rect.x + (menu->geometry.width - textw) / 2;
		else
			textx = rect.x;
		if (item->output != NULL) {
			ctrlfnt_draw(
				widget->fontset,
				canvas[CANVAS_NORMAL][LAYER_FG].picture,
				widget->colors[COLOR_FG].pict,
				(XRectangle){
					.x = textx,
					.y = rect.y,
					.width = rect.width,
					.height = rect.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				canvas[CANVAS_SELECT][LAYER_FG].picture,
				widget->colors[COLOR_SELFG].pict,
				(XRectangle){
					.x = textx,
					.y = rect.y,
					.width = rect.width,
					.height = rect.height,
				},
				item->label,
				item->labellen
			);
		} else {
			ctrlfnt_draw(
				widget->fontset,
				canvas[CANVAS_NORMAL][LAYER_FG].picture,
				widget->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = rect.y + 1,
					.width = rect.width,
					.height = rect.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				canvas[CANVAS_NORMAL][LAYER_FG].picture,
				widget->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = rect.y,
					.width = rect.width,
					.height = rect.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				canvas[CANVAS_SELECT][LAYER_FG].picture,
				widget->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = rect.y + 1,
					.width = rect.width,
					.height = rect.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				canvas[CANVAS_SELECT][LAYER_FG].picture,
				widget->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = rect.y,
					.width = rect.width,
					.height = rect.height,
				},
				item->label,
				item->labellen
			);
		}
next:
		rect.y += rect.height;
		if (menu->overflow &&
		    rect.y + widget->itemh * 2 >=
		    menu->geometry.height) {
			break;
		}
	}
	for (size_t i = 0; i < CANVAS_FINAL; i++) {
		y = widget->shadowwid;
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
			y += widget->separatorh;
		}
		if (menu->overflow) {
			drawtriangle(
				widget,
				canvas[i][LAYER_FG].picture,
				widget->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->geometry.width / 2 - TRIANGLE_HEIGHT / 2,
				y + widget->separatorh /2 - TRIANGLE_WIDTH / 2,
				DIR_UP
			);
			drawtriangle(
				widget,
				canvas[i][LAYER_FG].picture,
				widget->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->geometry.width / 2 - TRIANGLE_HEIGHT / 2,
				menu->geometry.height - widget->separatorh /2
				- TRIANGLE_WIDTH / 2,
				DIR_DOWN
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
	for (size_t i = 0; i < CANVAS_FINAL; i++) for (size_t j = 0; j < LAYER_LAST; j++) {
		XRenderFreePicture(widget->display, canvas[i][j].picture);
		XFreePixmap(widget->display, canvas[i][j].pixmap);
	}
}

static void
commitdraw(struct widget *widget, struct menu *menu, int ypos)
{
	int height;

	if (menu->selected == NULL)
		return;
	if (menu->selected == &tearoff || menu->selected == &scrollup ||
	    menu->selected == &scrolldown)
		height = widget->separatorh;
	else
		height = widget->itemh;
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
	if (ypos >= 0) {
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
			height
		);
	}
	XSetWindowBackgroundPixmap(
		widget->display,
		menu->window,
		menu->canvas[CANVAS_FINAL].pixmap
	);
	XClearWindow(widget->display, menu->window);
}

static bool
selitem(struct widget *widget, struct menu *menu, struct item *from, struct item *first, struct item *last, int ypos, int dir)
{
	struct item *item, *prev;
	int prevypos;

	prev = NULL;
	for (item = from;
	     item != NULL;
	     item = (dir == SEL_PREV) ? item->prev : item->next) {
		if (item->label != NULL && item->output != NULL &&
		    (item != menu->selected || dir == SEL_LAST || dir == SEL_FIRST)) {
			prev = item;
			prevypos = ypos;
			if (menu->first != first) {
				menu->first = first;
				menu->last = last;
			}
			if (dir != SEL_LAST) {
				menu->selected = item;
				drawmenu(widget, menu);
				commitdraw(widget, menu, ypos);
				return true;
			}
		}
		if ((dir == SEL_PREV) && menu->overflow && item == first) {
			first = first->prev;
			last = last->prev;
				ypos = firstitempos(widget, menu);
		} else if (menu->overflow && item == last) {
			first = first->next;
			last = last->next;
		} else if (item->label == NULL) {
			ypos += ((dir == SEL_PREV) ? -1 : 1) * widget->separatorh;
		} else {
			ypos += ((dir == SEL_PREV) ? -1 : 1) * widget->itemh;
		}
	}
	if (dir == SEL_LAST && prev != NULL) {
		menu->selected = prev;
		drawmenu(widget, menu);
		commitdraw(widget, menu, prevypos);
		return true;
	} else {
		drawmenu(widget, menu);
		commitdraw(widget, menu, -1);
		return false;
	}
}

static void
selfirst(struct widget *widget, struct menu *menu)
{
	(void)selitem(
		widget, menu,
		menu->items, menu->items,
		menu->lastsave,
		firstitempos(widget, menu),
		SEL_FIRST
	);
}

static void
popupmenu(struct widget *widget, struct item *items, XRectangle *basis, bool isroot)
{
	XRectangle *monitor = &widget->monitor;
	struct menu *menu;
	struct item *caller;
	Atom type;
	struct item *item;
	size_t nitems;
	unsigned int textw, menuh;
	int xgap, ygap;
	char *name;
	bool override_redirect, tearoff;

	type = XInternAtom(widget->display, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
	override_redirect = true;
	tearoff = widget->tearoff;
	if (widget->menus != NULL) {
		if (widget->menus->selected == NULL)
			return;         /* no item selected */
		if (!openssubmenu(widget->menus->selected))
			return;         /* item creates no submenu */
		caller = widget->menus->selected;
		name = caller->output;
		xgap = widget->gap + widget->borderwid * 2;
		ygap = -widget->shadowwid;
		if (tearoff)
			ygap -= widget->separatorh;
		(void)grabpointer(widget);
		(void)grabkeyboard(widget);
	} else {
		caller = NULL;
		name = options.title;
		if (options.userplaced)
			xgap = ygap = 0;
		else
			xgap = ygap = INITIAL_DISPLACEMENT;
		if (options.windowed) {
			tearoff = false;
			override_redirect = false;
			type = XInternAtom(widget->display, "_NET_WM_WINDOW_TYPE_MENU", False);
		}
	}
	menu = emalloc(sizeof(*menu));
	*menu = (struct menu){
		.items = items,
		.first = items,
		.title = name,
		.last = NULL,
		.selected = NULL,
	};
	menu->next = widget->menus;
	menuh = widget->shadowwid * 2;
	if (tearoff)
		menuh += widget->separatorh;
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
			menuh += widget->separatorh;
		}
		if (item->file != NULL) {
			menu->hasicon = true;
			menu->nicons++;
		}
		if (openssubmenu(item))
			menu->hassubmenu = true;
		menu->geometry.width = MAX(menu->geometry.width, textw);
		if (menu->overflow)
			continue;
		menu->lastsave = menu->last = item->prev;
		if (widget->maxitems > 0 && nitems > (unsigned)widget->maxitems) {
			menu->lastsave = menu->last = item->prev;
			menu->overflow = true;
		}
		if (menuh + widget->separatorh * 2 <= monitor->height) {
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
		ygap -= widget->separatorh;
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
		menu->geometry.y -= menu->geometry.height;
	}

	if (isroot && options.userplaced) {
		XRectangle screen = *monitor;
		int x = options.geometry.x;
		int y = options.geometry.y;

		if (!options.use_monitor) {
			screen.x = screen.y = 0;
			screen.width = DisplayWidth(widget->display, widget->screen);
			screen.height = DisplayHeight(widget->display, widget->screen);
		}

		if (options.xpercent) {
			if (options.xneg) x = 100 - x;
			menu->geometry.x = screen.x + screen.width * x / 100;
			menu->geometry.x -= menu->geometry.width / 2;
		} else if (options.xneg) {
			menu->geometry.x = screen.x + screen.width - x - menu->geometry.width;
		} else {
			menu->geometry.x = screen.x + x;
		}

		if (options.ypercent) {
			if (options.yneg) y = 100 - y;
			menu->geometry.y = screen.y + screen.height * y / 100;
			menu->geometry.y -= menu->geometry.height / 2;
		} else if (options.yneg) {
			menu->geometry.y = screen.y + screen.height - y - menu->geometry.height;
		} else {
			menu->geometry.y = screen.y + y;
		}
	}

	menu->window = createwindow(
		widget, &menu->geometry,
		KeyPressMask | StructureNotifyMask | LeaveWindowMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		override_redirect
	);
	if (!options.rootmode && options.client != None) {
		(void)XSetTransientForHint(
			widget->display,
			menu->window,
			options.client
		);
		(void)XSelectInput(
			widget->display,
			options.client,
			StructureNotifyMask
		);
	}
	(void)XSetWMProtocols(
		widget->display,
		menu->window,
		(Atom[]){XInternAtom(widget->display, "WM_DELETE_WINDOW", False)},
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
		XInternAtom(widget->display, "_NET_WM_NAME", False),
		XInternAtom(widget->display, "UTF8_STRING", False),
		8,
		PropModeReplace,
		(unsigned char *)name,
		strlen(name)
	);
	(void)XChangeProperty(
		widget->display,
		menu->window,
		XInternAtom(widget->display, "_NET_WM_WINDOW_TYPE", False),
		XA_ATOM,
		32,
		PropModeReplace,
		(unsigned char *)&type,
		1
	);

	widget->menus = menu;
	drawmenu(widget, menu);
	if (caller != NULL)
		selfirst(widget, menu);
	else
		commitdraw(widget, menu, -1);
	XMapRaised(widget->display, menu->window);
}

static void
delmenu(struct widget *widget)
{
	struct menu *menu;

	if ((menu = widget->menus) == NULL)
		return;
	if (menu->directory)
		cleanitems(menu->items, NULL);
	widget->menus = menu->next;
	XDestroyWindow(widget->display, menu->window);
	for (size_t i = 0; i < CANVAS_LAST; i++) {
		XRenderFreePicture(widget->display, menu->canvas[i].picture);
		XFreePixmap(widget->display, menu->canvas[i].pixmap);
	}
}

static void
ungrab(struct widget *widget)
{
	XUngrabPointer(widget->display, CurrentTime);
	XUngrabKeyboard(widget->display, CurrentTime);
}

static void
closewidget(struct widget *widget)
{
	while (widget->menus != NULL)
		delmenu(widget);
	ungrab(widget);
}

static void
closepopups(struct widget *widget)
{
	while (widget->menus != NULL) {
		if (options.windowed && widget->menus->next == NULL) {
			ungrab(widget);
			break;
		}
		delmenu(widget);
	}
}

static struct item *
getitem(struct widget *widget, struct menu *menu, long y, int *ypos)
{
	long h;
	struct item *item;

	h = widget->shadowwid;
	if (cantearoff(widget, menu)) {
		if (y < h + widget->separatorh) {
			item = &tearoff;
			goto done;
		}
		h += widget->separatorh;
	}
	if (menu->overflow && y >= h && y < h + widget->separatorh) {
		item = &scrollup;
		goto done;
	}
	if (menu->overflow && y >= menu->geometry.height - widget->separatorh &&
	    y < menu->geometry.height) {
		h = menu->geometry.height - widget->separatorh;
		item = &scrolldown;
		goto done;
	}
	if (menu->overflow)
		h += widget->separatorh;
	for (item = menu->first; item != NULL; item = item->next) {
		if (item->label == NULL) {
			h += widget->separatorh;
			continue;
		}
		if (item->output == NULL) {
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
scroll(struct widget *widget, bool down)
{
	XRectangle rect;
	struct menu *menu;
	time_t elapsed;
	struct timespec ts;
	int ret;
	struct pollfd pfd = {
		.fd = XConnectionNumber(widget->display),
		.events = POLLIN,
	};

	if ((menu = widget->menus) == NULL)
		return;
	if (down)
		rect.y = menu->geometry.height - widget->separatorh - widget->shadowwid;
	else
		rect.y = widget->shadowwid + widget->separatorh;
	rect.x = 0;
	rect.width = menu->geometry.width;
	rect.height = widget->separatorh;
	for (;;) {
		static int const SCROLL_TIME = 32;
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
			drawmenu(widget, menu);
			commitdraw(widget, menu, rect.y);
			XFlush(widget->display);
			if (ret == 0)
				break;
			egettime(&widget->lasttime);
			break;
		}
	}
}

static void
forkandtearoff(struct widget *widget, struct menu *menu)
{
	if (efork() == 0) {
		/* child */
		ctrlfnt_free(widget->fontset);
		while (close(XConnectionNumber(widget->display)) == -1) {
			if (errno == EINTR)
				continue;
			err(EXIT_FAILURE, "close");
		}
		*widget = (struct widget){ 0 };
		widget->display = NULL;
		options.items = menu->first,
		options.monitor = -1,
		options.userplaced = true,
		options.xpercent = options.ypercent = false;
		options.windowed = true,
		options.rootmode = false,
		options.title = estrdup(menu->title),
		options.freetitle = true,
		options.geometry.x = menu->geometry.x,
		options.geometry.y = menu->geometry.y,
		options.geometry.width = 0,
		options.geometry.height = 0,
		cleanitems(options.items, menu->first);
		longjmp(jmpenv, 1);
		exit(EXIT_FAILURE);
	}
	closepopups(widget);
}

static int
direntsel(const struct dirent *dp)
{
	return dp->d_name[0] != '.' && strpbrk(dp->d_name, "\t\n") == NULL;
}

static int
direntcmp(const struct dirent **ap, const struct dirent **bp)
{
	const struct dirent *a = *ap;
	const struct dirent *b = *bp;

	if (a->d_type == DT_DIR && b->d_type != DT_DIR)
		return -1;
	if (b->d_type == DT_DIR && a->d_type != DT_DIR)
		return 1;
	return strcoll(a->d_name, b->d_name);
}

static struct item *
listdirentries(const char *dirname)
{
	struct dirent **namelist;
	struct item *items, *item, *prev;
	int nents;
	char buf[PATH_MAX + 8];

	nents = scandir(dirname, &namelist, &direntsel, &direntcmp);
	if (nents == -1)
		return NULL;
	items = NULL;
	prev = NULL;
	for (int i = 0; i < nents; i++) {
		snprintf(
			buf,
			sizeof(buf),
			"%s%s/%s",
			namelist[i]->d_type == DT_DIR ? "" : "file:",
			dirname,
			namelist[i]->d_name
		);
		item = allocitem(namelist[i]->d_name, buf, NULL);
		if (prev == NULL)
			items = item;
		else
			prev->next = item;
		item->prev = prev;
		prev = item;
		free(namelist[i]);
	}
	free(namelist);
	return items;
}

static void
printitem(struct widget *widget, const char *str)
{
	(void)printf("%s\n", str);
	(void)fflush(stdout);
	closepopups(widget);
}

static void
openitem(struct widget *widget, struct item *item, int ypos, bool alt)
{
	XRectangle rect;
	struct menu *menu;
	struct item *items;
	char buf[PATH_MAX + 8];

	if (item == NULL || item == &tearoff)
		return;
	menu = widget->menus;
	menu->selected = item;
	rect = menu->geometry;
	rect.y += ypos;
	if (item == &scrollup || item == &scrolldown)
		return;
	if (item->children != NULL) {
		popupmenu(widget, item->children, &rect, false);
	} else if (alt && item->altoutput != NULL) {
		printitem(widget, item->altoutput);
	} else if (options.filebrowse && item->output != NULL &&
	           item->output[0] == '/') {
		if (alt) {
			snprintf(buf, PATH_MAX, "%s%s", "file:", item->output);
			printitem(widget, buf);
			return;
		}
		items = listdirentries(item->output);
		if (items == NULL)
			return;
		popupmenu(widget, items, &rect, false);
		widget->menus->directory = true;
		widget->menus->items = items;
	} else if (item->output != NULL) {
		printitem(widget, item->output);
	}
}

static struct menu *
getmenu(struct widget *widget, Window window)
{
	struct menu *menu;

	for (menu = widget->menus; menu != NULL; menu = menu->next)
		if (window == menu->window)
			return menu;
	return NULL;
}

static void
keypress(struct widget *widget, struct menu *menu, unsigned int state, KeySym ksym)
{
	struct item *last, *first, *start;
	int ypos;

	if (menu == NULL)
		return;
	if (ksym == XK_Tab && FLAG(state, ShiftMask))
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
		start = menu->selected->next;
		ypos = menu->selposition;
		if (selitem(widget, menu, start, first, last, ypos, SEL_NEXT))
			break;
		/* fallthrough */
	case XK_Home:
		selfirst(widget, menu);
		break;
	case XK_ISO_Left_Tab:
	case XK_Up:
		first = menu->first;
		last = menu->last;
		start = menu->selected->prev;
		ypos = menu->selposition;
		if (selitem(widget, menu, start, first, last, ypos, SEL_PREV))
			break;
		/* fallthrough */
	case XK_End:
		first = menu->items;
		start = menu->items;
		last = menu->lastsave;
		ypos = firstitempos(widget, menu);
		(void)selitem(widget, menu, start, first, last, ypos, SEL_LAST);
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
		openitem(widget, menu->selected, menu->selposition, false);
		break;
	}
}

static int
run(struct widget *widget, XRectangle *geometry)
{
	XEvent xev;
	struct menu *menu;
	struct item *item;
	int ypos;

	getposition(widget, geometry);
	popupmenu(widget, options.items, geometry, true);
#define XNextEvent(dpy, xev) (XNextEvent((dpy),(xev)),(xev)->type)
	while (widget->menus != NULL) switch (XNextEvent(widget->display, &xev)) {
	case ButtonPress:
		menu = getmenu(widget, xev.xbutton.window);
		if (menu == NULL) {
			closepopups(widget);
			continue;
		}
		while (widget->menus != menu)
			delmenu(widget);
		if ((item = getitem(widget, menu, xev.xbutton.y, &ypos)) != NULL) {
			if (item->children != NULL) {
				openitem(widget, item, ypos, false);
			}
		}
		continue;
	case ButtonRelease:
		if (xev.xbutton.button != Button1 &&
		    xev.xbutton.button != Button2 &&
		    xev.xbutton.button != Button3)
			continue;
		menu = getmenu(widget, xev.xbutton.window);
		if (menu == NULL)
			continue;
		item = getitem(widget, menu, xev.xbutton.y, &ypos);
		if (item == NULL)
			continue;
		if (item->children != NULL)
			continue;
		while (widget->menus != menu)
			delmenu(widget);
		if (item == &tearoff)
			forkandtearoff(widget, menu);
		else
			openitem(widget, item, ypos, xev.xbutton.button == Button2);
		continue;
	case ClientMessage:
		if ((Atom)xev.xclient.data.l[0] == XInternAtom(widget->display, "WM_DELETE_WINDOW", False))
			closewidget(widget);
		continue;
	case ConfigureNotify:
		for (menu = widget->menus; menu != NULL; menu = menu->next)
			if (menu->window == xev.xconfigure.window)
				break;
		if (menu == NULL)
			continue;
		menu->geometry.x = xev.xconfigure.x;
		menu->geometry.y = xev.xconfigure.y;
		if (menu->geometry.width  == xev.xconfigure.width &&
		    menu->geometry.height == xev.xconfigure.height)
			continue;       /* size not changed */
		menu->geometry.width = xev.xconfigure.width;
		menu->geometry.height = xev.xconfigure.height;
		drawmenu(widget, menu);
		if (menu->selected != NULL)
			commitdraw(widget, menu, menu->selposition);
		continue;
	case DestroyNotify:
		if (options.client != None && xev.xdestroywindow.window == options.client)
			closewidget(widget);
		continue;
	case KeyPress:
		keypress(
			widget, widget->menus, xev.xkey.state,
			XkbKeycodeToKeysym(widget->display, xev.xkey.keycode, 0, 0)
		);
		continue;
	case LeaveNotify:
		if (getmenu(widget, xev.xcrossing.window) == widget->menus) {
			widget->menus->selected = NULL;
			commitdraw(widget, widget->menus, -1);
		}
		continue;
	case MotionNotify:
		menu = getmenu(widget, xev.xmotion.window);
		if (menu == NULL)
			continue;
		item = getitem(widget, menu, xev.xmotion.y, &ypos);
		if (item == &scrollup || item == &scrolldown)
			scroll(widget, item == &scrolldown);
		if (item == menu->selected)
			continue;
		if (item == NULL)
			ypos = -1;
		menu->selected = item;
		commitdraw(widget, menu, ypos);
		if (item != NULL && xev.xmotion.state & (Button1Mask|Button3Mask)) {
			while (widget->menus != menu)
				delmenu(widget);
			if (openssubmenu(item)) {
				openitem(widget, item, ypos, false);
			}
		}
		continue;
	case PropertyNotify:
		if (xev.xproperty.state == PropertyNewValue &&
		    xev.xproperty.window == widget->rootwin &&
		    xev.xproperty.atom == XA_RESOURCE_MANAGER) {
			char *str = gettextprop(
				widget->display,
				widget->rootwin,
				XA_RESOURCE_MANAGER
			);
			if (str == NULL)
				continue;
			loadresources(widget, str);
			free(str);
			for (menu = widget->menus; menu != NULL; menu = menu->next) {
				drawmenu(widget, menu);
				commitdraw(widget, menu, menu->selposition);
			}
		}
		continue;
	}
	return RETURN_SUCCESS;
}

static void
waitrootclick(struct widget *widget)
{
	XEvent xev;

	if (!options.rootmode)
		return;
	while (!XNextEvent(widget->display, &xev)) {
		if (xev.type != ButtonPress)
			continue;
		if ((options.modifier != 0 &&
		     (xev.xbutton.state & options.modifier)) ||
		    xev.xbutton.subwindow == None) {
			XAllowEvents(
				widget->display,
				AsyncPointer,
				xev.xbutton.time
			);
			return;
		}
		XAllowEvents(
			widget->display,
			ReplayPointer,
			xev.xbutton.time
		);
	}
}

int
main(int argc, char *argv[])
{
	char const *dpyname;
	struct sigaction sa;
	struct widget *widget = NULL;
	Display *display = NULL;
	XRectangle geometry = { 0 };
	Visual *visual;
	Colormap colormap;
	int depth;
	XVisualInfo vinfo;

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

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support for X11");
	if ((dpyname = XDisplayName(NULL)) == NULL || dpyname[0] == '\0')
		errx(EXIT_FAILURE, "DISPLAY is not set");
	if ((display = XOpenDisplay(dpyname)) == NULL)
		errx(EXIT_FAILURE, "%s: cannot connect to X server", dpyname);
	if (fcntl(XConnectionNumber(display), F_SETFD, FD_CLOEXEC) == -1)
		err(EXIT_FAILURE, "connection to display \"%s\"", dpyname);
	if (options.rootmode) {
		XGrabButton(
			display,
			options.button,
			AnyModifier,
			DefaultRootWindow(display),
			False,
			ButtonPressMask,
			GrabModeSync,
			GrabModeSync,
			None,
			None
		);
	}
	if (XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &vinfo)) {
		colormap = XCreateColormap(
			display,
			DefaultRootWindow(display),
			vinfo.visual,
			AllocNone
		);
		visual = vinfo.visual;
		depth = vinfo.depth;
	} else {
		colormap = DefaultColormap(display, DefaultScreen(display));
		visual = DefaultVisual(display, DefaultScreen(display));
		depth = DefaultDepth(display, DefaultScreen(display));
	}
	widget = setup(
		display, DefaultScreen(display),
		visual, colormap, depth
	);
	if (widget == NULL)
		errx(EXIT_FAILURE, "cannot create widget");
	do {
		waitrootclick(widget);
		if (!options.windowed) {
			if (grabpointer(widget) == RETURN_FAILURE)
				return EXIT_FAILURE;
			if (grabkeyboard(widget) == RETURN_FAILURE)
				return EXIT_FAILURE;
		}
		run(widget, &geometry);
		ungrab(widget);
	} while (options.rootmode);
error:
	cleanup(widget);
	free(options.iconstring);
	if (options.freetitle)
		free(options.title);
	cleanitems(options.items, NULL);
	if (display != NULL)
		XCloseDisplay(display);
	return EXIT_SUCCESS;
}
