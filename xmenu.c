#include <sys/types.h>

#include <assert.h>
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
	COLOR_BG,
	COLOR_FG,
	COLOR_TOP,
	COLOR_BOT,
	COLOR_SELBG,
	COLOR_SELFG,
	COLOR_BORDER,
	COLOR_OPACITY,
	COLOR_LAST,
};

enum {
	CANVAS_NORMAL,
	CANVAS_SELECT,
	CANVAS_FINAL,
	CANVAS_LAST
};

enum {
	DIR_UP,
	DIR_DOWN,
	DIR_LEFT,
	DIR_RIGHT
};

struct item {
	char *label;
	size_t labellen;

	char *output;
	char *altoutput;
	char *file;

	struct item *children;
	size_t nchildren;
};

struct menu {
	struct menu *next;
	struct item *items;
	size_t nitems;
	ssize_t selected;
	char const *title;
	XRectangle window_geometry;
	int canvas_width, canvas_height;
	int total_height;
	struct {
		size_t first_item;
		int offset;
	} curr_screenful, last_screenful;
	Window window;
	struct Canvas {
		Pixmap pixmap;
		Picture picture;
	} canvas[CANVAS_LAST];
	size_t nicons;
	bool tearable;
	bool overflow;
	bool hasicon;
	bool hassubmenu;
	bool directory;
};

struct Options {
	struct item *items;
	size_t nitems;
	bool windowed;
	bool rootmode;
	bool userplaced;
	bool xneg, yneg, xpercent, ypercent;
	bool monplaced;
	bool use_monitor;
	int monitor;

	unsigned int button;
	unsigned int modifier;
	Window client;

	char *iconstring;
	char *iconpaths[MAXPATHS];
	size_t niconpaths;

	XRectangle geometry;
} options;

struct widget {
	Display *display;
	Visual *visual;
	Colormap colormap;
	int depth;
	int screen;
	Window rootwin;
	Window window;
	XRectangle monitor;
	XrmDatabase cli_xrm;
	ctrlfnt *fontset;
	struct menu *menus;
	unsigned int fonth;
	int itemh, separatorh;
	int inner_border_width;  /* part of window content, for shadow/relief */
	int outer_border_width;  /* X11 border, not counted on window size */
	int iconsize, gap;
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
	} colors[COLOR_LAST];

	int argc;
	char **argv;
	char *name;
	char *class;
	char *title;
};

static jmp_buf jmpenv;

/* unindexable indices for special entries */
static ssize_t TEAROFF    = -100;
static ssize_t SCROLLUP   = -200;
static ssize_t SCROLLDOWN = -300;

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
	options.niconpaths = 0;
	if (s == NULL)
		return;
	if ((options.iconstring = strdup(s)) == NULL) {
		warn("strdup");
		return;
	}
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
		widget->outer_border_width,
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
		.red   = rgb.red * 0xFFFF,
		.green = rgb.green * 0xFFFF,
		.blue  = rgb.blue * 0xFFFF,
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
		.red = (double)xcolor.red / 0xFFFF,
		.green = (double)xcolor.green / 0xFFFF,
		.blue = (double)xcolor.blue / 0xFFFF,
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
		.red = (double)xcolor.red / 0xFFFF,
		.green = (double)xcolor.green / 0xFFFF,
		.blue = (double)xcolor.blue / 0xFFFF,
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
	widget->colors[COLOR_OPACITY].chans = (XRenderColor){ .alpha = d * 0xFFFF};
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->colors[COLOR_OPACITY].pict,
		&widget->colors[COLOR_OPACITY].chans,
		0, 0, 1, 1
	);
	return true;
}

static Bool
parse_tearoff(void *, void *addr, char const *value)
{
	bool *dst = addr;
	if (dst != NULL && value != NULL)
		*dst = ctrlres_istrue(value) || strcasecmp(value, "TEAR_OFF_ENABLED") == 0;
	return true;
}

static void
loadresources(struct widget *widget, const char *str)
{
	XrmDatabase xdb = NULL;
	char const *fontspec = NULL;
	double fontsize = 0.0;
#define MK_RES(n, c, p, fun) (struct ctrlres){\
	.name_path  = (char *[]){widget->name, n},\
	.class_path = (char *[]){widget->class, c},\
	.nnodes = 2, .addr = p, .setter = fun}
	struct ctrlres resources[] = {
		MK_RES("activeBackground", "ActiveBackground", &widget->colors[COLOR_SELBG], parse_color),
		MK_RES("activeForeground", "ActiveForeground", &widget->colors[COLOR_SELFG], parse_color),
		MK_RES("alignment", "Alignment", &widget->alignment, parse_alignment),
		MK_RES("background", "Background", NULL, parse_bg),
		MK_RES("borderWidth", "BorderWidth", &widget->outer_border_width, parse_width),
		MK_RES("borderColor", "BorderColor", &widget->colors[COLOR_BORDER], parse_color),
		MK_RES("font", "Font", &fontspec, CTRLRES_SETSTR),
		MK_RES("fontSize", "FontSize", &fontsize, parse_double),
		MK_RES("foreground", "Foreground", &widget->colors[COLOR_FG], parse_color),
		MK_RES("gap", "Gap", &widget->gap, parse_width),
		MK_RES("maxItems", "MaxItems", &widget->maxitems, parse_width),
		MK_RES("opacity", "Opacity", NULL, parse_opacity),
		MK_RES("shadowThickness", "ShadowThickness", &widget->inner_border_width, parse_width),
		MK_RES("tearOff", "TearOff", &widget->tearoff, CTRLRES_SETBOOL),

		/* undocumented; for compatibility with Motif's TearOffModel resource */
		MK_RES("tearOffModel", "TearOffModel", &widget->tearoff, parse_tearoff),

		/* overriding resources; must come later */
		MK_RES("faceName", "FaceName", &fontspec, CTRLRES_SETSTR),
		MK_RES("topShadowColor", "TopShadowColor", &widget->colors[COLOR_TOP], parse_color),
		MK_RES("bottomShadowColor", "BottomShadowColor", &widget->colors[COLOR_BOT], parse_color),
	};

	if (str != NULL)
		xdb = XrmGetStringDatabase(str);
	ctrlres_parsexrdb(
		widget,
		(XrmDatabase[]){widget->cli_xrm, xdb}, 2,
		resources, LEN(resources)
	);
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

static size_t
fill_entries(struct item *entries, size_t menu_indentlvl, char *data, size_t datasize, size_t *from)
{
	size_t nentries;
	size_t linelen;
	size_t maxentries = 0;
	struct item *prev_entry = NULL;

	for (size_t len = *from; len < datasize; len++) {
		size_t entry_indentlvl;

		entry_indentlvl = strspn(data+len, "\t");
		len += strcspn(data+len, "\n");
		if (entry_indentlvl < menu_indentlvl)
			break;
		if (entry_indentlvl == menu_indentlvl)
			maxentries++;
	}

	linelen = 0;
	nentries = 0;
	while (*from < datasize) {
		struct item *entry = &entries[nentries];
		size_t entry_indentlvl;
		char *line = data + *from;

		linelen = strcspn(line, "\n");
		entry_indentlvl = strspn(line, "\t");
		if (entry_indentlvl < menu_indentlvl) {
			break;
		} else if (entry_indentlvl > menu_indentlvl) {
			assert(prev_entry != NULL);
			prev_entry->children = entries+maxentries,
			prev_entry->nchildren = fill_entries(
				prev_entry->children,
				entry_indentlvl,
				data, datasize, from
			);
		} else {
			line[linelen] = '\0';
			*from += linelen + 1;
			entry->label = line + entry_indentlvl;
			if (strncmp(entry->label, "IMG:", 4) == 0) {
				size_t pathlen;
				entry->file = entry->label + 4;
				pathlen = strcspn(entry->file, "\t");
				entry->label = entry->file + pathlen;
				entry->label += strspn(entry->label, "\t");
				entry->file[pathlen] = '\0';
			}
			entry->labellen = strcspn(entry->label, "\t");
			entry->output = entry->label + entry->labellen;
			entry->output += strspn(entry->output, "\t");
			entry->label[entry->labellen] = '\0';

			if (*entry->label == '\0')
				entry->label = NULL;
			if (*entry->output == '\0' || strcmp(entry->output, ":") == 0)
				entry->output = entry->label;
			if (prev_entry != NULL && prev_entry->label != NULL &&
			    entry->label != NULL && strcmp(entry->label, "''") == 0) {
				prev_entry->altoutput = entry->output;
				continue;
			}
			prev_entry = entry;
			nentries++;
		}
	}
	return nentries;
}

static size_t
parsestdin(struct item **entries, char **data)
{
	size_t nlines;
	FILE *stream;
	char buf[BUFSIZ];
	size_t size;
	ssize_t nread;

	if ((stream = open_memstream(data, &size)) == NULL)
		err(EXIT_FAILURE, "open_memstream");
	while ((nread = read(STDIN_FILENO, buf, sizeof(buf))) != 0) {
		if (nread < 0)
			err(EXIT_FAILURE, "read");
		if (fwrite(buf, 1, nread, stream) != (size_t)nread)
			err(EXIT_FAILURE, "fwrite");
	}
	fflush(stream);
	if (size == 0)
		return 0;
	if ((*data)[size-1] != '\n' && (*data)[size-1] != '\0')
		if (fwrite("\0", 1, 1, stream) != 1)
			err(EXIT_FAILURE, "fwrite");
	(*data)[size-1] = '\0';
	if (fclose(stream) == EOF)
			err(EXIT_FAILURE, "fclose");

	nlines = 0;
	for (size_t len = 0; len < size; len += strcspn((*data)+len, "\n") + 1)
		nlines++;
	if ((*entries = calloc(nlines, sizeof(**entries))) == NULL)
		err(EXIT_FAILURE, "calloc");

	return fill_entries(*entries, strspn(*data, "\t"), *data, size, &(size_t){0});
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
	for (int i = 0; i < widget->inner_border_width; i++) {
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

	x = widget->inner_border_width + PADDING;
	y += widget->separatorh / 2;
	w = 0;
	maxw = width - widget->inner_border_width * 2 - PADDING * 2;
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
		widget->inner_border_width + PADDING,
		rect->y + widget->separatorh / 2 - 1,
		rect->width - widget->inner_border_width * 2 - PADDING * 2,
		1
	);
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		picture,
		&widget->colors[COLOR_TOP].chans,
		widget->inner_border_width + PADDING,
		rect->y + widget->separatorh / 2,
		rect->width - widget->inner_border_width * 2 - PADDING * 2,
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

static int
firstitempos(struct widget *widget, struct menu *menu)
{
	int y;

	y = widget->inner_border_width;
	if (menu->tearable)
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
	imlib_context_set_blend(1);
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
	//if (item->output != NULL && item->output[0] == '/')
	//	return true;
	return false;
}

static void
drawmenu(struct widget *widget, struct menu *menu)
{
	XRectangle item_geometry;
	int textx, textw, iconw, iconh, y;

	if (menu->canvas_width  < menu->window_geometry.width &&
	    menu->canvas_height < menu->window_geometry.height) {
		menu->canvas_width = menu->window_geometry.width + 128;
		menu->canvas_height = MAX(
			menu->window_geometry.height + 128,
			menu->total_height
		);
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
				menu->canvas_width,
				menu->canvas_height,
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
				menu->canvas_width,
				menu->canvas_height
			);
		}
	}
	for (size_t i = 0; i < CANVAS_LAST; i++) {
		XRenderComposite(
			widget->display,
			PictOpSrc,
			widget->colors[i==CANVAS_SELECT ?  COLOR_SELBG : COLOR_BG].pict,
			widget->colors[COLOR_OPACITY].pict,
			menu->canvas[i].picture,
			0, 0,
			0, 0,
			widget->inner_border_width, widget->inner_border_width,
			menu->window_geometry.width - widget->inner_border_width * 2,
			menu->total_height - widget->inner_border_width * 2
		);
	}
	item_geometry.x = widget->inner_border_width + PADDING;
	if (menu->hasicon)
		item_geometry.x += widget->iconsize + PADDING;
	item_geometry.y = firstitempos(widget, menu);
	item_geometry.width = menu->window_geometry.width;
	for (size_t itemc = 0; itemc < menu->nitems; itemc++) {
		struct item *item = &menu->items[itemc];
		Imlib_Image image;

		if (item->label == NULL) {
			item_geometry.height = widget->separatorh;
			drawseparator(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				&item_geometry
			);
			drawseparator(
				widget,
				menu->canvas[CANVAS_SELECT].picture,
				&item_geometry
			);
			goto next;
		}
		item_geometry.height = widget->itemh;
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
				menu->canvas[CANVAS_NORMAL].pixmap
			);
			imlib_render_image_on_drawable(
				widget->inner_border_width + PADDING
				+ (widget->iconsize - iconw) / 2,
				item_geometry.y + (widget->itemh - iconh) / 2
			);
			imlib_context_set_drawable(
				menu->canvas[CANVAS_SELECT].pixmap
			);
			imlib_render_image_on_drawable(
				widget->inner_border_width + PADDING
				+ (widget->iconsize - iconw) / 2,
				item_geometry.y + (widget->itemh - iconh) / 2
			);
			imlib_free_image();
		}
		if (openssubmenu(item)) {
			drawtriangle(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_FG].pict,
				item_geometry.width - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD/2,
				item_geometry.y + widget->itemh/2 - TRIANGLE_HEIGHT/2,
				DIR_RIGHT
			);
			drawtriangle(
				widget,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_SELFG].pict,
				item_geometry.width - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD/2,
				item_geometry.y + widget->itemh/2 - TRIANGLE_HEIGHT/2,
				DIR_RIGHT
			);
		}
		textw = ctrlfnt_width(
			widget->fontset,
			item->label,
			item->labellen
		);
		if (widget->alignment == ALIGN_RIGHT && menu->hassubmenu)
			textx = item_geometry.width - textw - PADDING - TRIANGLE_WIDTH - TRIANGLE_PAD;
		else if (widget->alignment == ALIGN_RIGHT)
			textx = item_geometry.width - textw - PADDING;
		else if (widget->alignment == ALIGN_CENTER)
			textx = item_geometry.x + (menu->window_geometry.width - textw) / 2;
		else
			textx = item_geometry.x;
		if (item->output != NULL) {
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_FG].pict,
				(XRectangle){
					.x = textx,
					.y = item_geometry.y,
					.width = item_geometry.width,
					.height = item_geometry.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_SELFG].pict,
				(XRectangle){
					.x = textx,
					.y = item_geometry.y,
					.width = item_geometry.width,
					.height = item_geometry.height,
				},
				item->label,
				item->labellen
			);
		} else {
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = item_geometry.y + 1,
					.width = item_geometry.width,
					.height = item_geometry.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = item_geometry.y,
					.width = item_geometry.width,
					.height = item_geometry.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = item_geometry.y + 1,
					.width = item_geometry.width,
					.height = item_geometry.height,
				},
				item->label,
				item->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = item_geometry.y,
					.width = item_geometry.width,
					.height = item_geometry.height,
				},
				item->label,
				item->labellen
			);
		}
next:
		item_geometry.y += item_geometry.height;
	}
	for (size_t i = 0; i < CANVAS_FINAL; i++) {
		y = widget->inner_border_width;
		if (menu->tearable) {
			drawdashline(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				menu->window_geometry.width, y
			);
			drawdashline(
				widget,
				menu->canvas[CANVAS_SELECT].picture,
				menu->window_geometry.width, y
			);
			y += widget->separatorh;
		}
		if (menu->overflow) {
			drawtriangle(
				widget,
				menu->canvas[i].picture,
				widget->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->window_geometry.width / 2 - TRIANGLE_HEIGHT / 2,
				y + widget->separatorh /2 - TRIANGLE_WIDTH / 2,
				DIR_UP
			);
			drawtriangle(
				widget,
				menu->canvas[i].picture,
				widget->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->window_geometry.width / 2 - TRIANGLE_HEIGHT / 2,
				menu->total_height - widget->separatorh /2
				- TRIANGLE_WIDTH / 2,
				DIR_DOWN
			);
		}
	}
	XSetWindowBackgroundPixmap(
		widget->display,
		menu->window,
		menu->canvas[CANVAS_NORMAL].pixmap
	);
	XSetWindowBorderPixmap(widget->display, menu->window, widget->colors[COLOR_BORDER].pix);
	XClearWindow(widget->display, menu->window);
}

static int
get_item_ypos(struct widget *widget, struct menu *menu, int selected)
{
	int ypos = widget->inner_border_width;

	if (menu->tearable) {
		if (selected == TEAROFF)
			return ypos;
		ypos += widget->separatorh;
	}
	if (menu->overflow) {
		if (selected == SCROLLDOWN)
			return menu->total_height - widget->separatorh
			- widget->inner_border_width;
		if (selected == SCROLLUP)
			return ypos;
		ypos += widget->separatorh;
	}
	if (selected < 0 || selected >= (ssize_t)menu->nitems)
		return -1;
	for (size_t itemc = 0; itemc < menu->nitems; itemc++) {
		struct item *item = &menu->items[itemc];

		if ((ssize_t)itemc == selected)
			return ypos;
		if (item->label != NULL)
			ypos += widget->itemh;
		else
			ypos += widget->separatorh;
	}
	return -1;
}

static int
get_item_height(struct widget *widget, struct menu *menu, int selected)
{
	if (menu->tearable && selected == TEAROFF)
		return widget->separatorh;
	if (menu->overflow && (selected == SCROLLDOWN || selected == SCROLLUP))
		return widget->separatorh;
	if (selected < 0 || selected >= (ssize_t)menu->nitems)
		return 0;
	if (menu->items[selected].label != NULL)
		return widget->itemh;
	else
		return widget->separatorh;
}

static void
commitdraw(struct widget *widget, struct menu *menu)
{
	int src_canvas_y, dst_canvas_y;

	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		menu->canvas[CANVAS_FINAL].picture,
		&(XRenderColor){ 0 },
		0, 0,
		menu->window_geometry.width,
		menu->window_geometry.height
	);
	XRenderComposite(
		widget->display,
		PictOpOver,
		menu->canvas[CANVAS_NORMAL].picture,
		None,
		menu->canvas[CANVAS_FINAL].picture,
		0, menu->curr_screenful.offset,
		0, 0,
		0, 0,
		menu->window_geometry.width,
		menu->window_geometry.height
	);
	if (menu->tearable) {
		XRenderComposite(
			widget->display,
			PictOpOver,
			menu->canvas[CANVAS_NORMAL].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, get_item_ypos(widget, menu, TEAROFF),
			0, 0,
			0, get_item_ypos(widget, menu, TEAROFF),
			menu->window_geometry.width,
			get_item_height(widget, menu, TEAROFF)
		);
	}
	if (menu->overflow) {
		XRenderComposite(
			widget->display,
			PictOpOver,
			menu->canvas[CANVAS_NORMAL].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, get_item_ypos(widget, menu, SCROLLUP),
			0, 0,
			0, get_item_ypos(widget, menu, SCROLLUP),
			menu->window_geometry.width,
			get_item_height(widget, menu, SCROLLUP)
		);
		XRenderComposite(
			widget->display,
			PictOpOver,
			menu->canvas[CANVAS_NORMAL].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, get_item_ypos(widget, menu, SCROLLDOWN),
			0, 0,
			0, menu->window_geometry.height
			- get_item_height(widget, menu, SCROLLDOWN)
			- widget->inner_border_width,
			menu->window_geometry.width,
			get_item_height(widget, menu, SCROLLDOWN)
		);
	}
	src_canvas_y = get_item_ypos(widget, menu, menu->selected);
	if (menu->selected == SCROLLDOWN)
		dst_canvas_y = menu->window_geometry.height
		             - widget->separatorh - widget->inner_border_width;
	else if (menu->selected == SCROLLUP || menu->selected == TEAROFF)
		dst_canvas_y = src_canvas_y;
	else
		dst_canvas_y = src_canvas_y - menu->curr_screenful.offset;
	if (src_canvas_y >= 0) {
		XRenderComposite(
			widget->display,
			PictOpOver,
			menu->canvas[CANVAS_SELECT].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, src_canvas_y,
			0, 0,
			0, dst_canvas_y,
			menu->window_geometry.width,
			get_item_height(widget, menu, menu->selected)
		);
	}
	drawshadows(
		widget,
		menu->canvas[CANVAS_FINAL].picture,
		&menu->window_geometry
	);
	XSetWindowBackgroundPixmap(
		widget->display,
		menu->window,
		menu->canvas[CANVAS_FINAL].pixmap
	);
	XClearWindow(widget->display, menu->window);
}

static void
selfirst(struct widget *widget, struct menu *menu)
{
	menu->curr_screenful.first_item = 0;
	menu->curr_screenful.offset = 0;
	for (ssize_t itemc = 0; itemc < (ssize_t)menu->nitems; itemc++) {
		struct item *item = &menu->items[itemc];
		if (item->output != NULL) {
			menu->selected = itemc;
			commitdraw(widget, menu);
			return;
		}
	}
}

static void
sellast(struct widget *widget, struct menu *menu)
{
	menu->curr_screenful = menu->last_screenful;
	for (ssize_t itemc = menu->nitems - 1; itemc >= 0; itemc--) {
		struct item *item = &menu->items[itemc];
		if (item->output != NULL) {
			menu->selected = itemc;
			commitdraw(widget, menu);
			return;
		}
	}
}

static void
scrollup(struct widget *widget, struct menu *menu, int diff)
{
	int height = 0;

	while (height < diff && menu->curr_screenful.first_item > 0) {
		height = get_item_height(widget, menu, menu->curr_screenful.first_item);
		menu->curr_screenful.first_item--;
		menu->curr_screenful.offset -= height;
	}
}

static void
scrolldown(struct widget *widget, struct menu *menu, int diff)
{
	int height = 0;

	if (menu->curr_screenful.offset + height >= menu->last_screenful.offset) {
		menu->curr_screenful = menu->last_screenful;
		return;
	}
	while (height < diff && menu->curr_screenful.first_item + 1 < menu->nitems) {
		height = get_item_height(widget, menu, menu->curr_screenful.first_item);
		menu->curr_screenful.first_item++;
		menu->curr_screenful.offset += height;
	}
}

static void
selprev(struct widget *widget, struct menu *menu)
{
	int ypos, height;
	int scroll_threshold;

	if (menu->selected < 0) {
		sellast(widget, menu);
		return;
	}
	scroll_threshold = widget->inner_border_width + get_item_height(widget, menu, SCROLLUP);
	if (menu->tearable)
		scroll_threshold += get_item_height(widget, menu, TEAROFF);
	ypos = get_item_ypos(widget, menu, menu->selected);
	height = get_item_height(widget, menu, menu->selected);
	for (ssize_t itemc = menu->selected - 1; itemc >= 0; itemc--) {
		struct item *item = &menu->items[itemc];

		if (menu->overflow) {
			ypos -= height;
			height = get_item_height(widget, menu, itemc);
			if (ypos - menu->curr_screenful.offset < scroll_threshold) {
				scrollup(widget, menu, height);
			}
		}
		if (item->output != NULL) {
			menu->selected = itemc;
			commitdraw(widget, menu);
			return;
		}
	}
	sellast(widget, menu);
}

static void
selnext(struct widget *widget, struct menu *menu)
{
	int ypos, height;
	int scroll_threshold = menu->window_geometry.height
	                     - widget->separatorh - widget->inner_border_width;

	if (menu->selected < 0) {
		selfirst(widget, menu);
		return;
	}
	ypos = get_item_ypos(widget, menu, menu->selected);
	height = get_item_height(widget, menu, menu->selected);
	for (ssize_t itemc = menu->selected+1; itemc < (ssize_t)menu->nitems; itemc++) {
		struct item *item = &menu->items[itemc];

		if (menu->overflow) {
			ypos += height;
			height = get_item_height(widget, menu, itemc);
			if (ypos - menu->curr_screenful.offset > scroll_threshold) {
				scrolldown(widget, menu, height);
			}
		}
		if (item->output != NULL) {
			menu->selected = itemc;
			commitdraw(widget, menu);
			return;
		}
	}
	selfirst(widget, menu);
}

static void
popupmenu(struct widget *widget, struct item *items, size_t nitems,
		XRectangle *basis, bool isroot, char const *title)
{
	XRectangle *monitor = &widget->monitor;
	struct menu *menu;
	Atom type;
	unsigned int textw;
	int max_height = monitor->height - widget->outer_border_width * 2;
	int xgap, ygap;
	bool override_redirect, tearoff;

	if ((menu = malloc(sizeof(*menu))) == NULL) {
		warn("malloc");
		return;
	}
	*menu = (struct menu){
		.items = items,
		.nitems = nitems,
		.title = title,
		.selected = -1,
		.curr_screenful = {0, 0},
		.last_screenful = {0, 0},
		.next = widget->menus,
		.tearable = (widget->menus != NULL || !options.windowed) && widget->tearoff,
	};
	type = XInternAtom(widget->display, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
	override_redirect = true;
	tearoff = widget->tearoff;
	if (widget->menus != NULL) {
		xgap = widget->gap + widget->outer_border_width * 2;
		ygap = -widget->inner_border_width;
		if (tearoff)
			ygap -= widget->separatorh;
		(void)grabpointer(widget);
		(void)grabkeyboard(widget);
	} else {
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
	menu->total_height = widget->inner_border_width * 2;
	if (tearoff)
		menu->total_height += widget->separatorh;
	menu->window_geometry.height = widget->inner_border_width * 2;
	menu->nicons = 0;
	for (size_t itemc = 0; itemc < menu->nitems; itemc++) {
		struct item *item = &menu->items[itemc];

		if (item->label != NULL) {
			menu->total_height += widget->itemh;
			textw = ctrlfnt_width(
				widget->fontset,
				item->label,
				item->labellen
			);
		} else {
			textw = 0;
			menu->total_height += widget->separatorh;
		}
		if (item->file != NULL) {
			menu->hasicon = true;
			menu->nicons++;
		}
		if (openssubmenu(item))
			menu->hassubmenu = true;
		menu->window_geometry.width = MAX(menu->window_geometry.width, textw);
		if (menu->overflow) {
			menu->last_screenful.offset += get_item_height(
				widget, menu, menu->last_screenful.first_item
			);
			menu->last_screenful.first_item++;
			continue;
		}
		if (menu->total_height + widget->itemh <= max_height) {
			menu->window_geometry.height = menu->total_height;
		} else {
			menu->overflow = true;
			menu->window_geometry.height = max_height;
		}
	}
	if (menu->overflow)
		menu->total_height += widget->separatorh * 2;

	menu->window_geometry.width += PADDING * 2 + widget->inner_border_width * 2;
	if (menu->hasicon)
		menu->window_geometry.width += widget->iconsize + PADDING;
	if (menu->hassubmenu)
		menu->window_geometry.width += TRIANGLE_WIDTH + TRIANGLE_PAD + PADDING;
	menu->window_geometry.width = MIN(menu->window_geometry.width, monitor->width/2);

	/* place menu to align with parent menu and be visible on monitor */
	if (menu->overflow && widget->menus != NULL)
		ygap -= widget->separatorh;
	menu->window_geometry.x = monitor->x;
	menu->window_geometry.y = monitor->y;
	if (monitor->x + monitor->width - (basis->x + basis->width + xgap)
	    >= menu->window_geometry.width) {
		menu->window_geometry.x = basis->x + basis->width + xgap;
	} else if (basis->x > (long)menu->window_geometry.width + xgap) {
		menu->window_geometry.x = basis->x - menu->window_geometry.width - xgap;
	}
	if (monitor->y + max_height - (basis->y + ygap)
	    >= menu->window_geometry.height) {
		menu->window_geometry.y = basis->y + ygap;
	} else if (monitor->y + max_height > menu->window_geometry.height + ygap) {
		menu->window_geometry.y = monitor->y + max_height;
		menu->window_geometry.y -= menu->window_geometry.height;
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
			menu->window_geometry.x = screen.x + screen.width * x / 100;
			menu->window_geometry.x -= menu->window_geometry.width / 2;
		} else if (options.xneg) {
			menu->window_geometry.x = screen.x + screen.width - x - menu->window_geometry.width;
		} else {
			menu->window_geometry.x = screen.x + x;
		}

		if (options.ypercent) {
			if (options.yneg) y = 100 - y;
			menu->window_geometry.y = screen.y + screen.height * y / 100;
			menu->window_geometry.y -= menu->window_geometry.height / 2;
		} else if (options.yneg) {
			menu->window_geometry.y = screen.y + screen.height - y - menu->window_geometry.height;
		} else {
			menu->window_geometry.y = screen.y + y;
		}
	}

	menu->window = createwindow(
		widget, &menu->window_geometry,
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
		title, title,
		widget->argv,
		widget->argc,
		&(XSizeHints){ .flags = USPosition|USSize },
		NULL,
		&(XClassHint){
			.res_class = widget->class,
			.res_name = widget->name,
		}
	);
	(void)XChangeProperty(
		widget->display,
		menu->window,
		XInternAtom(widget->display, "_NET_WM_NAME", False),
		XInternAtom(widget->display, "UTF8_STRING", False),
		8,
		PropModeReplace,
		(unsigned char *)title,
		strlen(title)
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
	commitdraw(widget, menu);
	XMapRaised(widget->display, menu->window);
}

static void
delmenu(struct widget *widget)
{
	struct menu *menu;

	if ((menu = widget->menus) == NULL)
		return;
	//if (menu->directory)
	//	cleanitems(menu->items, NULL);
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

static size_t
index_item(struct widget *widget, struct menu *menu, long y)
{
	long h;

	h = widget->inner_border_width;
	if (menu->tearable) {
		if (y < h + widget->separatorh)
			return TEAROFF;
		h += widget->separatorh;
	}
	if (menu->overflow && y < h + widget->separatorh) {
		return SCROLLUP;
	}
	if (menu->overflow && y >= menu->window_geometry.height - widget->separatorh) {
		return SCROLLDOWN;
	}
	if (menu->overflow)
		h += widget->separatorh;
	for (size_t itemc = menu->curr_screenful.first_item; itemc < menu->nitems; itemc++) {
		struct item *item = &menu->items[itemc];
		if (item->label == NULL) {
			h += widget->separatorh;
			continue;
		}
		if (item->output == NULL) {
			h += widget->itemh;
			continue;
		}
		if (y >= h && y < (long)h + widget->itemh)
			return itemc;
		h += widget->itemh;
		if (menu->overflow &&
		    h + widget->itemh + widget->inner_border_width >=
		    menu->window_geometry.height) {
			break;
		}
	}
	return -1;
}

static struct item *
get_item(struct menu *menu, ssize_t itemc)
{
	if (itemc >= 0 && itemc < (ssize_t)menu->nitems)
		return &menu->items[itemc];
	return NULL;
}

static void
scroll(struct widget *widget, struct menu *menu, bool down)
{
	static struct timespec last_time = {};
	struct timespec ts;
	time_t elapsed;
	int ret;
	int last_offset = menu->curr_screenful.offset;
	struct pollfd pfd = {
		.fd = XConnectionNumber(widget->display),
		.events = POLLIN,
	};

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
			if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
				warn("clock_gettime");
				return;
			}
			elapsed = (ts.tv_sec - last_time.tv_sec) * 1000;
			elapsed += ts.tv_nsec / 1000000;
			elapsed -= last_time.tv_nsec / 1000000;
			if (elapsed < SCROLL_TIME)
				return;
			if (clock_gettime(CLOCK_MONOTONIC, &last_time) == -1) {
				warn("clock_gettime");
				return;
			}
			/* fallthrough */
		case 0:
			if (down)
				scrolldown(widget, menu, widget->itemh);
			else
				scrollup(widget, menu, widget->itemh);
			if (menu->curr_screenful.offset != last_offset)
				commitdraw(widget, menu);
			last_offset = menu->curr_screenful.offset;
			XFlush(widget->display);
			break;
		}
	}
}

static void
forkandtearoff(struct widget *widget, struct menu *menu)
{
	pid_t pid;

	if ((pid = fork()) == -1) {
		warn("fork");
		return;
	}
	if (pid == 0) {
		/* child */
		ctrlfnt_free(widget->fontset);
		while (close(XConnectionNumber(widget->display)) == -1) {
			if (errno == EINTR)
				continue;
			err(EXIT_FAILURE, "close");
		}
		*widget = (struct widget){ 0 };
		widget->display = NULL;
		options.items = menu->items,
		options.monitor = -1,
		options.userplaced = true,
		options.xpercent = options.ypercent = false;
		options.windowed = true,
		options.rootmode = false,
		options.geometry.x = menu->window_geometry.x,
		options.geometry.y = menu->window_geometry.y,
		options.geometry.width = 0,
		options.geometry.height = 0,
		longjmp(jmpenv, 1);
		exit(EXIT_FAILURE);
	}
	closepopups(widget);
}

//static int
//direntsel(const struct dirent *dp)
//{
//	return dp->d_name[0] != '.' && strpbrk(dp->d_name, "\t\n") == NULL;
//}
//
//static int
//direntcmp(const struct dirent **ap, const struct dirent **bp)
//{
//	const struct dirent *a = *ap;
//	const struct dirent *b = *bp;
//
//	if (a->d_type == DT_DIR && b->d_type != DT_DIR)
//		return -1;
//	if (b->d_type == DT_DIR && a->d_type != DT_DIR)
//		return 1;
//	return strcoll(a->d_name, b->d_name);
//}
//
//static struct item *
//listdirentries(const char *dirname)
//{
//	struct dirent **namelist;
//	struct item *items, *item, *prev;
//	int nents;
//	char buf[PATH_MAX + 8];
//	
//	nents = scandir(dirname, &namelist, &direntsel, &direntcmp);
//	if (nents == -1)
//		return NULL;
//	items = NULL;
//	prev = NULL;
//	for (int i = 0; i < nents; i++) {
//		snprintf(
//			buf,
//			sizeof(buf),
//			"%s%s/%s",
//			namelist[i]->d_type == DT_DIR ? "" : "file:",
//			dirname,
//			namelist[i]->d_name
//		);
//		item = allocitem(namelist[i]->d_name, buf, NULL);
//		if (prev == NULL)
//			items = item;
//		else
//			prev->next = item;
//		item->prev = prev;
//		prev = item;
//		free(namelist[i]);
//	}
//	free(namelist);
//	return items;
//	return NULL;
//}

static void
printitem(struct widget *widget, const char *str)
{
	(void)printf("%s\n", str);
	(void)fflush(stdout);
	closepopups(widget);
}

static void
openitem(struct widget *widget, struct menu *menu, ssize_t nitem, bool alt)
{
	XRectangle rect;
	struct item *item = get_item(menu, nitem);

	if (item == NULL)
		return;
	rect = menu->window_geometry;
	rect.y += get_item_ypos(widget, menu, nitem);
	if (item->children != NULL) {
		popupmenu(widget, item->children, item->nchildren, &rect, false, item->label);
	} else if (alt && item->altoutput != NULL) {
		printitem(widget, item->altoutput);
	//} else if (item->output != NULL &&
	//           item->output[0] == '/') {
	//	if (alt) {
	//		snprintf(buf, PATH_MAX, "%s%s", "file:", item->output);
	//		printitem(widget, buf);
	//		return;
	//	}
	//	items = listdirentries(item->output);
	//	if (items == NULL)
	//		return;
	//	popupmenu(widget, items, &rect, false);
	//	widget->menus->directory = true;
	//	widget->menus->items = items;
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
		selnext(widget, menu);
		break;
	case XK_Home:
		selfirst(widget, menu);
		break;
	case XK_ISO_Left_Tab:
	case XK_Up:
		selprev(widget, menu);
		break;
	case XK_End:
		sellast(widget, menu);
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
		openitem(widget, menu, menu->selected, false);
		break;
	}
}

static int
run(struct widget *widget, XRectangle *geometry)
{
	XEvent xev;
	struct menu *menu;
	struct item *item;
	ssize_t nitem;

	getposition(widget, geometry);
	popupmenu(widget, options.items, options.nitems, geometry, true, widget->title);
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
		nitem = index_item(widget, menu, xev.xbutton.y);
		item = get_item(menu, nitem);
		if (item != NULL) {
			if (item->children != NULL) {
				openitem(widget, menu, nitem, false);
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
		nitem = index_item(widget, menu, xev.xbutton.y);
		if (nitem == TEAROFF) {
			forkandtearoff(widget, menu);
			continue;
		}
		item = get_item(menu, nitem);
		if (item == NULL)
			continue;
		if (item->children != NULL)
			continue;
		while (widget->menus != menu)
			delmenu(widget);
		openitem(widget, menu, nitem, xev.xbutton.button == Button2);
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
		menu->window_geometry.x = xev.xconfigure.x;
		menu->window_geometry.y = xev.xconfigure.y;
		if (menu->window_geometry.width  == xev.xconfigure.width &&
		    menu->window_geometry.height == xev.xconfigure.height)
			continue;       /* size not changed */
		menu->window_geometry.width = xev.xconfigure.width;
		menu->window_geometry.height = xev.xconfigure.height;
		drawmenu(widget, menu);
		if (get_item(menu, menu->selected) != NULL)
			commitdraw(widget, menu);
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
			widget->menus->selected = -1;
			commitdraw(widget, widget->menus);
		}
		continue;
	case MotionNotify:
		menu = getmenu(widget, xev.xmotion.window);
		if (menu == NULL)
			continue;
		nitem = index_item(widget, menu, xev.xbutton.y);
		if (nitem == SCROLLUP || nitem == SCROLLDOWN)
			scroll(widget, menu, nitem == SCROLLDOWN);
		if (nitem == menu->selected)
			continue;
		item = get_item(menu, nitem);
		menu->selected = nitem;
		commitdraw(widget, menu);
		if (item != NULL && xev.xmotion.state & (Button1Mask|Button3Mask)) {
			while (widget->menus != menu)
				delmenu(widget);
			if (openssubmenu(item)) {
				openitem(widget, menu, nitem, false);
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
				commitdraw(widget, menu);
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
	if (widget->colormap != None)
		XFreeColormap(widget->display, widget->colormap);
	if (widget->window != None)
		XDestroyWindow(widget->display, widget->window);
	free(widget);
}

static struct widget *
setup(Display *display, int screen, Visual *visual, Colormap colormap, int depth,
		XrmDatabase cli_xrm, char *title, char *name, char *class,
		int argc, char *argv[])
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
		.cli_xrm = cli_xrm,
		.window = None,
		.argc = argc,
		.argv = argv,
		.name = name,
		.class = class,
		.title = title,

		/* defaults [for color channels:  RED,  GREEN,   BLUE,  ALPHA] */
		.colors[COLOR_BG].chans     = {0x3100, 0x3100, 0x3100, 0xFFFF},
		.colors[COLOR_FG].chans     = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
		.colors[COLOR_SELBG].chans  = {0x3400, 0x6500, 0xA400, 0xFFFF},
		.colors[COLOR_SELFG].chans  = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
		.colors[COLOR_TOP].chans    = {0x7300, 0x7300, 0x7300, 0xFFFF},
		.colors[COLOR_BOT].chans    = {0x1000, 0x1000, 0x1000, 0xFFFF},
		.colors[COLOR_BORDER].chans = {0x0000, 0x0000, 0x0000, 0xFFFF},
		.colors[COLOR_OPACITY].chans = {.alpha = 0xFFFF},
		.outer_border_width = 1,
		.iconsize = 16,
		.gap = 0,
		.alignment = ALIGN_LEFT,
	};
	XSelectInput(widget->display, widget->rootwin, PropertyChangeMask);
	if ((widget->window = createwindow(widget, NULL, 0, True)) == None)
		goto error;

	for (size_t color = 0; color < COLOR_LAST; color++) {
		int retval;
		retval = createpicture(
			widget,
			&widget->colors[color].pict,
			&widget->colors[color].pix,
			&widget->colors[color].chans,
			false
		);
		if (retval == RETURN_FAILURE)
			goto error;
	}
	loadresources(widget, XResourceManagerString(widget->display));
	if (widget->fontset == NULL)
		setfont(widget, NULL, 0.0);
	if (widget->fontset == NULL)
		goto error;
	return widget;
error:
	cleanup(widget);
	return NULL;
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
	char *name = "xmenu";
	char *class = "XMenu";
	char *title = "XMenu";
	char *read_data = NULL;
	XrmDatabase cli_xrm = NULL;

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		err(EXIT_FAILURE, "sigaction");
	parseiconpaths(getenv("ICONPATH"));

	if (argv[0] != NULL && argv[0][0] != '\0') {
		name = strchr(argv[0], '/');
		if (name != NULL)
			name++;
		else
			name = argv[0];
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-windowed") || !strcmp(argv[i], "-w")) {
			options.windowed = true;
		} else if (!strcmp(argv[i], "-name") || !strcmp(argv[i], "-N")) {
			name = argv[++i];
		} else if (!strcmp(argv[i], "-leader")) {
			options.client = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-position") || !strcmp(argv[i], "-p")) {
			parsegeometry(argv[++i]);
		} else if (!strcmp(argv[i], "-rootgrab") || !strcmp(argv[i], "-x")) {
			char *arg = argv[++i];
			if (setbutton(arg))
				options.rootmode = true;
			setmodifier(arg);
		} else if (!strcmp(argv[i], "-title")) {
			title = argv[++i];
		} else if (!strcmp(argv[i], "-xrm")) {
			XrmDatabase db = XrmGetStringDatabase(argv[++i]);
			if (db != NULL)
				XrmCombineDatabase(db, &cli_xrm, True);
		} else {
			warnx("unknown option -- %s", argv[i]);
		usage:
			(void)fprintf(
				stderr,
				"usage: xmenu"
				" [-leader WINDOWID]"
				" [-name NAME]"
				" [-position POSITION]"
				" [-rootgrab BUTTON]"
				" [-title TITLE]"
				" [-windowed]"
				" [-xrm RESOURCES]"
				"\n"
			);
			exit(EXIT_FAILURE);
		}
		if (i == argc) {
			warnx("option requires an argument -- %s", argv[i-1]);
			goto usage;
		}
	}
	if (options.rootmode) {
		options.client = None;
		options.windowed = false;
	} else if (options.windowed) {
		options.rootmode = false;
	}
	options.nitems = parsestdin(&options.items, &read_data);
	if (options.nitems == 0)
		errx(EXIT_FAILURE, "no menu generated");
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
		display, DefaultScreen(display), visual, colormap, depth,
		cli_xrm, title, name, class, argc, argv
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
	cleanup(widget);
	free(options.iconstring);
	free(read_data);
	free(options.items);
	XrmDestroyDatabase(cli_xrm);
	if (display != NULL)
		XCloseDisplay(display);
	return EXIT_SUCCESS;
}
