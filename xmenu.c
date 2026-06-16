#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
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
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
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
#define MIN_HEIGHT              (16 + PADDING * 2)
#define TRIANGLE_HEIGHT(wid)    (ctrlfnt_height((wid)->fontset)/2)
#define TRIANGLE_WIDTH(wid)     (TRIANGLE_HEIGHT(wid)/2)
#define TRIANGLE_PAD(wid)       TRIANGLE_HEIGHT(wid)

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

struct entry {
	char *label;
	size_t labellen;

	char *output;
	char *altoutput;
	char *file;

	struct entry *children;
	size_t nchildren;
};

struct menu {
	struct menu *parent;
	struct entry *entries;
	size_t nentries;
	ssize_t selected;
	char const *title;
	XRectangle window_geometry;
	int canvas_width, canvas_height;
	int total_height;
	struct {
		size_t first_entry;
		int offset;
	} curr_screenful, last_screenful;
	Window window;
	struct Canvas {
		Pixmap pixmap;
		Picture picture;
	} canvas[CANVAS_LAST];
	bool tearable;
	bool overflow;
	bool hasicon;
	bool hassubmenu;
	bool tornoff;
};

struct widget {
	Display *display;
	Visual *visual;
	Colormap colormap;
	int depth;
	int screen;
	Window rootwin;
	Window window;
	Window leader;
	XrmDatabase cli_xrm;
	ctrlfnt *fontset;
	unsigned int fonth;
	int entryh, separatorh;
	int inner_border_width;  /* part of window content, for shadow/relief */
	int outer_border_width;  /* X11 border, not counted on window size */
	int iconsize;
	int maxentries;
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

/* unindexable indices for special entries */
static ssize_t TEAROFF    = -100;
static ssize_t SCROLLUP   = -200;
static ssize_t SCROLLDOWN = -300;

static int
parse_geometry(char *geometry_spec, int *x_root, int *y_root, char **monitor_name)
{
	char *str = geometry_spec;
	long n;
	int retval = XValue|YValue;

	if (str == NULL || *str == '\0') goto error;
	if (*str == '=') str++;

	/* size /([0-9]+[Xx][0-9]+)?/, compatibile with XParseGeometry(3); ignored */
	if (isdigit(*str)) {
		n = strtoul(str, &str, 10);
		if (n <= 0 || n > USHRT_MAX) goto error;

		if (*str != 'X' && *str != 'x') goto error;
		str++;

		if (!isdigit(*str)) goto error;
		n = strtoul(str, &str, 10);
		if (n <= 0 || n > USHRT_MAX) goto error;
		retval |= WidthValue|HeightValue;
	}

	/* x placement /[+-][0-9]+/; obligatory */
	if (*str != '+' && *str != '-') goto error;
	if (*(str++) == '-')
		retval |= XNegative;
	if (!isdigit(*str)) goto error;
	n = strtoul(str, &str, 10);
	if (n < 0 || n > SHRT_MAX) goto error;
	*x_root = n;

	/* y placement /[+-][0-9]+/; obligatory */
	if (*str != '+' && *str != '-') goto error;
	if (*(str++) == '-')
		retval |= YNegative;
	if (!isdigit(*str)) goto error;
	n = strtoul(str, &str, 10);
	if (n < 0 || n > SHRT_MAX) goto error;
	*y_root = n;

	/* monitor /([@:].*)?/; optional */
	*monitor_name = NULL;
	if (*str == '@' || *str == ':') {
		str++;
		if (*str == '\0')
			goto error;
		*monitor_name = str;
		return retval;
	}
	if (*str == '\0')
		return retval;
error:
	errx(EXIT_FAILURE, "invalid geometry specification -- %s", geometry_spec);
}

static void
parse_keychord(char *key_spec, unsigned int *modifier, unsigned int *button)
{
	static struct {
		unsigned int mask;
		char *names[2];
	} modifiers[] = {
		{ ShiftMask,    {"shift-", "s-"} },
		{ ControlMask,  {"ctrl-", "c-"} },
		{ Mod1Mask,     {"alt-", "a-"} },
		{ Mod4Mask,     {"super-", "w-"} },
	};
	size_t spn = 0;
	char *str = key_spec;
	long n;

	*modifier = 0;
	*button = 0;
	while (str[spn = strcspn(str, "-")] == '-') {
		for (size_t i = 0; i < LEN(modifiers); i++) {
			for (size_t j = 0; j < 2; j++) {
				size_t len = strlen(modifiers[i].names[j]);
				if (strncasecmp(str, modifiers[i].names[j], len) == 0) {
					*modifier |= modifiers[i].mask;
					goto next;
				}
			}
		}
		goto error;
	next:
		str += spn + 1;
	}
	if (str[0] == '\0')
		goto error;
	n = strtol(str, &str, 10);
	if (str[0] != '\0')
		goto error;
	if (n < 1 || n > 64)
		goto error;
	*button = n;
	return;
error:
	errx(EXIT_FAILURE, "%s: invalid keychord specification", key_spec);
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
	widget->entryh = widget->fonth + PADDING * 2;
	widget->entryh = MAX(widget->entryh, MIN_HEIGHT);
	widget->separatorh = widget->entryh / 2;
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
		MK_RES("maxItems", "MaxItems", &widget->maxentries, parse_width),
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
fill_entries(struct entry *entries, size_t menu_indentlvl, char *data, size_t datasize, size_t *from)
{
	size_t nentries;
	struct entry *next_entry = entries;
	struct entry *prev_entry = NULL;

	for (size_t len = *from; len < datasize; len++) {
		size_t entry_indentlvl;

		entry_indentlvl = strspn(data+len, "\t");
		len += strcspn(data+len, "\n");
		if (entry_indentlvl < menu_indentlvl)
			break;
		if (entry_indentlvl == menu_indentlvl)
			next_entry++;
	}

	nentries = 0;
	while (*from < datasize) {
		struct entry *entry = &entries[nentries];
		size_t entry_indentlvl;
		char *line = data + *from;
		size_t linelen = strcspn(line, "\n");

		entry_indentlvl = strspn(line, "\t");
		if (entry_indentlvl < menu_indentlvl) {
			break;
		} else if (entry_indentlvl > menu_indentlvl) {
			assert(prev_entry != NULL);
			prev_entry->output = NULL;
			prev_entry->children = next_entry;
			prev_entry->nchildren = fill_entries(
				prev_entry->children,
				entry_indentlvl,
				data, datasize, from
			);
			next_entry += prev_entry->nchildren;
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
			if (*entry->output == '\0')
				entry->output = entry->label;
			else if (strcmp(entry->output, ":") == 0)
				entry->output = NULL;
			entry->altoutput = entry->output;
			if (prev_entry != NULL && prev_entry->output != NULL &&
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
parsestdin(struct entry **entries, char **data)
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

static int
grabpointer(struct widget *widget)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	Cursor cursor;

	/* set cursor so it does not change to whatever the window below uses */
	cursor = XCreateFontCursor(widget->display, XC_left_ptr);

	/* we may have to wait for another process to ungrab */
	for (int i = 0; i < 1000; i++) {
		if (XGrabPointer(
			widget->display, widget->rootwin, True,
			ButtonPressMask|PointerMotionMask,
			GrabModeAsync, GrabModeAsync,
			None, cursor, CurrentTime
		) == GrabSuccess) {
			if (cursor != None)
				XFreeCursor(widget->display, cursor);
			return RETURN_SUCCESS;
		}
		nanosleep(&ts, NULL);
	}
	if (cursor != None)
		XFreeCursor(widget->display, cursor);
	warnx("cannot grab keyboard");
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
			XDoubleToFixed(y + TRIANGLE_WIDTH(widget))
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(widget) / 2),
			XDoubleToFixed(y)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(widget)),
			XDoubleToFixed(y + TRIANGLE_WIDTH(widget))
		};
	} else if (direction == DIR_DOWN) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(widget) / 2),
			XDoubleToFixed(y + TRIANGLE_WIDTH(widget))
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(widget)),
			XDoubleToFixed(y)
		};
	} else if (direction == DIR_LEFT) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_WIDTH(widget)),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x - TRIANGLE_WIDTH(widget)),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(widget) / 2)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(widget))
		};
	} else {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_WIDTH(widget)),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(widget) / 2)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(widget))
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
firstentrypos(struct widget *widget, struct menu *menu)
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

static Imlib_Image
loadicon(struct widget *widget, const char *file, int size, int *width_ret, int *height_ret)
{
	Imlib_Image icon = NULL;
	Imlib_Load_Error errcode;
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
	icon = imlib_load_image_with_error_return(file, &errcode);
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
openssubmenu(struct entry *entry)
{
	if (entry->children != NULL)
		return true;
	return false;
}

static void
drawmenu(struct widget *widget, struct menu *menu)
{
	XRectangle entry_geometry;
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
	entry_geometry.x = widget->inner_border_width + PADDING;
	if (menu->hasicon)
		entry_geometry.x += widget->iconsize + PADDING;
	entry_geometry.y = firstentrypos(widget, menu);
	entry_geometry.width = menu->window_geometry.width;
	for (size_t entryc = 0; entryc < menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];
		Imlib_Image image;

		if (entry->label == NULL) {
			entry_geometry.height = widget->separatorh;
			drawseparator(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				&entry_geometry
			);
			drawseparator(
				widget,
				menu->canvas[CANVAS_SELECT].picture,
				&entry_geometry
			);
			goto next;
		}
		entry_geometry.height = widget->entryh;
		image = loadicon(
			widget,
			entry->file,
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
				entry_geometry.y + (widget->entryh - iconh) / 2
			);
			imlib_context_set_drawable(
				menu->canvas[CANVAS_SELECT].pixmap
			);
			imlib_render_image_on_drawable(
				widget->inner_border_width + PADDING
				+ (widget->iconsize - iconw) / 2,
				entry_geometry.y + (widget->entryh - iconh) / 2
			);
			imlib_free_image();
		}
		if (openssubmenu(entry)) {
			drawtriangle(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_FG].pict,
				entry_geometry.width - PADDING - TRIANGLE_WIDTH(widget) - TRIANGLE_PAD(widget)/2,
				entry_geometry.y + widget->entryh/2 - TRIANGLE_HEIGHT(widget)/2,
				DIR_RIGHT
			);
			drawtriangle(
				widget,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_SELFG].pict,
				entry_geometry.width - PADDING - TRIANGLE_WIDTH(widget) - TRIANGLE_PAD(widget)/2,
				entry_geometry.y + widget->entryh/2 - TRIANGLE_HEIGHT(widget)/2,
				DIR_RIGHT
			);
		}
		textw = ctrlfnt_width(
			widget->fontset,
			entry->label,
			entry->labellen
		);
		if (widget->alignment == ALIGN_RIGHT && menu->hassubmenu)
			textx = entry_geometry.width - textw - PADDING - TRIANGLE_WIDTH(widget) - TRIANGLE_PAD(widget);
		else if (widget->alignment == ALIGN_RIGHT)
			textx = entry_geometry.width - textw - PADDING;
		else if (widget->alignment == ALIGN_CENTER)
			textx = entry_geometry.x + (menu->window_geometry.width - textw) / 2;
		else
			textx = entry_geometry.x;
		if (entry->output != NULL || entry->nchildren > 0) {
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_FG].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label,
				entry->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_SELFG].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label,
				entry->labellen
			);
		} else {
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = entry_geometry.y + 1,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label,
				entry->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label,
				entry->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = entry_geometry.y + 1,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label,
				entry->labellen
			);
			ctrlfnt_draw(
				widget->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				widget->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label,
				entry->labellen
			);
		}
next:
		entry_geometry.y += entry_geometry.height;
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
				menu->window_geometry.width / 2 - TRIANGLE_HEIGHT(widget) / 2,
				y + widget->separatorh /2 - TRIANGLE_WIDTH(widget) / 2,
				DIR_UP
			);
			drawtriangle(
				widget,
				menu->canvas[i].picture,
				widget->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->window_geometry.width / 2 - TRIANGLE_HEIGHT(widget) / 2,
				menu->total_height - widget->separatorh /2
				- TRIANGLE_WIDTH(widget) / 2,
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
get_entry_ypos(struct widget *widget, struct menu *menu, int selected)
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
	if (selected < 0 || selected >= (ssize_t)menu->nentries)
		return -1;
	for (size_t entryc = 0; entryc < menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];

		if ((ssize_t)entryc == selected)
			return ypos;
		if (entry->label != NULL)
			ypos += widget->entryh;
		else
			ypos += widget->separatorh;
	}
	return -1;
}

static int
get_entry_height(struct widget *widget, struct menu *menu, int selected)
{
	if (menu->tearable && selected == TEAROFF)
		return widget->separatorh;
	if (menu->overflow && (selected == SCROLLDOWN || selected == SCROLLUP))
		return widget->separatorh;
	if (selected < 0 || selected >= (ssize_t)menu->nentries)
		return 0;
	if (menu->entries[selected].label != NULL)
		return widget->entryh;
	else
		return widget->separatorh;
}

static void
commitdraw(struct widget *widget, struct menu *menu, ssize_t selected)
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
			0, get_entry_ypos(widget, menu, TEAROFF),
			0, 0,
			0, get_entry_ypos(widget, menu, TEAROFF),
			menu->window_geometry.width,
			get_entry_height(widget, menu, TEAROFF)
		);
	}
	if (menu->overflow) {
		XRenderComposite(
			widget->display,
			PictOpOver,
			menu->canvas[CANVAS_NORMAL].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, get_entry_ypos(widget, menu, SCROLLUP),
			0, 0,
			0, get_entry_ypos(widget, menu, SCROLLUP),
			menu->window_geometry.width,
			get_entry_height(widget, menu, SCROLLUP)
		);
		XRenderComposite(
			widget->display,
			PictOpOver,
			menu->canvas[CANVAS_NORMAL].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, get_entry_ypos(widget, menu, SCROLLDOWN),
			0, 0,
			0, menu->window_geometry.height
			- get_entry_height(widget, menu, SCROLLDOWN)
			- widget->inner_border_width,
			menu->window_geometry.width,
			get_entry_height(widget, menu, SCROLLDOWN)
		);
	}
	src_canvas_y = get_entry_ypos(widget, menu, selected);
	if (selected == SCROLLDOWN)
		dst_canvas_y = menu->window_geometry.height
		             - widget->separatorh - widget->inner_border_width;
	else if (selected == SCROLLUP || selected == TEAROFF)
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
			get_entry_height(widget, menu, selected)
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

static ssize_t
selfirst(struct widget *, struct menu *menu)
{
	menu->curr_screenful.first_entry = 0;
	menu->curr_screenful.offset = 0;
	for (ssize_t entryc = 0; entryc < (ssize_t)menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];
		if (entry->output != NULL || entry->nchildren > 0) {
			return entryc;
		}
	}
	return -1;
}

static ssize_t
sellast(struct widget *, struct menu *menu)
{
	menu->curr_screenful = menu->last_screenful;
	for (ssize_t entryc = menu->nentries - 1; entryc >= 0; entryc--) {
		struct entry *entry = &menu->entries[entryc];
		if (entry->output != NULL || entry->nchildren > 0) {
			return entryc;
		}
	}
	return -1;
}

static void
scrollup(struct widget *widget, struct menu *menu, int diff)
{
	int height = 0;

	while (height < diff && menu->curr_screenful.first_entry > 0) {
		height = get_entry_height(widget, menu, menu->curr_screenful.first_entry);
		menu->curr_screenful.first_entry--;
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
	while (height < diff && menu->curr_screenful.first_entry + 1 < menu->nentries) {
		height = get_entry_height(widget, menu, menu->curr_screenful.first_entry);
		menu->curr_screenful.first_entry++;
		menu->curr_screenful.offset += height;
	}
}

static ssize_t
selprev(struct widget *widget, struct menu *menu, ssize_t selected)
{
	int ypos, height;
	int scroll_threshold;

	if (selected < 0)
		return sellast(widget, menu);
	scroll_threshold = widget->inner_border_width + get_entry_height(widget, menu, SCROLLUP);
	if (menu->tearable)
		scroll_threshold += get_entry_height(widget, menu, TEAROFF);
	ypos = get_entry_ypos(widget, menu, selected);
	height = get_entry_height(widget, menu, selected);
	for (ssize_t entryc = selected - 1; entryc >= 0; entryc--) {
		struct entry *entry = &menu->entries[entryc];

		if (menu->overflow) {
			ypos -= height;
			height = get_entry_height(widget, menu, entryc);
			if (ypos - menu->curr_screenful.offset < scroll_threshold) {
				scrollup(widget, menu, height);
			}
		}
		if (entry->output != NULL || entry->nchildren > 0) {
			return entryc;
		}
	}
	return sellast(widget, menu);
}

static ssize_t
selnext(struct widget *widget, struct menu *menu, ssize_t selected)
{
	int ypos, height;
	int scroll_threshold = menu->window_geometry.height
	                     - widget->separatorh - widget->inner_border_width;

	if (selected < 0)
		return selfirst(widget, menu);
	ypos = get_entry_ypos(widget, menu, selected);
	height = get_entry_height(widget, menu, selected);
	for (ssize_t entryc = selected+1; entryc < (ssize_t)menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];

		if (menu->overflow) {
			ypos += height;
			height = get_entry_height(widget, menu, entryc);
			if (ypos - menu->curr_screenful.offset > scroll_threshold) {
				scrolldown(widget, menu, height);
			}
		}
		if (entry->output != NULL || entry->nchildren > 0) {
			return entryc;
		}
	}
	return selfirst(widget, menu);
}

static struct menu *
menu_new(struct widget *widget, struct menu *parent,
		struct entry *entries, size_t nentries,
		int x_root, int y_root, unsigned int button_width,
		bool windowed, char const *title)
{
	struct menu *menu;
	int max_height;
	int xgap, ygap;
	XRectangle monitor;
	XRRMonitorInfo *infos;
	int nmons = 0;

	monitor.x = 0;
	monitor.y = 0;
	monitor.width = DisplayWidth(widget->display, widget->screen);
	monitor.height = DisplayHeight(widget->display, widget->screen);
	infos = XRRGetMonitors(widget->display, widget->rootwin, True, &nmons);
	for (int i = 0; i < nmons && infos != NULL; i++) {
		if (x_root < infos[i].x)
			continue;
		if (x_root >= infos[i].x + infos[i].width)
			continue;
		if (y_root < infos[i].y)
			continue;
		if (y_root >= infos[i].y + infos[i].height)
			continue;
		monitor.x = infos[i].x;
		monitor.y = infos[i].y;
		monitor.width = infos[i].width;
		monitor.height = infos[i].height;
		break;
	}
	XRRFreeMonitors(infos);
	max_height = monitor.height - widget->outer_border_width * 2;

	/*
	 * If popup position is not a button, make an initial displacement
	 * to avoid pointer stay above an entry when menu pops up.
	 */
	if (button_width == 0) {
		if (x_root + (int)button_width + 1 < monitor.x + monitor.width)
			button_width = 1;
		if (x_root > monitor.x)
			x_root--;
	}

	if ((menu = malloc(sizeof(*menu))) == NULL)
		return NULL;
	*menu = (struct menu){
		.entries = entries,
		.nentries = nentries,
		.title = title,
		.selected = -1,
		.curr_screenful = {0, 0},
		.last_screenful = {0, 0},
		.parent = parent,
		.tearable = !windowed && widget->tearoff,
		.tornoff = windowed,
	};
	xgap = widget->outer_border_width * 2;
	ygap = -widget->inner_border_width;
	if (menu->tearable)
		ygap -= widget->separatorh;
	menu->total_height = widget->inner_border_width * 2;
	if (menu->tearable)
		menu->total_height += widget->separatorh;
	menu->window_geometry.height = widget->inner_border_width * 2;
	for (size_t entryc = 0; entryc < menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];
		unsigned int textw;

		if (entry->label != NULL) {
			menu->total_height += widget->entryh;
			textw = ctrlfnt_width(
				widget->fontset,
				entry->label,
				entry->labellen
			);
		} else {
			textw = 0;
			menu->total_height += widget->separatorh;
		}
		if (entry->file != NULL)
			menu->hasicon = true;
		if (openssubmenu(entry))
			menu->hassubmenu = true;
		menu->window_geometry.width = MAX(menu->window_geometry.width, textw);
		if (menu->overflow) {
			menu->last_screenful.offset += get_entry_height(
				widget, menu, menu->last_screenful.first_entry
			);
			menu->last_screenful.first_entry++;
			continue;
		}
		if (menu->total_height + widget->entryh <= max_height) {
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
		menu->window_geometry.width += TRIANGLE_WIDTH(widget) + TRIANGLE_PAD(widget) + PADDING;
	menu->window_geometry.width = MIN(menu->window_geometry.width, monitor.width/2);

	/* place menu to align with parent menu and be visible on monitor */
	if (menu->overflow && menu->parent != NULL)
		ygap -= widget->separatorh;
	menu->window_geometry.x = monitor.x;
	menu->window_geometry.y = monitor.y;
	if (monitor.x + monitor.width - (x_root + button_width + xgap)
	    >= menu->window_geometry.width) {
		menu->window_geometry.x = x_root + button_width + xgap;
	} else if (x_root > (long)menu->window_geometry.width + xgap) {
		menu->window_geometry.x = x_root - menu->window_geometry.width - xgap;
	}
	if (monitor.y + max_height - (y_root + ygap)
	    >= menu->window_geometry.height) {
		menu->window_geometry.y = y_root + ygap;
	} else if (monitor.y + max_height > menu->window_geometry.height + ygap) {
		menu->window_geometry.y = monitor.y + max_height;
		menu->window_geometry.y -= menu->window_geometry.height;
	}

	menu->window = createwindow(
		widget, &menu->window_geometry,
		KeyPressMask | StructureNotifyMask |
		EnterWindowMask | LeaveWindowMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		menu->tornoff ? false : true
	);
	if (widget->leader != None) {
		(void)XSetTransientForHint(
			widget->display,
			menu->window,
			widget->leader
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
		(void *)(Atom[]){XInternAtom(
			widget->display,
			menu->tornoff
			? "_NET_WM_WINDOW_TYPE_MENU"
			: "_NET_WM_WINDOW_TYPE_POPUP_MENU",
			False
		)}, 1
	);

	drawmenu(widget, menu);
	commitdraw(widget, menu, -1);
	XMapRaised(widget->display, menu->window);
	return menu;
}

static void
menu_free(struct widget *widget, struct menu *menu)
{
	if (menu == NULL)
		return;
	XDestroyWindow(widget->display, menu->window);
	for (size_t i = 0; i < CANVAS_LAST; i++) {
		XRenderFreePicture(widget->display, menu->canvas[i].picture);
		XFreePixmap(widget->display, menu->canvas[i].pixmap);
	}
	free(menu);
}

static void
ungrab(struct widget *widget)
{
	XUngrabPointer(widget->display, CurrentTime);
	XUngrabKeyboard(widget->display, CurrentTime);
}

static ssize_t
index_entry(struct widget *widget, struct menu *menu, long y)
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
	for (size_t entryc = menu->curr_screenful.first_entry; entryc < menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];
		if (entry->label == NULL) {
			h += widget->separatorh;
			continue;
		}
		if (entry->output == NULL && entry->nchildren == 0) {
			h += widget->entryh;
			continue;
		}
		if (y >= h && y < (long)h + widget->entryh)
			return entryc;
		h += widget->entryh;
		if (menu->overflow &&
		    h + widget->entryh + widget->inner_border_width >=
		    menu->window_geometry.height) {
			break;
		}
	}
	return -1;
}

static struct entry *
get_entry(struct menu *menu, ssize_t entryc)
{
	if (entryc >= 0 && entryc < (ssize_t)menu->nentries)
		return &menu->entries[entryc];
	return NULL;
}

static int
timed_poll(int fd, int timeout)
{
	struct timespec then, now;
	int elapsed;

	if (timeout <= 0)
		return 0;
	if (clock_gettime(CLOCK_MONOTONIC, &then) == -1) {
		warn("clock_gettime");
		return -1;
	}
	switch (poll(&(struct pollfd){.fd = fd, .events = POLLIN}, 1, timeout)) {
	case -1:
		if (errno != EINTR) {
			warn("poll");
			return -1;
		}
		/* fallthrough */
	default:
		if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
			warn("clock_gettime");
			return -1;
		}
		elapsed = (now.tv_sec - then.tv_sec) * 1000;
		elapsed += now.tv_nsec / 1000000;
		elapsed -= then.tv_nsec / 1000000;
		if (timeout > elapsed)
			return timeout - elapsed;
		/* fallthrough */
	case 0:
		return 0;
	}
}

static int
next_event(Display *display, XEvent *event)
{
	for (;;) {
		static Time const DELAY_MOUSE = 32;
		static Time last_motion = 0;

		XNextEvent(display, event);

		if (event->type != MotionNotify)
			return event->type;

		/* compress sequential motion events into a single one */
		while (XPending(display)) {
			XEvent next;
			XPeekEvent(display, &next);
			if (next.type != MotionNotify)
				break;
			if (next.xmotion.window != event->xmotion.window)
				break;
			if (next.xmotion.subwindow != event->xmotion.subwindow)
				break;
			XNextEvent(display, event);
		}
		if (event->xmotion.time - last_motion >= DELAY_MOUSE) {
			last_motion = event->xmotion.time;
			return MotionNotify;
		}
	}
}

static void
scroll(struct widget *widget, struct menu *menu, ssize_t nentry)
{
	static int const SCROLL_TIME = 32;
	int timeout = SCROLL_TIME;
	int last_offset = menu->curr_screenful.offset;

	for (;;) {
		XEvent event;

		while (XPending(widget->display)) switch (next_event(widget->display, &event)) {
		case MotionNotify: case ButtonPress: case ButtonRelease:
			if (event.xmotion.window != menu->window)
				return;
			if (index_entry(widget, menu, event.xmotion.y) != nentry) {
				XPutBackEvent(widget->display, &event);
				return;
			}
			break;
		default:
			return;
		}

		timeout = timed_poll(XConnectionNumber(widget->display), timeout);
		if (timeout == 0) {
			timeout = SCROLL_TIME;
			if (nentry == SCROLLDOWN)
				scrolldown(widget, menu, widget->entryh);
			else
				scrollup(widget, menu, widget->entryh);
			if (menu->curr_screenful.offset != last_offset)
				commitdraw(widget, menu, nentry);
			last_offset = menu->curr_screenful.offset;
			XFlush(widget->display);
		}
	}
}

static void
xsleep(unsigned int usec)
{
	struct timespec tm = {
		.tv_sec = usec / 1000000,
		.tv_nsec = (usec % 1000000) * 1000,
	};

	while (nanosleep(&tm, &tm) == -1 && errno == EINTR)
		;
}

static void
activate_entry(struct widget *widget, struct menu *menu, ssize_t nentry, bool alt)
{
	struct entry *entry;

	/* blink */
	commitdraw(widget, menu, -1);
	XSync(widget->display, False); xsleep(60000);
	commitdraw(widget, menu, nentry);
	XSync(widget->display, False); xsleep(60000);
	commitdraw(widget, menu, -1);
	XSync(widget->display, False); xsleep(60000);
	commitdraw(widget, menu, nentry);
	while (XCheckMaskEvent(
		widget->display,
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		&(XEvent){}
	))
		;

	entry = get_entry(menu, menu->selected);
	if (entry != NULL && entry->output != NULL) {
		(void)printf("%s\n", alt ? entry->altoutput : entry->output);
		(void)fflush(stdout);
	}
}

static void
menu_run(struct widget *widget, struct menu *menu)
{
	static int INITIAL_TIMEOUT = 500;
	struct menu *submenu = NULL;
	XEvent xev;
	struct entry *entry;
	ssize_t nentry, prevselected = menu->selected;
	int popup_timeout = INITIAL_TIMEOUT;
	int motion_timeout = INITIAL_TIMEOUT;
	int last_x = -1, last_y = -1;

	for (;;) {
		switch (next_event(widget->display, &xev)) {
		case ButtonPress:
			if (xev.xbutton.window == widget->rootwin) {
				XPutBackEvent(widget->display, &xev);
				goto done;
			}
			break;
		case ButtonRelease:
			if (xev.xbutton.window != menu->window) {
				XPutBackEvent(widget->display, &xev);
				goto done;
			}
			if (xev.xbutton.button != Button1 &&
			    xev.xbutton.button != Button2 &&
			    xev.xbutton.button != Button3)
				break;
			menu->selected = index_entry(widget, menu, xev.xbutton.y);
			if (menu->selected == TEAROFF) {
#warning reimplement tearing off
				break;
			}
			commitdraw(widget, menu, menu->selected);
			entry = get_entry(menu, menu->selected);
			if (entry == NULL)
				break;
			if (entry->children != NULL) {
				if (submenu == NULL || submenu->entries != entry->children) {
					if (submenu != NULL)
						menu_free(widget, submenu);
					submenu = menu_new(
						widget, menu, entry->children, entry->nchildren,
						menu->window_geometry.x,
						menu->window_geometry.y + get_entry_ypos(widget, menu, menu->selected),
						menu->window_geometry.width,
						false, entry->label
					);
				}
				menu_run(widget, submenu);
			} else if (entry->output != NULL) {
				activate_entry(
					widget, menu, menu->selected,
					(xev.xbutton.state & ShiftMask) ||
					(xev.xbutton.button == Button2)
				);
				XPutBackEvent(widget->display, &xev);
				goto done;
			}
			break;
		case ClientMessage:
			if ((Atom)xev.xclient.data.l[0] == XInternAtom(widget->display, "WM_DELETE_WINDOW", False)) {
				XPutBackEvent(widget->display, &xev);
				goto done;
			}
			break;
		case ConfigureNotify:
			if (xev.xconfigure.window != menu->window)
				break;
			menu->window_geometry.x = xev.xconfigure.x;
			menu->window_geometry.y = xev.xconfigure.y;
			if (menu->window_geometry.width  == xev.xconfigure.width &&
			    menu->window_geometry.height == xev.xconfigure.height)
				break;       /* size not changed */
			menu->window_geometry.width = xev.xconfigure.width;
			menu->window_geometry.height = xev.xconfigure.height;
			drawmenu(widget, menu);
			commitdraw(widget, menu, menu->selected);
			break;
		case UnmapNotify:
			if (widget->leader == None)
				break;
			if (xev.xdestroywindow.window != widget->leader)
				break;
			XPutBackEvent(widget->display, &xev);
			goto done;
		case KeyPress:
			KeySym ksym = XkbKeycodeToKeysym(widget->display, xev.xkey.keycode, 0, 0);

			if (ksym == XK_Tab && FLAG(xev.xkey.state, ShiftMask))
				ksym = XK_ISO_Left_Tab;
			if (ksym == XK_Up || ksym == XK_KP_Up || ksym == XK_ISO_Left_Tab) {
				menu->selected = selprev(widget, menu, menu->selected);
			} else if (ksym == XK_Down || ksym == XK_KP_Down || ksym == XK_Tab) {
				menu->selected = selnext(widget, menu, menu->selected);
			} else if (ksym == XK_Home || ksym == XK_KP_Home) {
				menu->selected = selfirst(widget, menu);
			} else if (ksym == XK_End || ksym == XK_KP_End) {
				menu->selected = sellast(widget, menu);
			} else if (ksym == XK_Right || ksym == XK_KP_Right ||
			           ksym == XK_Return || ksym == XK_KP_Enter) {
				entry = get_entry(menu, menu->selected);
				if (entry == NULL)
					break;
				if (entry->children != NULL) {
					if (submenu == NULL || submenu->entries != entry->children) {
						if (submenu != NULL)
							menu_free(widget, submenu);
						submenu = menu_new(
							widget, menu,
							entry->children, entry->nchildren,
							menu->window_geometry.x,
							menu->window_geometry.y + get_entry_ypos(widget, menu, menu->selected),
							menu->window_geometry.width,
							false, entry->label
						);
					}
					submenu->selected = 0;
					commitdraw(widget, submenu, submenu->selected);
					menu_run(widget, submenu);
				} else {
					activate_entry(
						widget, menu, menu->selected,
						xev.xkey.state & ShiftMask
					);
					xev.xkey.keycode = XKeysymToKeycode(
						widget->display,
						XK_Escape
					);
					XPutBackEvent(widget->display, &xev);
					goto done;
				}
			} else if (ksym == XK_Escape) {
				XPutBackEvent(widget->display, &xev);
				goto done;
			} else if (ksym == XK_Left || ksym == XK_KP_Left) {
				goto done;
			}
			break;
		case EnterNotify:
			if (submenu != NULL && xev.xcrossing.window == submenu->window)
				menu_run(widget, submenu);
			break;
		case MotionNotify:
			for (struct menu *m = menu; m->parent != NULL; m = m->parent) {
				struct entry *above;
				if (xev.xmotion.window != m->parent->window)
					continue;
				above = get_entry(m->parent, index_entry(
					widget, m->parent, xev.xmotion.y
				));
				if (above != NULL && above->children == m->entries)
					break;
				/*
				 * Pointer on ancestor menu, over entry which
				 * does not descends into current menu.
				 */
				XPutBackEvent(widget->display, &xev);
				goto done;
			}
			if (xev.xmotion.window != menu->window) {
				if (submenu == NULL)
					menu->selected = -1;
				break;
			}
			nentry = index_entry(widget, menu, xev.xbutton.y);
			if (nentry == SCROLLUP || nentry == SCROLLDOWN)
				scroll(widget, menu, nentry);
			if (submenu == NULL || xev.xbutton.x <= last_x)
				menu->selected = nentry;
			last_x = xev.xbutton.x;
			last_y = xev.xbutton.y;
			break;
		case PropertyNotify:
			if (xev.xproperty.state != PropertyNewValue ||
			    xev.xproperty.window != widget->rootwin ||
			    xev.xproperty.atom != XA_RESOURCE_MANAGER)
				break;
			char *str = gettextprop(
				widget->display,
				widget->rootwin,
				XA_RESOURCE_MANAGER
			);
			if (str == NULL)
				break;
			loadresources(widget, str);
			free(str);
			for (struct menu *ancestor = menu;
			     ancestor != NULL;
			     ancestor = ancestor->parent) {
				commitdraw(widget, ancestor, ancestor->selected);
			}
			break;
		}

		/*
		 * When a submenu is being previewed, allow moving the
		 * pointer rightwards for a while and not unmap the
		 * submenu, even if the pointer pass over another entry.
		 */
		if (submenu != NULL && menu->selected == prevselected) {
			nentry = index_entry(widget, menu, last_y);
			if (nentry == menu->selected)
				continue;
			if (XPending(widget->display) > 0)
				continue;
			motion_timeout = timed_poll(
				XConnectionNumber(widget->display),
				motion_timeout
			);
			if (motion_timeout > 0)
				continue;
			menu->selected = index_entry(widget, menu, last_y);
		}
		motion_timeout = INITIAL_TIMEOUT;

		if (menu->selected != prevselected) {
			popup_timeout = INITIAL_TIMEOUT;
			commitdraw(widget, menu, menu->selected);
		}
		prevselected = menu->selected;
		entry = get_entry(menu, menu->selected);
		if (submenu != NULL && entry != NULL && submenu->entries == entry->children)
			continue;
		if (submenu != NULL)
			menu_free(widget, submenu);
		submenu = NULL;
		if (entry != NULL && entry->children != NULL) {
			if (XPending(widget->display) > 0)
				continue;
			popup_timeout = timed_poll(
				XConnectionNumber(widget->display),
				popup_timeout
			);
			if (popup_timeout > 0)
				continue;
			popup_timeout = INITIAL_TIMEOUT;
			submenu = menu_new(
				widget, menu, entry->children, entry->nchildren,
				menu->window_geometry.x,
				menu->window_geometry.y + get_entry_ypos(widget, menu, menu->selected),
				menu->window_geometry.width,
				false, entry->label
			);
		}
	}
done:
	if (submenu != NULL)
		menu_free(widget, submenu);
	menu->selected = -1;
	commitdraw(widget, menu, menu->selected);
	return;
}

static void
menu_popup(struct widget *widget, struct entry *entries, size_t nentries,
		int x_root, int y_root, bool windowed)
{
	struct menu *menu;
	menu = menu_new(
		widget, NULL, entries, nentries,
		x_root, y_root, 0, windowed, widget->title
	);
	(void)grabpointer(widget);
	(void)grabkeyboard(widget);
	menu_run(widget, menu);
	menu_free(widget, menu);
	ungrab(widget);
}

static void
waitrootclick(struct widget *widget, unsigned int modifier, unsigned int button)
{
	XEvent xev;
	static unsigned int MODS = ShiftMask|ControlMask|Mod1Mask|Mod4Mask;

	for (;;) {
		(void)XNextEvent(widget->display, &xev);
		if (xev.type != ButtonPress)
			continue;
		if (xev.xbutton.button == button && (
			(modifier != 0 && modifier == (xev.xbutton.state & MODS)) ||
			xev.xbutton.subwindow == None
		)) {
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
		Window leader, XrmDatabase cli_xrm, char *title, char *name, char *class,
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
		.leader = leader,

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
	struct widget *widget = NULL;
	Display *display = NULL;
	Visual *visual;
	Colormap colormap;
	int depth;
	int x_root, y_root;
	int geometry_flags = 0;
	XVisualInfo vinfo;
	char *name = "xmenu";
	char *title = "XMenu";
	char *read_data = NULL;
	XrmDatabase cli_xrm = NULL;
	Window leader = None;
	bool rootmode = false;
	struct entry *entries = NULL;
	size_t nentries = 0;
	unsigned int button = 0;
	unsigned int modifier = 0;
	bool windowed = false;
	char *monitor_name = NULL;

	if (argv[0] != NULL && argv[0][0] != '\0') {
		name = strchr(argv[0], '/');
		if (name != NULL)
			name++;
		else
			name = argv[0];
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-windowed") || !strcmp(argv[i], "-w")) {
			windowed = true;
		} else if (!strcmp(argv[i], "-leader")) {
			leader = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-name") || !strcmp(argv[i], "-N")) {
			name = argv[++i];
		} else if (!strcmp(argv[i], "-position") || !strcmp(argv[i], "-p")) {
			geometry_flags = parse_geometry(
				argv[++i], &x_root, &y_root, &monitor_name
			);
		} else if (!strcmp(argv[i], "-rootgrab") || !strcmp(argv[i], "-x")) {
			parse_keychord(argv[++i], &modifier, &button);
			rootmode = true;
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
				" [-monitor MONITOR]"
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
	if (rootmode)
		leader = None;
	else if (windowed)
		rootmode = false;
	nentries = parsestdin(&entries, &read_data);
	if (nentries == 0)
		errx(EXIT_FAILURE, "no menu generated");

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support for X11");
	if ((dpyname = XDisplayName(NULL)) == NULL || dpyname[0] == '\0')
		errx(EXIT_FAILURE, "DISPLAY is not set");
	if ((display = XOpenDisplay(dpyname)) == NULL)
		errx(EXIT_FAILURE, "%s: cannot connect to X server", dpyname);
	if (fcntl(XConnectionNumber(display), F_SETFD, FD_CLOEXEC) == -1)
		err(EXIT_FAILURE, "connection to display \"%s\"", dpyname);
	if (rootmode) {
		XGrabButton(
			display,
			button,
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
		leader, cli_xrm, title, name, "XMenu", argc, argv
	);
	if (widget == NULL)
		errx(EXIT_FAILURE, "cannot create widget");
	if (FLAG(geometry_flags, XValue|YValue)) {
		int nmons = 0;
		int x = 0;
		int y = 0;
		int width = DisplayWidth(display, DefaultScreen(display));
		int height = DisplayHeight(display, DefaultScreen(display));
		XRRMonitorInfo *infos = XRRGetMonitors(
			display, DefaultRootWindow(display),
			True, &nmons
		);
		
		if (monitor_name != NULL) {
			Atom name = None;
			long num = -1;
			int x = -1, y = -1;

			if (strcmp(monitor_name, "cursor") == 0) {
				XQueryPointer(
					display, DefaultRootWindow(display),
					&(Window){}, &(Window){},
					&x, &y, &(int){}, &(int){}, &(unsigned){}
				);
			} else if (isdigit(*monitor_name)) {
				char *endp;
				num = strtoul(monitor_name, &endp, 10);
				if (*endp != '\0' || num < 0 || num > INT_MAX)
					num = -1;
			} else {
				name = XInternAtom(display, monitor_name, true);
			}

			for (int i = 0; i < nmons && infos != NULL; i++) {
				if (i == num || infos[i].name == name ||
				    (x >= infos[i].x && x < infos[i].x + infos[i].width) ||
				    (y >= infos[i].y && y < infos[i].y + infos[i].height)) {
					x = infos[i].x;
					y = infos[i].y;
					width = infos[i].width;
					height = infos[i].height;
					goto found;
				}
			}
			errx(EXIT_FAILURE, "cannot find monitor -- %s", monitor_name);
		}
	found:
		if (geometry_flags & XNegative)
			x_root = x + width - x_root;
		else
			x_root += x;
		if (geometry_flags & YNegative)
			y_root = y + height - y_root;
		else
			y_root += y;
		XRRFreeMonitors(infos);
	}

	do {
		if (rootmode)
			waitrootclick(widget, modifier, button);
		if (!FLAG(geometry_flags, XValue|YValue)) {
			XQueryPointer(
				display, DefaultRootWindow(display),
				&(Window){}, &(Window){},
				&x_root, &y_root, &(int){}, &(int){}, &(unsigned){}
			);
		}
		menu_popup(widget, entries, nentries, x_root, y_root, windowed);
	} while (rootmode);
	cleanup(widget);
	free(read_data);
	free(entries);
	XrmDestroyDatabase(cli_xrm);
	if (display != NULL)
		XCloseDisplay(display);
	return EXIT_SUCCESS;
}
