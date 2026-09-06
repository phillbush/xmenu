#include <err.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>

#include <control/application.h>
#include <control/font.h>
#include <control/menu.h>
#include "control/resources.h"

#include <util/color.h>
#include <util/defs.h>

#define MAXPATHS                32
#define PADDING                 4
#define MIN_HEIGHT              (16 + PADDING * 2)
#define TRIANGLE_HEIGHT(wid)    (ctrlfnt_height((wid)->fontset)/2)
#define TRIANGLE_WIDTH(wid)     (TRIANGLE_HEIGHT(wid)/2)
#define TRIANGLE_PAD(wid)       TRIANGLE_HEIGHT(wid)

/* unindexable indices for special entries */
#define MAKE_DUMMY_ENTRY(str) (&(struct ctrlmenu_entry){\
	.type = CTRLMENU_SEPARATOR,\
	.label = (str),\
})
static struct ctrlmenu_entry *TEAR_OFF    = MAKE_DUMMY_ENTRY("TEAR_OFF");
static struct ctrlmenu_entry *SCROLL_UP   = MAKE_DUMMY_ENTRY("SCROLL_UP");
static struct ctrlmenu_entry *SCROLL_DOWN = MAKE_DUMMY_ENTRY("SCROLL_DOWN");

enum {
	DIR_UP,
	DIR_DOWN,
	DIR_LEFT,
	DIR_RIGHT
};

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

struct ctrlmenu_context {
	void *ctx;
	Display *display;
	int screen_number;
	Window root_window;
	Window leader;

	Visual *visual;
	int depth;
	Colormap colormap;
	Cursor cursor;

	struct ctrlfnt *fontset;
	unsigned int fonth;
	int entryh, separatorh;
	int inner_border_width;  /* part of window content, for shadow/relief */
	int outer_border_width;  /* X11 border, not counted on window size */
	int iconsize;
	int maxentries;
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
};

enum run_flags{
	RUN_POPUP      = 0x01,   /* map menu before running its event loop */
	RUN_ENTERED    = 0x02,   /* run as if pointer has entered menu */
	RUN_SELECT     = 0x04,   /* automatically select first entry */
	RUN_UNTEARABLE = 0x08,   /* dynamically generated submenu, do not tear off */
};

static int menu_run(struct ctrlmenu_context *,
	struct ctrlmenu *, struct ctrlmenu *,
	int x, int y, enum run_flags, Time
);

/*
 * resource creators
 */

static int
create_picture(Display *display, Window root_window,
		Visual *visual, int depth,
		Picture *picture, Pixmap *pixmap,
		XRenderColor *color, bool isalpha)
{
	*pixmap = XCreatePixmap(display, root_window, 1, 1, isalpha ? 8 : depth);
	if (*pixmap == None) {
		warnx("could not create pixmap");
		return RETURN_FAILURE;
	}
	*picture = XRenderCreatePicture(
		display,
		*pixmap,
		isalpha
			? XRenderFindStandardFormat(display, PictStandardA8)
			: XRenderFindVisualFormat(display, visual),
		CPRepeat,
		&(XRenderPictureAttributes){
			.repeat = RepeatNormal,
		}
	);
	if (*picture == None) {
		warnx("could not create picture");
		return RETURN_FAILURE;
	}
	XRenderFillRectangle(display, PictOpSrc, *picture, color, 0, 0, 1, 1);
	return RETURN_SUCCESS;
}

/*
 * resource parsers and setters
 */

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

static void
setfont(struct ctrlmenu_context *menuctx, const char *facename, double facesize)
{
	struct ctrlfnt *fontset;

	if (facename == NULL)
		facename = "xft:";
	fontset = ctrlfnt_open(
		menuctx->display,
		menuctx->screen_number,
		menuctx->visual,
		menuctx->colormap,
		facename,
		facesize
	);
	if (fontset == NULL)
		return;
	menuctx->fontset = fontset;
	menuctx->fonth = ctrlfnt_height(fontset);
	menuctx->entryh = menuctx->fonth + PADDING * 2;
	menuctx->entryh = MAX(menuctx->entryh, MIN_HEIGHT);
	menuctx->iconsize = menuctx->fonth;
	menuctx->separatorh = menuctx->entryh / 2;
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

static Bool
parse_bg(void *ctx, void *, char const *value)
{
	struct ctrlmenu_context *menuctx = ctx;
	struct rgb rgb;
	struct hsy hsy;
	XColor xcolor;

	if (value == NULL)
		return false;
	if (!XParseColor(menuctx->display, menuctx->colormap, value, &xcolor)) {
		warnx("%s: unkwnown color name", value);
		return false;
	}
	hsy = to_hsy(rgb = (struct rgb){
		.red = (double)xcolor.red / 0xFFFF,
		.green = (double)xcolor.green / 0xFFFF,
		.blue = (double)xcolor.blue / 0xFFFF,
	});

	setcolor(menuctx->display, &menuctx->colors[COLOR_BG], rgb);
	if (hsy.luma < 0.5) {
		/* dark background; make text white */
		setcolor(menuctx->display, &menuctx->colors[COLOR_FG], (struct rgb){1,1,1});
	} else {
		/* light background; make text black */
		setcolor(menuctx->display, &menuctx->colors[COLOR_FG], (struct rgb){0,0,0});
	}
	setcolor(menuctx->display, &menuctx->colors[COLOR_TOP], to_rgb((struct hsy){
		hsy.hue, hsy.saturation, hsy.luma*0.5 + 0.5
	}));
	setcolor(menuctx->display, &menuctx->colors[COLOR_BOT], to_rgb((struct hsy){
		hsy.hue, hsy.saturation, hsy.luma*0.5
	}));
	return true;
}

static Bool
parse_color(void *ctx, void *addr, char const *value)
{
	struct ctrlmenu_context *menuctx = ctx;
	struct color *color = addr;
	XColor xcolor;

	if (value == NULL)
		return false;
	if (!XParseColor(menuctx->display, menuctx->colormap, value, &xcolor))
		return false;
	setcolor(menuctx->display, color, (struct rgb){
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
	struct ctrlmenu_context *menuctx = ctx;
	double d;

	if (!parse_double(NULL, &d, value) || d < 0.0 || d > 1.0)
		return false;
	menuctx->colors[COLOR_OPACITY].chans = (XRenderColor){ .alpha = d * 0xFFFF};
	XRenderFillRectangle(
		menuctx->display,
		PictOpSrc,
		menuctx->colors[COLOR_OPACITY].pict,
		&menuctx->colors[COLOR_OPACITY].chans,
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
reload_resources(struct ctrlmenu_context *menuctx, XrmDatabase xdb)
{
	char const *fontspec = NULL;
	double fontsize = 0.0;
#define MK_RES(n, c, p, fun) (struct ctrlres){\
	.name_path  = (char *[]){(char *)ctrlapp_getname(), n},\
	.class_path = (char *[]){(char *)ctrlapp_getclass(), c},\
	.nnodes = 2, .addr = p, .setter = fun}
	struct ctrlres resources[] = {
		MK_RES("activeBackground", "ActiveBackground", &menuctx->colors[COLOR_SELBG], parse_color),
		MK_RES("activeForeground", "ActiveForeground", &menuctx->colors[COLOR_SELFG], parse_color),
		MK_RES("alignment", "Alignment", &menuctx->alignment, parse_alignment),
		MK_RES("background", "Background", NULL, parse_bg),
		MK_RES("borderWidth", "BorderWidth", &menuctx->outer_border_width, parse_width),
		MK_RES("borderColor", "BorderColor", &menuctx->colors[COLOR_BORDER], parse_color),
		MK_RES("font", "Font", &fontspec, CTRLRES_SETSTR),
		MK_RES("fontSize", "FontSize", &fontsize, parse_double),
		MK_RES("foreground", "Foreground", &menuctx->colors[COLOR_FG], parse_color),
		MK_RES("maxItems", "MaxItems", &menuctx->maxentries, parse_width),
		MK_RES("opacity", "Opacity", NULL, parse_opacity),
		MK_RES("shadowThickness", "ShadowThickness", &menuctx->inner_border_width, parse_width),
		MK_RES("tearOff", "TearOff", &menuctx->tearoff, CTRLRES_SETBOOL),

		/* undocumented; for compatibility with Motif's TearOffModel resource */
		MK_RES("tearOffModel", "TearOffModel", &menuctx->tearoff, parse_tearoff),

		/* overriding resources; must come later */
		MK_RES("faceName", "FaceName", &fontspec, CTRLRES_SETSTR),
		MK_RES("topShadowColor", "TopShadowColor", &menuctx->colors[COLOR_TOP], parse_color),
		MK_RES("bottomShadowColor", "BottomShadowColor", &menuctx->colors[COLOR_BOT], parse_color),
	};

	ctrlres_parsexrdb(
		menuctx,
		(XrmDatabase[]){xdb}, 1,
		resources, LEN(resources)
	);
	if (fontspec != NULL)
		setfont(menuctx, fontspec, fontsize);
}


/*
 * entry positioning routines
 */

static struct ctrlmenu_entry *
get_entry(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu, long y)
{
	long h = menuctx->inner_border_width;

	if (menu->tearable) {
		if (y < h + menuctx->separatorh)
			return TEAR_OFF;
		h += menuctx->separatorh;
	}
	if (menu->overflow && y < h + menuctx->separatorh) {
		return SCROLL_UP;
	}
	if (menu->overflow && y >= menu->window_geometry.height - menuctx->separatorh) {
		return SCROLL_DOWN;
	}
	if (menu->overflow)
		h += menuctx->separatorh;
	for (size_t i = menu->curr_screenful.first_entry; i < menu->nentries; i++) {
		struct ctrlmenu_entry *entry = &menu->entries[i];
		if (entry->type == CTRLMENU_SEPARATOR) {
			h += menuctx->separatorh;
			continue;
		}
		if (entry->disabled) {
			h += menuctx->entryh;
			continue;
		}
		if (y >= h && y < (long)h + menuctx->entryh)
			return entry;
		h += menuctx->entryh;
		if (menu->overflow &&
		    h + menuctx->entryh + menuctx->inner_border_width >=
		    menu->window_geometry.height) {
			break;
		}
	}
	return NULL;
}

static int
get_entry_height(struct ctrlmenu_context *menuctx, struct ctrlmenu_entry *entry)
{
	if (entry == NULL)
		return 0;
	if (entry->type == CTRLMENU_SEPARATOR)
		return menuctx->separatorh;
	else
		return menuctx->entryh;
}

static int
get_entry_ypos(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu,
		struct ctrlmenu_entry *entry)
{
	int ypos = menuctx->inner_border_width;

	if (entry == NULL)
		return -1;
	if (entry == TEAR_OFF)
		return ypos;
	ypos += menuctx->separatorh;
	if (menu->overflow) {
		if (entry == SCROLL_DOWN)
			return menu->total_height - menuctx->separatorh
			- menuctx->inner_border_width;
		if (entry == SCROLL_UP)
			return ypos;
		ypos += menuctx->separatorh;
	}
	for (size_t i = 0; i < menu->nentries; i++) {
		struct ctrlmenu_entry *e = &menu->entries[i];

		if (e == entry)
			return ypos;
		if (e->type == CTRLMENU_SEPARATOR)
			ypos += menuctx->separatorh;
		else
			ypos += menuctx->entryh;
	}
	return -1;
}

static int
get_first_entry_ypos(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu)
{
	int y;

	y = menuctx->inner_border_width;
	y += get_entry_height(menuctx, TEAR_OFF);
	if (menu->overflow)
		y += menuctx->separatorh;
	return y;
}


/*
 * drawing routines
 */

static void
draw_dashline(struct ctrlmenu_context *menuctx, Picture picture, int width, int y)
{
	XRectangle toprects[32];
	XRectangle botrects[32];
	size_t nrects;
	int x;
	int w;
	int maxw;

	x = menuctx->inner_border_width + PADDING;
	y += menuctx->separatorh / 2;
	w = 0;
	maxw = width - menuctx->inner_border_width * 2 - PADDING * 2;
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
				menuctx->display,
				PictOpSrc,
				picture,
				&menuctx->colors[COLOR_TOP].chans,
				toprects,
				nrects
			);
			XRenderFillRectangles(
				menuctx->display,
				PictOpSrc,
				picture,
				&menuctx->colors[COLOR_BOT].chans,
				botrects,
				nrects
			);
		}
	}
}

static void
draw_separator(struct ctrlmenu_context *menuctx, Picture picture, XRectangle *rect)
{
	XRenderFillRectangle(
		menuctx->display,
		PictOpSrc,
		picture,
		&menuctx->colors[COLOR_BOT].chans,
		menuctx->inner_border_width + PADDING,
		rect->y + menuctx->separatorh / 2 - 1,
		rect->width - menuctx->inner_border_width * 2 - PADDING * 2,
		1
	);
	XRenderFillRectangle(
		menuctx->display,
		PictOpSrc,
		picture,
		&menuctx->colors[COLOR_TOP].chans,
		menuctx->inner_border_width + PADDING,
		rect->y + menuctx->separatorh / 2,
		rect->width - menuctx->inner_border_width * 2 - PADDING * 2,
		1
	);
}

static void
draw_shadows(struct ctrlmenu_context *menuctx, Picture picture, XRectangle *geometry)
{
	for (int i = 0; i < menuctx->inner_border_width; i++) {
		/* draw light shadow */
		XRenderFillRectangle(
			menuctx->display,
			PictOpSrc,
			picture,
			&menuctx->colors[COLOR_TOP].chans,
			i, i,
			1, geometry->height - (i * 2 + 1)
		);
		XRenderFillRectangle(
			menuctx->display,
			PictOpSrc,
			picture,
			&menuctx->colors[COLOR_TOP].chans,
			i, i,
			geometry->width - (i * 2 + 1), 1
		);

		/* draw dark shadow */
		XRenderFillRectangle(
			menuctx->display,
			PictOpSrc,
			picture,
			&menuctx->colors[COLOR_BOT].chans,
			geometry->width - 1 - i, i,
			1, geometry->height - i * 2
		);
		XRenderFillRectangle(
			menuctx->display,
			PictOpSrc,
			picture,
			&menuctx->colors[COLOR_BOT].chans,
			i, geometry->height - 1 - i,
			geometry->width - i * 2, 1
		);
	}
}

static void
draw_triangle(struct ctrlmenu_context *menuctx, Picture picture, Picture src, int x, int y, int direction)
{
	XTriangle triangle;

	if (direction == DIR_UP) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_WIDTH(menuctx))
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(menuctx) / 2),
			XDoubleToFixed(y)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(menuctx)),
			XDoubleToFixed(y + TRIANGLE_WIDTH(menuctx))
		};
	} else if (direction == DIR_DOWN) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(menuctx) / 2),
			XDoubleToFixed(y + TRIANGLE_WIDTH(menuctx))
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_HEIGHT(menuctx)),
			XDoubleToFixed(y)
		};
	} else if (direction == DIR_LEFT) {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_WIDTH(menuctx)),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x - TRIANGLE_WIDTH(menuctx)),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(menuctx) / 2)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(menuctx))
		};
	} else {
		triangle.p1 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y)
		};
		triangle.p2 = (XPointFixed){
			XDoubleToFixed(x + TRIANGLE_WIDTH(menuctx)),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(menuctx) / 2)
		};
		triangle.p3 = (XPointFixed){
			XDoubleToFixed(x),
			XDoubleToFixed(y + TRIANGLE_HEIGHT(menuctx))
		};
	}
	XRenderCompositeTriangles(
		menuctx->display,
		PictOpOver,
		src,
		picture,
		XRenderFindStandardFormat(menuctx->display, PictStandardA8),
		0, 0,
		&triangle,
		1
	);
}

static void
canvas_reset(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu)
{
	XRectangle entry_geometry;
	int textx, textw;

	if (menu->canvas_width  < menu->window_geometry.width ||
	    menu->canvas_height < menu->window_geometry.height) {
		menu->canvas_width = MAX(
			menu->window_geometry.width + 128,
			menu->canvas_width
		);
		menu->canvas_height = MAX(
			menu->window_geometry.height + 128,
			menu->total_height
		);
		for (size_t i = 0; i < CANVAS_LAST; i++) {
			if (menu->canvas[i].picture != None) {
				XRenderFreePicture(
					menuctx->display,
					menu->canvas[i].picture
				);
			}
			if (menu->canvas[i].pixmap != None)
				XFreePixmap(menuctx->display, menu->canvas[i].pixmap);
			menu->canvas[i].pixmap = XCreatePixmap(
				menuctx->display,
				menu->window,
				menu->canvas_width,
				menu->canvas_height,
				menuctx->depth
			);
			menu->canvas[i].picture = XRenderCreatePicture(
				menuctx->display,
				menu->canvas[i].pixmap,
				XRenderFindVisualFormat(menuctx->display, menuctx->visual),
				0,
				NULL
			);
			XRenderFillRectangle(
				menuctx->display,
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
			menuctx->display,
			PictOpSrc,
			menuctx->colors[i==CANVAS_SELECT ?  COLOR_SELBG : COLOR_BG].pict,
			menuctx->colors[COLOR_OPACITY].pict,
			menu->canvas[i].picture,
			0, 0,
			0, 0,
			0, 0,
			menu->canvas_width, menu->canvas_height
		);
	}
	entry_geometry.x = menuctx->inner_border_width + PADDING;
	if (menu->hasicon)
		entry_geometry.x += menuctx->iconsize + PADDING;
	entry_geometry.y = get_first_entry_ypos(menuctx, menu);
	entry_geometry.width = menu->window_geometry.width;
	for (size_t entryc = 0; entryc < menu->nentries; entryc++) {
		struct ctrlmenu_entry *entry = &menu->entries[entryc];
		size_t labellen;
		int size_diff = menuctx->iconsize;
		XImage *icon = NULL;
		XImage *largest_icon = NULL;

		if (entry->type == CTRLMENU_SEPARATOR) {
			entry_geometry.height = menuctx->separatorh;
			draw_separator(
				menuctx,
				menu->canvas[CANVAS_NORMAL].picture,
				&entry_geometry
			);
			draw_separator(
				menuctx,
				menu->canvas[CANVAS_SELECT].picture,
				&entry_geometry
			);
			goto next;
		}
		entry_geometry.height = menuctx->entryh;

		/* find best-sized icon */
		largest_icon = entry->icons;
		size_diff = menuctx->iconsize;
		for (size_t nicon = 0; nicon < entry->nicons; nicon++) {
			if (entry->icons[nicon].width > largest_icon->width &&
			    entry->icons[nicon].height > largest_icon->height)
				largest_icon = &entry->icons[nicon];
			if (MAX(entry->icons[nicon].width,
			        entry->icons[nicon].height) >
				menuctx->iconsize)
				continue;
			if (menuctx->iconsize - entry->icons[nicon].height <= size_diff)
				continue;
			size_diff = menuctx->iconsize - entry->icons[nicon].height;
			icon = &entry->icons[nicon];
		}
		if (icon == NULL)
			icon = largest_icon;
		if (icon != NULL) {
			Pixmap pixmap;
			Picture picture;
			GC gc;
			int width = icon->width;
			int height = icon->height;

			pixmap = XCreatePixmap(
				menuctx->display,
				menuctx->root_window,
				icon->width, icon->height,
				icon->depth
			);
			picture = XRenderCreatePicture(
				menuctx->display, pixmap,
				XRenderFindFormat(
					menuctx->display,
					PictFormatDepth,
					&(XRenderPictFormat){
						.depth = icon->depth,
					}, 0
				), 0, NULL
			);
			gc = XCreateGC(menuctx->display, pixmap, 0, NULL),

			XPutImage(
				menuctx->display, pixmap, gc, icon,
				0, 0, 0, 0, icon->width, icon->height
			);

			if (size_diff > PADDING) {
				/* image is too small/large; scale it */
				if (width > height) {
					height = (height * menuctx->iconsize) / width;
					width = menuctx->iconsize;
				} else {
					width = (width * menuctx->iconsize) / height;
					height = menuctx->iconsize;
				}

				XRenderSetPictureFilter(
					menuctx->display, picture, FilterBilinear, NULL, 0
				);
				XRenderSetPictureTransform(
					menuctx->display, picture, &(XTransform){
						{{(icon->width<<16u)/width, 0, 0},
						{0, (icon->height<<16u)/height, 0},
						{0, 0, 0x10000}},
					}
				);
			}

			XRenderComposite(
				menuctx->display, PictOpOver,
				picture, None,
				menu->canvas[CANVAS_NORMAL].picture,
				0, 0, 0, 0,
				menuctx->inner_border_width + PADDING
				+ (menuctx->iconsize - width)/2,
				entry_geometry.y
				+ (menuctx->entryh - height)/2,
				width, height
			);
			XRenderComposite(
				menuctx->display, PictOpOver,
				picture, None,
				menu->canvas[CANVAS_SELECT].picture,
				0, 0, 0, 0,
				menuctx->inner_border_width + PADDING
				+ (menuctx->iconsize - width)/2,
				entry_geometry.y
				+ (menuctx->entryh - height)/2,
				width, height
			);

			XFreeGC(menuctx->display, gc);
			XRenderFreePicture(menuctx->display, picture);
			XFreePixmap(menuctx->display, pixmap);
		}
		if (entry->type & CTRLMENU_CASCADE) {
			draw_triangle(
				menuctx,
				menu->canvas[CANVAS_NORMAL].picture,
				menuctx->colors[COLOR_FG].pict,
				entry_geometry.width - PADDING - TRIANGLE_WIDTH(menuctx) - TRIANGLE_PAD(menuctx)/2,
				entry_geometry.y + menuctx->entryh/2 - TRIANGLE_HEIGHT(menuctx)/2,
				DIR_RIGHT
			);
			draw_triangle(
				menuctx,
				menu->canvas[CANVAS_SELECT].picture,
				menuctx->colors[COLOR_SELFG].pict,
				entry_geometry.width - PADDING - TRIANGLE_WIDTH(menuctx) - TRIANGLE_PAD(menuctx)/2,
				entry_geometry.y + menuctx->entryh/2 - TRIANGLE_HEIGHT(menuctx)/2,
				DIR_RIGHT
			);
		}
		labellen = strlen(entry->label);
		textw = ctrlfnt_width(menuctx->fontset, entry->label, labellen);
		if (menuctx->alignment == ALIGN_RIGHT && menu->hassubmenu)
			textx = entry_geometry.width - textw - PADDING - TRIANGLE_WIDTH(menuctx) - TRIANGLE_PAD(menuctx);
		else if (menuctx->alignment == ALIGN_RIGHT)
			textx = entry_geometry.width - textw - PADDING;
		else if (menuctx->alignment == ALIGN_CENTER)
			textx = entry_geometry.x + (menu->window_geometry.width - textw) / 2;
		else
			textx = entry_geometry.x;
		if (entry->type != CTRLMENU_SEPARATOR && !entry->disabled) {
			ctrlfnt_draw(
				menuctx->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				menuctx->colors[COLOR_FG].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label, labellen
			);
			ctrlfnt_draw(
				menuctx->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				menuctx->colors[COLOR_SELFG].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label, labellen
			);
		} else {
			ctrlfnt_draw(
				menuctx->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				menuctx->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = entry_geometry.y + 1,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label, labellen
			);
			ctrlfnt_draw(
				menuctx->fontset,
				menu->canvas[CANVAS_NORMAL].picture,
				menuctx->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label, labellen
			);
			ctrlfnt_draw(
				menuctx->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				menuctx->colors[COLOR_TOP].pict,
				(XRectangle){
					.x = textx + 1,
					.y = entry_geometry.y + 1,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label, labellen
			);
			ctrlfnt_draw(
				menuctx->fontset,
				menu->canvas[CANVAS_SELECT].picture,
				menuctx->colors[COLOR_BOT].pict,
				(XRectangle){
					.x = textx,
					.y = entry_geometry.y,
					.width = entry_geometry.width,
					.height = entry_geometry.height,
				},
				entry->label, labellen
			);
		}
next:
		entry_geometry.y += entry_geometry.height;
	}

	/* draw tear-off line */
	draw_dashline(
		menuctx,
		menu->canvas[CANVAS_NORMAL].picture,
		menu->window_geometry.width,
		menuctx->inner_border_width
	);
	draw_dashline(
		menuctx,
		menu->canvas[CANVAS_SELECT].picture,
		menu->window_geometry.width,
		menuctx->inner_border_width
	);

	for (size_t i = 0; i < CANVAS_FINAL; i++) {
		int y = menuctx->inner_border_width + get_entry_height(menuctx, TEAR_OFF);
		if (menu->overflow) {
			draw_triangle(
				menuctx,
				menu->canvas[i].picture,
				menuctx->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->window_geometry.width / 2 - TRIANGLE_HEIGHT(menuctx) / 2,
				y + menuctx->separatorh /2 - TRIANGLE_WIDTH(menuctx) / 2,
				DIR_UP
			);
			draw_triangle(
				menuctx,
				menu->canvas[i].picture,
				menuctx->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->window_geometry.width / 2 - TRIANGLE_HEIGHT(menuctx) / 2,
				menu->total_height - menuctx->separatorh /2
				- TRIANGLE_WIDTH(menuctx) / 2,
				DIR_DOWN
			);
		}
	}
	XSetWindowBackgroundPixmap(
		menuctx->display,
		menu->window,
		menu->canvas[CANVAS_NORMAL].pixmap
	);
	XSetWindowBorderPixmap(menuctx->display, menu->window, menuctx->colors[COLOR_BORDER].pix);
	XClearWindow(menuctx->display, menu->window);
}

static void
canvas_commit(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu,
		struct ctrlmenu_entry *entry)
{
	int src_canvas_y, dst_canvas_y;

	XRenderFillRectangle(
		menuctx->display,
		PictOpClear,
		menu->canvas[CANVAS_FINAL].picture,
		&(XRenderColor){ 0 },
		0, 0,
		menu->window_geometry.width,
		menu->window_geometry.height
	);
	XRenderComposite(
		menuctx->display,
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
	if (menu->overflow) {
		XRenderComposite(
			menuctx->display,
			PictOpOver,
			menu->canvas[CANVAS_NORMAL].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, get_entry_ypos(menuctx, menu, SCROLL_UP),
			0, 0,
			0, get_entry_ypos(menuctx, menu, SCROLL_UP),
			menu->window_geometry.width,
			get_entry_height(menuctx, SCROLL_UP)
		);
		XRenderComposite(
			menuctx->display,
			PictOpOver,
			menu->canvas[CANVAS_NORMAL].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, get_entry_ypos(menuctx, menu, SCROLL_DOWN),
			0, 0,
			0, menu->window_geometry.height
			- get_entry_height(menuctx, SCROLL_DOWN)
			- menuctx->inner_border_width,
			menu->window_geometry.width,
			get_entry_height(menuctx, SCROLL_DOWN)
		);
	}
	src_canvas_y = get_entry_ypos(menuctx, menu, entry);
	if (entry == SCROLL_DOWN)
		dst_canvas_y = menu->window_geometry.height
		             - menuctx->separatorh - menuctx->inner_border_width;
	else if (entry == SCROLL_UP || entry == TEAR_OFF)
		dst_canvas_y = src_canvas_y;
	else
		dst_canvas_y = src_canvas_y - menu->curr_screenful.offset;
	if (src_canvas_y >= 0) {
		XRenderComposite(
			menuctx->display,
			PictOpOver,
			menu->canvas[CANVAS_SELECT].picture,
			None,
			menu->canvas[CANVAS_FINAL].picture,
			0, src_canvas_y,
			0, 0,
			0, dst_canvas_y,
			menu->window_geometry.width,
			get_entry_height(menuctx, entry)
		);
	}
	draw_shadows(
		menuctx,
		menu->canvas[CANVAS_FINAL].picture,
		&menu->window_geometry
	);
	XSetWindowBackgroundPixmap(
		menuctx->display,
		menu->window,
		menu->canvas[CANVAS_FINAL].pixmap
	);
	XClearWindow(menuctx->display, menu->window);
}

/*
 * event getter
 */

static Bool
is_menu_event(Display *display, XEvent *event, XPointer arg)
{
	struct ctrlmenu *top_menu = (void *)arg;

	switch (event->type) {
	case ButtonPress: case ButtonRelease:
	case KeyPress: case KeyRelease:
	case EnterNotify: case LeaveNotify:
	case MotionNotify:
		for (struct ctrlmenu *menu = top_menu; menu != NULL; menu = menu->next)
			if (event->xconfigure.window == menu->window)
				return True;
		for (int screen = 0; screen < ScreenCount(display); screen++)
			if (event->xconfigure.window == RootWindow(display, screen))
				return True;
		return False;
	case ClientMessage:
		return False;
	case ConfigureNotify:
		for (struct ctrlmenu *menu = top_menu; menu != NULL; menu = menu->next)
			if (event->xconfigure.window == menu->window)
				return True;
		return False;
	default:
		for (struct ctrlmenu *menu = top_menu; menu != NULL; menu = menu->next)
			if (event->xany.window == menu->window)
				return True;
		return False;
	}
}

static bool
check_event(Display *display, struct ctrlmenu *menu)
{
	XEvent event;

	if (XCheckIfEvent(display, &event, is_menu_event, (void *)menu)) {
		XPutBackEvent(display, &event);
		return true;
	}
	return false;
}

static bool
next_event(Display *display, XEvent *event, struct ctrlmenu *menu,
		int *timeout, bool is_popup)
{
	struct timespec then, now;
	int elapsed;
	struct pollfd pfd = {
		.fd = XConnectionNumber(display),
		.events = POLLIN,
	};

	if (clock_gettime(CLOCK_MONOTONIC, &then) == -1) {
		warn("clock_gettime");
		then.tv_sec = 0;
	}

	if (XPending(display) == 0 || !check_event(display, menu)) {
		if (poll(&pfd, 1, *timeout?*timeout:-1) == 0) {
			*timeout = 0;
			event->type = TimeoutNotify;
			return true;
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
		warn("clock_gettime");
		*timeout = 0;
	}
	elapsed = (now.tv_sec - then.tv_sec) * 1000;
	elapsed += now.tv_nsec / 1000000;
	elapsed -= then.tv_nsec / 1000000;
	if (*timeout > elapsed)
		*timeout -= elapsed;

	for (;;) {
		if (is_popup) {
			(void)XIfEvent(display, event, is_menu_event, (void *)menu);
		} else {
			(void)XNextEvent(display, event);
			if (!is_menu_event(display, event, (void *)menu)) {
				XPutBackEvent(display, event);
				return false;
			}
		}
		if (event->type != MotionNotify)
			return true;
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
		return true;
	}
}


/*
 * entry scrolling routines
 */

static void
scroll_down(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu, int diff)
{
	int height = 0;

	if (menu->curr_screenful.offset + height >= menu->last_screenful.offset) {
		menu->curr_screenful = menu->last_screenful;
		return;
	}
	while (height < diff && menu->curr_screenful.first_entry + 1 < menu->nentries) {
		height = get_entry_height(
			menuctx,
			&menu->entries[menu->curr_screenful.first_entry]
		);
		menu->curr_screenful.first_entry++;
		menu->curr_screenful.offset += height;
	}
}

static void
scroll_up(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu, int diff)
{
	int height = 0;

	while (height < diff && menu->curr_screenful.first_entry > 0) {
		height = get_entry_height(
			menuctx,
			&menu->entries[menu->curr_screenful.first_entry]
		);
		menu->curr_screenful.first_entry--;
		menu->curr_screenful.offset -= height;
	}
}

static void
scroll_loop(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu,
		struct ctrlmenu_entry *entry)
{
	static int const SCROLL_TIME = 32;
	int timeout = SCROLL_TIME;
	int last_offset = menu->curr_screenful.offset;

	for (;;) {
		XEvent event;

		next_event(menuctx->display, &event, menu, &timeout, true);
		switch (event.type) {
		case MotionNotify: case ButtonPress: case ButtonRelease:
			if (event.xmotion.window != menu->window)
				return;
			if (get_entry(menuctx, menu, event.xmotion.y) != entry) {
				XPutBackEvent(menuctx->display, &event);
				return;
			}
			break;
		case TimeoutNotify:
			timeout = SCROLL_TIME;
			if (entry == SCROLL_DOWN)
				scroll_down(menuctx, menu, menuctx->entryh);
			else
				scroll_up(menuctx, menu, menuctx->entryh);
			if (menu->curr_screenful.offset != last_offset)
				canvas_commit(menuctx, menu, entry);
			last_offset = menu->curr_screenful.offset;
			XFlush(menuctx->display);
			break;
		default:
			return;
		}
	}
}


/*
 * entry selection routines
 */

static struct ctrlmenu_entry *
select_first_entry(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu)
{
	menu->curr_screenful.first_entry = 0;
	if (menu->tearable)
		menu->curr_screenful.offset = 0;
	else
		menu->curr_screenful.offset = get_entry_height(menuctx, TEAR_OFF);
	for (size_t i = 0; i < menu->nentries; i++) {
		struct ctrlmenu_entry *entry = &menu->entries[i];
		if (entry->type != CTRLMENU_SEPARATOR && !entry->disabled) {
			return entry;
		}
	}
	return NULL;
}

static struct ctrlmenu_entry *
select_last_entry(struct ctrlmenu_context *, struct ctrlmenu *menu)
{
	menu->curr_screenful = menu->last_screenful;
	if (menu->nentries == 0)
		return NULL;
	for (size_t i = menu->nentries - 1; i >= 0; i--) {
		struct ctrlmenu_entry *entry = &menu->entries[i];
		if (entry->type != CTRLMENU_SEPARATOR && !entry->disabled) {
			return entry;
		}
	}
	return NULL;
}

static struct ctrlmenu_entry *
select_prev_entry(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu)
{
	int ypos, height;
	int scroll_threshold;

	if (menu->selected == NULL || menu->selected == SCROLL_DOWN ||
	    menu->selected == TEAR_OFF || menu->selected == SCROLL_UP ||
	    menu->selected == menu->entries)
		return select_last_entry(menuctx, menu);
	scroll_threshold = menuctx->inner_border_width
	                 + get_entry_height(menuctx, SCROLL_UP);
	if (menu->tearable)
		scroll_threshold += get_entry_height(menuctx, TEAR_OFF);
	ypos = get_entry_ypos(menuctx, menu, menu->selected);
	height = get_entry_height(menuctx, menu->selected);
	for (struct ctrlmenu_entry *entry = menu->selected;
	     entry > menu->entries; ) {
		entry--;
		if (menu->overflow) {
			ypos -= height;
			height = get_entry_height(menuctx, entry);
			if (ypos - menu->curr_screenful.offset < scroll_threshold) {
				scroll_up(menuctx, menu, height);
			}
		}
		if (entry->type != CTRLMENU_SEPARATOR && !entry->disabled) {
			return entry;
		}
	}
	return select_last_entry(menuctx, menu);
}

static struct ctrlmenu_entry *
select_next_entry(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu)
{
	int ypos, height;
	int scroll_threshold = menu->window_geometry.height
	                     - menuctx->separatorh - menuctx->inner_border_width;

	if (menu->selected == NULL || menu->selected == SCROLL_DOWN ||
	    menu->selected == TEAR_OFF || menu->selected == SCROLL_UP ||
	    menu->selected == menu->entries + menu->nentries - 1)
		return select_first_entry(menuctx, menu);
	ypos = get_entry_ypos(menuctx, menu, menu->selected);
	height = get_entry_height(menuctx, menu->selected);
	for (struct ctrlmenu_entry *entry = menu->selected;
	     ++entry < menu->entries + menu->nentries; ) {
		if (menu->overflow) {
			ypos += height;
			height = get_entry_height(menuctx, entry);
			if (ypos - menu->curr_screenful.offset > scroll_threshold) {
				scroll_down(menuctx, menu, height);
			}
		}
		if (entry->type != CTRLMENU_SEPARATOR && !entry->disabled) {
			return entry;
		}
	}
	return select_first_entry(menuctx, menu);
}


/*
 * entry triggering
 */

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
activate_entry(struct ctrlmenu_context *menuctx, jmp_buf jmpenv,
		struct ctrlmenu *menu, struct ctrlmenu_entry *entry,
		enum run_flags flags, bool alt, Time time)
{
	if (entry == NULL)
		return;
	flags |= RUN_POPUP;
	if (entry->type == CTRLMENU_CASCADE) {
		if (menu_run(
			menuctx, menu, entry->submenu,
			0, 0, flags, time
		))
			longjmp(jmpenv, 1);
	} else if (entry->type == CTRLMENU_CASCADE_DYNAMIC) {
		struct ctrlmenu submenu = {};
		bool close_all;

		if (!entry->gen_menu(
			menuctx->ctx, entry->self,
			&submenu
		))
			return;
		close_all = menu_run(
			menuctx, menu, &submenu, 0, 0,
			flags|RUN_UNTEARABLE, time
		);
		entry->del_menu(menuctx->ctx, entry->self, &submenu);
		ctrlmenu_close(menuctx->display, &submenu);
		if (close_all)
			longjmp(jmpenv, 1);
	} else if (entry->type == CTRLMENU_COMMAND) {
		/* blink */
		canvas_commit(menuctx, menu, NULL);
		XSync(menuctx->display, False); xsleep(60000);
		canvas_commit(menuctx, menu, entry);
		XSync(menuctx->display, False); xsleep(60000);
		canvas_commit(menuctx, menu, NULL);
		XSync(menuctx->display, False); xsleep(60000);
		canvas_commit(menuctx, menu, entry);
		while (XCheckMaskEvent(
			menuctx->display,
			ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
			&(XEvent){}
		))
			;

		entry->callback(menuctx->ctx, entry->self, alt);
		longjmp(jmpenv, 1);
	}
}


/*
 * internal menu controlling routines
 */

static void
menu_map(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu,
		int x_root, int y_root, unsigned int button_width)
{
	int max_height;
	int xgap, ygap;
	XRectangle monitor;
	XRRMonitorInfo *infos;
	int nmons = 0;
	char const *title = menu->title;

	if (title == NULL)
		title = ctrlapp_getclass();
	monitor.x = 0;
	monitor.y = 0;
	monitor.width = DisplayWidth(menuctx->display, menuctx->screen_number);
	monitor.height = DisplayHeight(menuctx->display, menuctx->screen_number);
	infos = XRRGetMonitors(menuctx->display, menuctx->root_window, True, &nmons);
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
	max_height = monitor.height - menuctx->outer_border_width * 2;

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

	menu->curr_screenful.first_entry = menu->last_screenful.first_entry = 0;
	if (menu->tearable)
		menu->curr_screenful.offset = 0;
	else
		menu->curr_screenful.offset = get_entry_height(menuctx, TEAR_OFF);
	menu->last_screenful.offset = menu->curr_screenful.offset;
	xgap = menuctx->outer_border_width;
	ygap = -menuctx->inner_border_width;
	if (menu->tearable)
		ygap -= menuctx->separatorh;
	menu->total_height = menuctx->inner_border_width * 2;
	if (menu->tearable)
		menu->total_height += menuctx->separatorh;
	menu->window_geometry.height = menuctx->inner_border_width * 2;
	menu->window_geometry.width = 0;
	for (size_t entryc = 0; entryc < menu->nentries; entryc++) {
		struct ctrlmenu_entry *entry = &menu->entries[entryc];
		unsigned int textw;

		if (entry->type != CTRLMENU_SEPARATOR) {
			menu->total_height += menuctx->entryh;
			textw = ctrlfnt_width(
				menuctx->fontset,
				entry->label,
				strlen(entry->label)
			);
		} else {
			textw = 0;
			menu->total_height += menuctx->separatorh;
		}
		if (entry->nicons > 0)
			menu->hasicon = true;
		if (entry->type & CTRLMENU_CASCADE)
			menu->hassubmenu = true;
		menu->window_geometry.width = MAX(menu->window_geometry.width, textw);
		if (menu->overflow) {
			menu->last_screenful.offset += get_entry_height(
				menuctx,
				&menu->entries[menu->last_screenful.first_entry]
			);
			menu->last_screenful.first_entry++;
			continue;
		}
		if (menu->total_height + menuctx->entryh <= max_height) {
			menu->window_geometry.height = menu->total_height;
		} else {
			menu->overflow = true;
			menu->window_geometry.height = max_height;
		}
	}
	if (menu->overflow)
		menu->total_height += menuctx->separatorh * 2;

	menu->window_geometry.width += PADDING * 2 + menuctx->inner_border_width * 2;
	if (menu->hasicon)
		menu->window_geometry.width += menuctx->iconsize + PADDING;
	if (menu->hassubmenu)
		menu->window_geometry.width += TRIANGLE_WIDTH(menuctx) + TRIANGLE_PAD(menuctx) + PADDING;
	menu->window_geometry.width = MIN(menu->window_geometry.width, monitor.width/2);

	/* place menu to align with parent menu and be visible on monitor */
	if (menu->overflow && menu->next != NULL)
		ygap -= menuctx->separatorh;
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

	if (menu->window == None) {
		menu->window = XCreateWindow(
			menuctx->display, menuctx->root_window,
			menu->window_geometry.x, menu->window_geometry.y,
			menu->window_geometry.width,
			menu->window_geometry.height,
			menuctx->outer_border_width,
			menuctx->depth, InputOutput, menuctx->visual,
			CWBackPixel|CWColormap|CWBorderPixel|CWSaveUnder,
			&(XSetWindowAttributes){
				.border_pixel = 0,
				.background_pixel = 0,
				.colormap = menuctx->colormap,
				.save_under = True,
				.override_redirect = !menu->tornoff
			}
		);
	}

	/*
	 * Wrap XSelectInput(3) around with no input to avoid getting
	 * spurious events that we generated ourselves
	 */
	(void)XSelectInput(menuctx->display, menu->window, 0);
	(void)XSync(menuctx->display, False);
	(void)XConfigureWindow(
		menuctx->display, menu->window,
		CWX|CWY|CWWidth|CWHeight|CWBorderWidth,
		&(XWindowChanges){
			.x = menu->window_geometry.x,
			.y = menu->window_geometry.y,
			.width = menu->window_geometry.width,
			.height = menu->window_geometry.height,
			.border_width = menuctx->outer_border_width,
		}
	);
	(void)XChangeWindowAttributes(
		menuctx->display, menu->window, CWOverrideRedirect,
		&(XSetWindowAttributes){
			.override_redirect = !menu->tornoff
		}
	);
	if (menuctx->leader != None) {
		(void)XSetTransientForHint(
			menuctx->display, menu->window, menuctx->leader
		);
	}
	(void)XSetWMProtocols(
		menuctx->display,
		menu->window,
		(Atom[]){XInternAtom(menuctx->display, "WM_DELETE_WINDOW", False)},
		1
	);
	(void)XmbSetWMProperties(
		menuctx->display,
		menu->window,
		title, title,
		ctrlapp_getargv(),
		ctrlapp_getargc(),
		&(XSizeHints){ .flags = USPosition|USSize },
		NULL,
		&(XClassHint){
			.res_class = (char *)ctrlapp_getclass(),
			.res_name = (char *)ctrlapp_getname(),
		}
	);
	(void)XChangeProperty(
		menuctx->display,
		menu->window,
		XInternAtom(menuctx->display, "_NET_WM_NAME", False),
		XInternAtom(menuctx->display, "UTF8_STRING", False),
		8,
		PropModeReplace,
		(unsigned char *)title,
		strlen(title)
	);
	(void)XChangeProperty(
		menuctx->display,
		menu->window,
		XInternAtom(menuctx->display, "_NET_WM_WINDOW_TYPE", False),
		XA_ATOM,
		32,
		PropModeReplace,
		(void *)(Atom[]){XInternAtom(
			menuctx->display,
			menu->tornoff
			? "_NET_WM_WINDOW_TYPE_MENU"
			: "_NET_WM_WINDOW_TYPE_POPUP_MENU",
			False
		)}, 1
	);

	for (size_t i = 0; i < CANVAS_LAST; i++) {
		if (menu->canvas[i].pixmap == None ||
		    menu->canvas[i].picture == None) {
			canvas_reset(menuctx, menu);
			break;
		}
	}
	canvas_commit(menuctx, menu, menu->selected);
	XMapRaised(menuctx->display, menu->window);
	(void)XSelectInput(
		menuctx->display, menu->window,
		KeyPressMask | StructureNotifyMask |
		EnterWindowMask | LeaveWindowMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask
	);
	(void)XSync(menuctx->display, False);
}

static void
menu_untear(Display *display, struct ctrlmenu *menu)
{
	XUnmapWindow(display, menu->window);
	menu->tornoff = false;
}

static void
menu_tear_off(struct ctrlmenu_context *menuctx, struct ctrlmenu *menu,
		int x, int y)
{
	y += get_entry_height(menuctx, TEAR_OFF);
	y += menuctx->inner_border_width + menuctx->outer_border_width;
	menu->tornoff = true;
	menu_map(menuctx, menu, x, y, 0);
}

static int
menu_run(struct ctrlmenu_context *menuctx,
		struct ctrlmenu *parent, struct ctrlmenu *orig_menu,
		int x_root, int y_root, enum run_flags flags, Time time)
{
	static int MOTION_TIMEOUT = 300;
	static XMotionEvent last_motion = {};
	static int timeout = 0;
	struct ctrlmenu_entry *prevselected;
	struct ctrlmenu buf_menu = {};
	struct ctrlmenu *menu = &buf_menu;
	XEvent xev;
	bool close_all = false;
	jmp_buf jmpenv;

	if (flags & RUN_SELECT)
		orig_menu->selected = select_first_entry(menuctx, orig_menu);
	else
		orig_menu->selected = NULL;
	orig_menu->canvas = orig_menu->canvas_orig;
	if (flags & RUN_POPUP) {
		unsigned int width = 0;

		*menu = (struct ctrlmenu){
			.window = None,
			.title = orig_menu->title,
			.entries = orig_menu->entries,
			.nentries = orig_menu->nentries,
			.selected = orig_menu->selected,
			.canvas = orig_menu->canvas_orig,
			.caller = (parent != NULL) ? parent->selected : NULL,
			.tornoff = false,
			.tearable = !(flags & RUN_UNTEARABLE) && menuctx->tearoff,
		};
		if (XGrabPointer(
			menuctx->display, menuctx->root_window, True,
			ButtonPressMask|PointerMotionMask,
			GrabModeAsync, GrabModeAsync,
			None, menuctx->cursor, time
		) != GrabSuccess)
			return true;
		if (XGrabKeyboard(
			menuctx->display, menuctx->root_window, True,
			GrabModeAsync, GrabModeAsync, time
		) != GrabSuccess)
			return true;
		if (parent != NULL) {
			x_root = parent->window_geometry.x;
			y_root = parent->window_geometry.y + get_entry_ypos(
				menuctx, parent, parent->selected
			);
			if (!parent->tearable)
				y_root -= get_entry_height(menuctx, TEAR_OFF);
			if (parent->tornoff)
				y_root -= menuctx->outer_border_width;
			width = parent->window_geometry.width;
		}
		menu_map(menuctx, menu, x_root, y_root, width);
	} else {
		menu = orig_menu;
		canvas_commit(menuctx, menu, menu->selected);
	}
	menu->next = parent;
	prevselected = menu->selected;
	if (setjmp(jmpenv)) {
		close_all = true;
		goto done;
	}
	for (;;) {
		if (!next_event(menuctx->display, &xev, menu, &timeout, flags&RUN_POPUP))
			goto done;
		switch (xev.type) {
		case ButtonPress:
			if (xev.xbutton.button > Button3)
				break;
			if (xev.xbutton.window == menuctx->root_window) {
				close_all = true;
				goto done;
			}
			break;
		case ButtonRelease:
		{
			if (xev.xbutton.button > Button3)
				break;
			if (menu->next != NULL &&
			    xev.xbutton.window == menu->next->window) {
				struct ctrlmenu_entry *above;

				above = get_entry(
					menuctx, menu->next, xev.xmotion.y
				);
				if (above == menu->caller) {
					/*
					 * Clicking, on ancestor menu, on entry
					 * which triggers current menu.
					 */
					if (!(flags&RUN_ENTERED))
						flags |= RUN_ENTERED;
					else goto done;
					break;
				}
			}
			if (xev.xbutton.window != menu->window) {
				XPutBackEvent(menuctx->display, &xev);
				goto done;
			}
			menu->selected = get_entry(menuctx, menu, xev.xbutton.y);
			if (menu->selected == TEAR_OFF) {
				if (orig_menu->tornoff) {
					menu_untear(menuctx->display, orig_menu);
				} else {
					menu_tear_off(
						menuctx, orig_menu,
						menu->window_geometry.x,
						menu->window_geometry.y
					);
				}
				close_all = true;
				goto done;
			}
			canvas_commit(menuctx, menu, menu->selected);
			if (menu->selected == NULL)
				break;
			activate_entry(
				menuctx, jmpenv,
				menu, menu->selected, RUN_ENTERED,
				(xev.xbutton.state & ShiftMask) ||
				(xev.xbutton.button == Button2),
				xev.xbutton.time
			);
			break;
		}
		case ConfigureNotify:
			if (xev.xconfigure.window != menu->window) {
				XPutBackEvent(menuctx->display, &xev);
				goto done;
			}
			menu->window_geometry.x = xev.xconfigure.x;
			menu->window_geometry.y = xev.xconfigure.y;
			if (menu->window_geometry.width  == xev.xconfigure.width &&
			    menu->window_geometry.height == xev.xconfigure.height)
				break;       /* size not changed */
			menu->window_geometry.width = xev.xconfigure.width;
			menu->window_geometry.height = xev.xconfigure.height;
			canvas_reset(menuctx, menu);
			canvas_commit(menuctx, menu, menu->selected);
			break;
		case KeyPress:
			KeySym ksym = XkbKeycodeToKeysym(menuctx->display, xev.xkey.keycode, 0, 0);

			timeout = MOTION_TIMEOUT;
			if (ksym == XK_Tab && FLAG(xev.xkey.state, ShiftMask))
				ksym = XK_ISO_Left_Tab;
			if (ksym == XK_Up || ksym == XK_KP_Up || ksym == XK_ISO_Left_Tab) {
				if (flags & RUN_ENTERED) {
					menu->selected = select_prev_entry(menuctx, menu);
				} else if (menu->next != NULL) {
					/* navigating on parent menu */
					XPutBackEvent(menuctx->display, &xev);
					goto done;
				}
			} else if (ksym == XK_Down || ksym == XK_KP_Down || ksym == XK_Tab) {
				if (flags & RUN_ENTERED) {
					menu->selected = select_next_entry(menuctx, menu);
				} else if (menu->next != NULL) {
					/* navigating on parent menu */
					XPutBackEvent(menuctx->display, &xev);
					goto done;
				}
			} else if (ksym == XK_Home || ksym == XK_KP_Home) {
				if (flags & RUN_ENTERED) {
					menu->selected = select_first_entry(menuctx, menu);
				} else if (menu->next != NULL) {
					/* navigating on parent menu */
					XPutBackEvent(menuctx->display, &xev);
					goto done;
				}
			} else if (ksym == XK_End || ksym == XK_KP_End) {
				if (flags & RUN_ENTERED) {
					menu->selected = select_last_entry(menuctx, menu);
				} else if (menu->next != NULL) {
					/* navigating on parent menu */
					XPutBackEvent(menuctx->display, &xev);
					goto done;
				}
			} else if (ksym == XK_Right || ksym == XK_KP_Right ||
			           ksym == XK_Return || ksym == XK_KP_Enter) {
				if (!(flags & RUN_ENTERED) && menu->next != NULL &&
				    menu->next->selected != NULL &&
				    (menu->next->selected->type & CTRLMENU_CASCADE) &&
				    menu->caller == menu->next->selected) {
					/* navigating on parent menu to current one */
					menu->selected = select_first_entry(menuctx, menu);
					break;
				}
				if (!(flags & RUN_ENTERED) && menu->next != NULL) {
					/* navigating on parent menu */
					XPutBackEvent(menuctx->display, &xev);
					goto done;
				}
				if (menu->selected == NULL)
					break;
				if (menu->selected->type == CTRLMENU_COMMAND &&
				    (ksym == XK_Right || ksym == XK_KP_Right))
					break;
				activate_entry(
					menuctx, jmpenv,
					menu, menu->selected,
					RUN_ENTERED|RUN_SELECT,
					xev.xkey.state & ShiftMask,
					xev.xkey.time
				);
			} else if (ksym == XK_Escape) {
				close_all = true;
				goto done;
			} else if (ksym == XK_Left || ksym == XK_KP_Left) {
				if (menu->next == NULL)
					break;
				if (menu->selected == NULL) {
					XPutBackEvent(menuctx->display, &xev);
					goto done;
				}
				if (flags & RUN_ENTERED) {
					/* navigate on parent menu */
					menu->selected = NULL;
					flags &= ~RUN_ENTERED;
					break;
				}
			}
			break;
		case EnterNotify:
			if (xev.xcrossing.window != menu->window)
				break;
			if (!(flags&RUN_ENTERED)) {
				flags |= RUN_ENTERED;
				break;
			}
			break;
		case LeaveNotify:
			if (flags & RUN_POPUP) {
				menu->selected = NULL;
				break;
			}
			if (xev.xcrossing.window == menu->window)
				goto done;
			break;
		case MotionNotify:
			for (struct ctrlmenu *m = menu; m->next != NULL; m = m->next) {
				struct ctrlmenu_entry *above;

				if (xev.xmotion.window != m->next->window)
					continue;
				above = get_entry(
					menuctx, m->next, xev.xmotion.y
				);
				if (above == NULL)
					continue;
				if (above == m->caller)
					break;
				/* user selected another entry on parent menu */
				if (xev.xmotion.window == last_motion.window &&
				    (double)(xev.xmotion.x - last_motion.x)/
				    (xev.xmotion.time - last_motion.time) < 0.01) {
					/*
					 * Current menu has not been entered yet;
					 * Motion is not quick rightwards movement.
					 */
					m->next->selected = above;
					canvas_commit(
						menuctx, m->next,
						m->next->selected
					);
					XPutBackEvent(menuctx->display, &xev);
					goto done;
				} else {
					/*
					 * Current menu has not been entered yet;
					 * Motion is a quick rightwards movement,
					 * that we should ignore (user may be
					 * aiming an entry on current menu)
					 */
					timeout = MOTION_TIMEOUT;
					break;
				}
			}
			if (xev.xmotion.window == menu->window) {
				struct ctrlmenu_entry *entry;
				entry = get_entry(menuctx, menu, xev.xmotion.y);
				if (entry == SCROLL_UP || entry == SCROLL_DOWN)
					scroll_loop(menuctx, menu, entry);
				menu->selected = entry;
			}
			last_motion = xev.xmotion;
			break;
		case TimeoutNotify:
			/*
			 * select entry below pointer after a delay, to
			 * allow user move pointer into subwindow.
			 */
			if (menu->next != NULL &&
			    last_motion.window == menu->next->window) {
				struct ctrlmenu_entry *pointed_entry;

				pointed_entry = get_entry(
					menuctx, menu->next, last_motion.y
				);
				if (menu->next->selected == pointed_entry)
					break;
				menu->next->selected = pointed_entry;
				canvas_commit(
					menuctx, menu->next,
					menu->next->selected
				);
				timeout = MOTION_TIMEOUT;
				goto done;
			}

			/*
			 * no submenu is being previewed and a time has passed;
			 * preview a submenu if possible
			 */
			if ((flags & RUN_ENTERED) &&
			    menu->selected != NULL &&
			    (menu->selected->type & CTRLMENU_CASCADE)) {
				activate_entry(
					menuctx, jmpenv,
					menu, menu->selected,
					0, 0, last_motion.time
				);
			}
			break;
		case CirculateNotify:
		case DestroyNotify:
		case GravityNotify:
		case MapNotify:
		case ReparentNotify:
		case UnmapNotify:
			/*
			 * By selecting StructureNotifyMask on menu
			 * windows, we get all these events that we
			 * dont care on those windows.
			 */
			continue;
		default:
			if (flags & RUN_POPUP)
				break;
			goto done;
		}

		if (menu->selected != prevselected) {
			canvas_commit(menuctx, menu, menu->selected);
			prevselected = menu->selected;
			timeout = MOTION_TIMEOUT;
		}
	}
done:
	menu->selected = NULL;
	canvas_commit(menuctx, menu, menu->selected);
	if (buf_menu.window != None) {
		XSelectInput(menuctx->display, buf_menu.window, 0);
		XSync(menuctx->display, false);
		XDestroyWindow(menuctx->display, buf_menu.window);
	}
	menu->next = NULL;
	return close_all;
}


/*
 * context routines
 */

static void
context_cleanup(struct ctrlmenu_context *menuctx)
{
	if (menuctx->cursor != None)
		XFreeCursor(menuctx->display, menuctx->cursor);
	if (menuctx->fontset != NULL)
		ctrlfnt_free(menuctx->fontset);
	for (int color = 0; color < COLOR_LAST; color++) {
		if (menuctx->colors[color].pict != None)
			XRenderFreePicture(menuctx->display, menuctx->colors[color].pict);
		if (menuctx->colors[color].pix != None)
			XFreePixmap(menuctx->display, menuctx->colors[color].pix);
	}
}

static int
context_setup(struct ctrlmenu_context *menuctx, void *ctx, Display *display,
		Drawable reference, Window leader, XrmDatabase xdb)
{
	XWindowAttributes attributes;

	*menuctx = (struct ctrlmenu_context){0};
	if (reference == None)
		goto error;
	if (!XGetWindowAttributes(display, reference, &attributes))
		goto error;
	*menuctx = (struct ctrlmenu_context){
		.ctx = ctx,
		.display = display,
		.cursor = XCreateFontCursor(display, XC_left_ptr),
		.screen_number = XScreenNumberOfScreen(attributes.screen),
		.root_window = attributes.root,
		.visual = attributes.visual,
		.depth = attributes.depth,
		.colormap = attributes.colormap,
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

	for (size_t color = 0; color < COLOR_LAST; color++) {
		int retval;
		retval = create_picture(
			display,
			menuctx->root_window, menuctx->visual, menuctx->depth,
			&menuctx->colors[color].pict,
			&menuctx->colors[color].pix,
			&menuctx->colors[color].chans,
			false
		);
		if (retval == RETURN_FAILURE)
			goto error;
	}
	setfont(menuctx, NULL, 0.0);
	reload_resources(menuctx, xdb);
	if (menuctx->fontset == NULL)
		goto error;

	return RETURN_SUCCESS;

error:
	context_cleanup(menuctx);
	return RETURN_FAILURE;
}


/*
 * exported routines
 */

void
ctrlmenu_close(Display *display, struct ctrlmenu *menu)
{
	if (menu->window != None) {
		XSelectInput(display, menu->window, 0);
		XSync(display, false);
		XDestroyWindow(display, menu->window);
	}
	menu->window = None;
	for (size_t i = 0; i < CANVAS_LAST; i++) {
		if (menu->canvas_orig[i].picture != None)
			XRenderFreePicture(display, menu->canvas_orig[i].picture);
		if (menu->canvas_orig[i].pixmap != None)
			XFreePixmap(display, menu->canvas_orig[i].pixmap);
		menu->canvas_orig[i].pixmap = None;
		menu->canvas_orig[i].picture = None;
	}
	*menu = (struct ctrlmenu){
		.entries = menu->entries,
		.nentries = menu->nentries,
	};
}

void
ctrlmenu_handle_event(void *ctx, Display *display, XrmDatabase xdb,
		Window leader, XEvent *event, struct ctrlmenu *menu)
{
	if (event->type == ClientMessage) {
		if (event->xclient.window != menu->window) {
			XPutBackEvent(display, event);
			return;
		}
		if (event->xclient.message_type != XInternAtom(
			display, "WM_PROTOCOLS", false
		))
			return;
		if ((Atom)event->xclient.data.l[0] != XInternAtom(
			display, "WM_DELETE_WINDOW", false
		))
			return;
		menu_untear(display, menu);
		return;
	} else {
		struct ctrlmenu_context menuctx;

		if (context_setup(
			&menuctx, ctx, display, menu->window, leader, xdb
		) == RETURN_FAILURE)
			return;
		XPutBackEvent(display, event);
		(void)menu_run(
			&menuctx, NULL, menu, 0, 0,
			RUN_ENTERED, CurrentTime
		);
		XUngrabPointer(display, CurrentTime);
		XUngrabKeyboard(display, CurrentTime);
		context_cleanup(&menuctx);
	}
}

void
ctrlmenu_popup(void *ctx, Display *display, Window reference,
		XrmDatabase xdb, Window leader, int x_root, int y_root,
		Time timestamp, struct ctrlmenu *menu)
{
	struct ctrlmenu_context menuctx;

	if (context_setup(
		&menuctx, ctx, display, reference, leader, xdb
	) == RETURN_FAILURE)
		return;

	(void)menu_run(
		&menuctx, NULL, menu, x_root, y_root,
		RUN_POPUP|RUN_ENTERED, timestamp
	);
	XUngrabPointer(display, CurrentTime);
	XUngrabKeyboard(display, CurrentTime);
	context_cleanup(&menuctx);
}

void
ctrlmenu_tearoff(Display *display, Window reference,
		XrmDatabase xdb, Window leader, int x_root, int y_root,
		struct ctrlmenu *menu)
{
	struct ctrlmenu_context menuctx;

	if (context_setup(
		&menuctx, NULL, display, reference, leader, xdb
	) == RETURN_FAILURE)
		return;
	menu->tornoff = true;
	menu->canvas = menu->canvas_orig;
	menu_map(&menuctx, menu, x_root, y_root, 0);
	context_cleanup(&menuctx);
}
