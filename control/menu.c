#include <err.h>
#include <errno.h>
#include <limits.h>
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
static ssize_t TEAROFF    = -100;
static ssize_t SCROLLUP   = -200;
static ssize_t SCROLLDOWN = -300;

enum {
	DIR_UP,
	DIR_DOWN,
	DIR_LEFT,
	DIR_RIGHT
};

/*
 * resource creators
 */

static Window
create_window(struct widget *widget, XRectangle *geometry, long eventmask, bool override)
{
	return XCreateWindow(
		widget->app->display,
		widget->app->root_window,
		geometry != NULL ? geometry->x : 0,
		geometry != NULL ? geometry->y : 0,
		geometry != NULL ? geometry->width : 1,
		geometry != NULL ? geometry->height : 1,
		widget->outer_border_width,
		widget->app->depth,
		InputOutput,
		widget->app->visual,
		CWBackPixel | CWEventMask | CWColormap |
		CWBorderPixel | CWOverrideRedirect | CWSaveUnder,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.background_pixel = 0,
			.colormap = widget->app->colormap,
			.event_mask = eventmask,
			.save_under = True,
			.override_redirect = override,
		}
	);
}

static int
create_picture(struct widget *widget,
	Picture *picture, Pixmap *pixmap, XRenderColor *color, bool isalpha)
{
	*pixmap = XCreatePixmap(
		widget->app->display,
		widget->app->root_window,
		1, 1,
		isalpha ? 8 : widget->app->depth
	);
	if (*pixmap == None) {
		warnx("could not create pixmap");
		return RETURN_FAILURE;
	}
	*picture = XRenderCreatePicture(
		widget->app->display,
		*pixmap,
		isalpha
			? XRenderFindStandardFormat(widget->app->display, PictStandardA8)
			: XRenderFindVisualFormat(widget->app->display, widget->app->visual),
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
		widget->app->display,
		PictOpSrc,
		*picture,
		color,
		0, 0, 1, 1
	);
	return RETURN_SUCCESS;
}

/*
 * resource parsers and setters
 */

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
	struct ctrlfnt *fontset;

	if (facename == NULL)
		facename = "xft:";
	fontset = ctrlfnt_open(
		widget->app->display,
		widget->app->screen_number,
		widget->app->visual,
		widget->app->colormap,
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
	if (!XParseColor(widget->app->display, widget->app->colormap, value, &xcolor)) {
		warnx("%s: unkwnown color name", value);
		return false;
	}
	hsy = to_hsy(rgb = (struct rgb){
		.red = (double)xcolor.red / 0xFFFF,
		.green = (double)xcolor.green / 0xFFFF,
		.blue = (double)xcolor.blue / 0xFFFF,
	});

	setcolor(widget->app->display, &widget->colors[COLOR_BG], rgb);
	if (hsy.luma < 0.5) {
		/* dark background; make text white */
		setcolor(widget->app->display, &widget->colors[COLOR_FG], (struct rgb){1,1,1});
	} else {
		/* light background; make text black */
		setcolor(widget->app->display, &widget->colors[COLOR_FG], (struct rgb){0,0,0});
	}
	setcolor(widget->app->display, &widget->colors[COLOR_TOP], to_rgb((struct hsy){
		hsy.hue, hsy.saturation, hsy.luma*0.5 + 0.5
	}));
	setcolor(widget->app->display, &widget->colors[COLOR_BOT], to_rgb((struct hsy){
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
	if (!XParseColor(widget->app->display, widget->app->colormap, value, &xcolor))
		return false;
	setcolor(widget->app->display, color, (struct rgb){
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
		widget->app->display,
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
reload_resources(struct widget *widget, const char *str)
{
	XrmDatabase xdb = NULL;
	char const *fontspec = NULL;
	double fontsize = 0.0;
#define MK_RES(n, c, p, fun) (struct ctrlres){\
	.name_path  = (char *[]){widget->app->name, n},\
	.class_path = (char *[]){widget->app->class, c},\
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
		(XrmDatabase[]){widget->app->resourcesdb, xdb}, 2,
		resources, LEN(resources)
	);
	if (fontspec != NULL)
		setfont(widget, fontspec, fontsize);
	XrmDestroyDatabase(xdb);
}


/*
 * entry positioning routines
 */

static ssize_t
get_entry_index(struct widget *widget, struct menu *menu, long y)
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
get_first_entry_ypos(struct widget *widget, struct menu *menu)
{
	int y;

	y = widget->inner_border_width;
	if (menu->tearable)
		y += widget->separatorh;
	if (menu->overflow)
		y += widget->separatorh;
	return y;
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


/*
 * drawing routines
 */

static void
draw_shadows(struct widget *widget, Picture picture, XRectangle *geometry)
{
	for (int i = 0; i < widget->inner_border_width; i++) {
		/* draw light shadow */
		XRenderFillRectangle(
			widget->app->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_TOP].chans,
			i, i,
			1, geometry->height - (i * 2 + 1)
		);
		XRenderFillRectangle(
			widget->app->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_TOP].chans,
			i, i,
			geometry->width - (i * 2 + 1), 1
		);

		/* draw dark shadow */
		XRenderFillRectangle(
			widget->app->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_BOT].chans,
			geometry->width - 1 - i, i,
			1, geometry->height - i * 2
		);
		XRenderFillRectangle(
			widget->app->display,
			PictOpSrc,
			picture,
			&widget->colors[COLOR_BOT].chans,
			i, geometry->height - 1 - i,
			geometry->width - i * 2, 1
		);
	}
}

static void
draw_dashline(struct widget *widget, Picture picture, int width, int y)
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
				widget->app->display,
				PictOpSrc,
				picture,
				&widget->colors[COLOR_TOP].chans,
				toprects,
				nrects
			);
			XRenderFillRectangles(
				widget->app->display,
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
draw_separator(struct widget *widget, Picture picture, XRectangle *rect)
{
	XRenderFillRectangle(
		widget->app->display,
		PictOpSrc,
		picture,
		&widget->colors[COLOR_BOT].chans,
		widget->inner_border_width + PADDING,
		rect->y + widget->separatorh / 2 - 1,
		rect->width - widget->inner_border_width * 2 - PADDING * 2,
		1
	);
	XRenderFillRectangle(
		widget->app->display,
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
draw_triangle(struct widget *widget, Picture picture, Picture src, int x, int y, int direction)
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
		widget->app->display,
		PictOpOver,
		src,
		picture,
		XRenderFindStandardFormat(widget->app->display, PictStandardA8),
		0, 0,
		&triangle,
		1
	);
}

static void
draw_menu(struct widget *widget, struct menu *menu)
{
	XRectangle entry_geometry;
	int textx, textw, y;
	// int iconw, iconh;

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
					widget->app->display,
					menu->canvas[i].picture
				);
			}
			if (menu->canvas[i].pixmap != None)
				XFreePixmap(widget->app->display, menu->canvas[i].pixmap);
			menu->canvas[i].pixmap = XCreatePixmap(
				widget->app->display,
				menu->window,
				menu->canvas_width,
				menu->canvas_height,
				widget->app->depth
			);
			menu->canvas[i].picture = XRenderCreatePicture(
				widget->app->display,
				menu->canvas[i].pixmap,
				XRenderFindVisualFormat(widget->app->display, widget->app->visual),
				0,
				NULL
			);
			XRenderFillRectangle(
				widget->app->display,
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
			widget->app->display,
			PictOpSrc,
			widget->colors[i==CANVAS_SELECT ?  COLOR_SELBG : COLOR_BG].pict,
			widget->colors[COLOR_OPACITY].pict,
			menu->canvas[i].picture,
			0, 0,
			0, 0,
			widget->inner_border_width, widget->inner_border_width,
			menu->window_geometry.width - widget->inner_border_width * 2,
			menu->window_geometry.height - widget->inner_border_width * 2
		);
	}
	entry_geometry.x = widget->inner_border_width + PADDING;
	if (menu->hasicon)
		entry_geometry.x += widget->iconsize + PADDING;
	entry_geometry.y = get_first_entry_ypos(widget, menu);
	entry_geometry.width = menu->window_geometry.width;
	for (size_t entryc = 0; entryc < menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];
		//Imlib_Image image;

		if (entry->label == NULL) {
			entry_geometry.height = widget->separatorh;
			draw_separator(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				&entry_geometry
			);
			draw_separator(
				widget,
				menu->canvas[CANVAS_SELECT].picture,
				&entry_geometry
			);
			goto next;
		}
		entry_geometry.height = widget->entryh;
		//image = loadicon(
		//	widget,
		//	entry->file,
		//	widget->iconsize,
		//	&iconw,
		//	&iconh
		//);
		//if (image != NULL) {
		//	imlib_context_set_image(image);
		//	imlib_context_set_drawable(
		//		menu->canvas[CANVAS_NORMAL].pixmap
		//	);
		//	imlib_render_image_on_drawable(
		//		widget->inner_border_width + PADDING
		//		+ (widget->iconsize - iconw) / 2,
		//		entry_geometry.y + (widget->entryh - iconh) / 2
		//	);
		//	imlib_context_set_drawable(
		//		menu->canvas[CANVAS_SELECT].pixmap
		//	);
		//	imlib_render_image_on_drawable(
		//		widget->inner_border_width + PADDING
		//		+ (widget->iconsize - iconw) / 2,
		//		entry_geometry.y + (widget->entryh - iconh) / 2
		//	);
		//	imlib_free_image();
		//}
		if (entry->children != NULL) {
			draw_triangle(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				widget->colors[COLOR_FG].pict,
				entry_geometry.width - PADDING - TRIANGLE_WIDTH(widget) - TRIANGLE_PAD(widget)/2,
				entry_geometry.y + widget->entryh/2 - TRIANGLE_HEIGHT(widget)/2,
				DIR_RIGHT
			);
			draw_triangle(
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
			draw_dashline(
				widget,
				menu->canvas[CANVAS_NORMAL].picture,
				menu->window_geometry.width, y
			);
			draw_dashline(
				widget,
				menu->canvas[CANVAS_SELECT].picture,
				menu->window_geometry.width, y
			);
			y += widget->separatorh;
		}
		if (menu->overflow) {
			draw_triangle(
				widget,
				menu->canvas[i].picture,
				widget->colors[i==CANVAS_SELECT ?  COLOR_SELFG : COLOR_FG].pict,
				menu->window_geometry.width / 2 - TRIANGLE_HEIGHT(widget) / 2,
				y + widget->separatorh /2 - TRIANGLE_WIDTH(widget) / 2,
				DIR_UP
			);
			draw_triangle(
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
		widget->app->display,
		menu->window,
		menu->canvas[CANVAS_NORMAL].pixmap
	);
	XSetWindowBorderPixmap(widget->app->display, menu->window, widget->colors[COLOR_BORDER].pix);
	XClearWindow(widget->app->display, menu->window);
}

static void
draw_commit(struct widget *widget, struct menu *menu, ssize_t selected)
{
	int src_canvas_y, dst_canvas_y;

	XRenderFillRectangle(
		widget->app->display,
		PictOpClear,
		menu->canvas[CANVAS_FINAL].picture,
		&(XRenderColor){ 0 },
		0, 0,
		menu->window_geometry.width,
		menu->window_geometry.height
	);
	XRenderComposite(
		widget->app->display,
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
			widget->app->display,
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
			widget->app->display,
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
			widget->app->display,
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
			widget->app->display,
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
	draw_shadows(
		widget,
		menu->canvas[CANVAS_FINAL].picture,
		&menu->window_geometry
	);
	XSetWindowBackgroundPixmap(
		widget->app->display,
		menu->window,
		menu->canvas[CANVAS_FINAL].pixmap
	);
	XClearWindow(widget->app->display, menu->window);
}

/*
 * event getter
 */

static int
next_event(Display *display, XEvent *event, int *timeout)
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

	if (XPending(display) == 0) {
		if (poll(&pfd, 1, *timeout?*timeout:-1) == 0) {
			*timeout = 0;
			return TimeoutNotify;
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
		(void)XNextEvent(display, event);
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
		return MotionNotify;
	}
}


/*
 * entry scrolling routines
 */

static void
scroll_up(struct widget *widget, struct menu *menu, int diff)
{
	int height = 0;

	while (height < diff && menu->curr_screenful.first_entry > 0) {
		height = get_entry_height(widget, menu, menu->curr_screenful.first_entry);
		menu->curr_screenful.first_entry--;
		menu->curr_screenful.offset -= height;
	}
}

static void
scroll_down(struct widget *widget, struct menu *menu, int diff)
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

static void
scroll_loop(struct widget *widget, struct menu *menu, ssize_t nentry)
{
	static int const SCROLL_TIME = 32;
	int timeout = SCROLL_TIME;
	int last_offset = menu->curr_screenful.offset;

	for (;;) {
		XEvent event;

		switch (next_event(widget->app->display, &event, &timeout)) {
		case MotionNotify: case ButtonPress: case ButtonRelease:
			if (event.xmotion.window != menu->window)
				return;
			if (get_entry_index(widget, menu, event.xmotion.y) != nentry) {
				XPutBackEvent(widget->app->display, &event);
				return;
			}
			break;
		case TimeoutNotify:
			timeout = SCROLL_TIME;
			if (nentry == SCROLLDOWN)
				scroll_down(widget, menu, widget->entryh);
			else
				scroll_up(widget, menu, widget->entryh);
			if (menu->curr_screenful.offset != last_offset)
				draw_commit(widget, menu, nentry);
			last_offset = menu->curr_screenful.offset;
			XFlush(widget->app->display);
			break;
		default:
			return;
		}
	}
}


/*
 * entry selection routines
 */

static ssize_t
select_first_entry(struct widget *, struct menu *menu)
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
select_last_entry(struct widget *, struct menu *menu)
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

static ssize_t
select_prev_entry(struct widget *widget, struct menu *menu, ssize_t selected)
{
	int ypos, height;
	int scroll_threshold;

	if (selected < 0)
		return select_last_entry(widget, menu);
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
				scroll_up(widget, menu, height);
			}
		}
		if (entry->output != NULL || entry->nchildren > 0) {
			return entryc;
		}
	}
	return select_last_entry(widget, menu);
}

static ssize_t
select_next_entry(struct widget *widget, struct menu *menu, ssize_t selected)
{
	int ypos, height;
	int scroll_threshold = menu->window_geometry.height
	                     - widget->separatorh - widget->inner_border_width;

	if (selected < 0)
		return select_first_entry(widget, menu);
	ypos = get_entry_ypos(widget, menu, selected);
	height = get_entry_height(widget, menu, selected);
	for (ssize_t entryc = selected+1; entryc < (ssize_t)menu->nentries; entryc++) {
		struct entry *entry = &menu->entries[entryc];

		if (menu->overflow) {
			ypos += height;
			height = get_entry_height(widget, menu, entryc);
			if (ypos - menu->curr_screenful.offset > scroll_threshold) {
				scroll_down(widget, menu, height);
			}
		}
		if (entry->output != NULL || entry->nchildren > 0) {
			return entryc;
		}
	}
	return select_first_entry(widget, menu);
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
activate_entry(struct widget *widget, struct menu *menu, ssize_t nentry, bool alt)
{
	struct entry *entry;

	/* blink */
	draw_commit(widget, menu, -1);
	XSync(widget->app->display, False); xsleep(60000);
	draw_commit(widget, menu, nentry);
	XSync(widget->app->display, False); xsleep(60000);
	draw_commit(widget, menu, -1);
	XSync(widget->app->display, False); xsleep(60000);
	draw_commit(widget, menu, nentry);
	while (XCheckMaskEvent(
		widget->app->display,
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


/*
 * internal menu controlling routines
 */

static void
menu_map(struct widget *widget, struct menu *parent, struct menu *menu,
		struct entry *entries, size_t nentries,
		int x_root, int y_root, unsigned int button_width,
		bool windowed, Time time, char const *title)
{
	int max_height;
	int xgap, ygap;
	XRectangle monitor;
	XRRMonitorInfo *infos;
	int nmons = 0;

	if (!windowed) {
		if (XGrabPointer(
			widget->app->display, widget->app->root_window, True,
			ButtonPressMask|PointerMotionMask,
			GrabModeAsync, GrabModeAsync,
			None, widget->app->cursor, time
		) != GrabSuccess)
			return;
		if (XGrabKeyboard(
			widget->app->display, widget->app->root_window, True,
			GrabModeAsync, GrabModeAsync, time
		) != GrabSuccess)
			return;
	}
	monitor.x = 0;
	monitor.y = 0;
	monitor.width = DisplayWidth(widget->app->display, widget->app->screen_number);
	monitor.height = DisplayHeight(widget->app->display, widget->app->screen_number);
	infos = XRRGetMonitors(widget->app->display, widget->app->root_window, True, &nmons);
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

	*menu = (struct menu){
		.entries = entries,
		.nentries = nentries,
		.title = title,
		.selected = -1,
		.curr_screenful = {0, 0},
		.last_screenful = {0, 0},
		.next = parent,
		.tearable = !windowed && widget->tearoff,
		.tornoff = windowed,
	};
	xgap = widget->outer_border_width;
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
		if (entry->children != NULL)
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
	if (menu->overflow && menu->next != NULL)
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

	menu->window = create_window(
		widget, &menu->window_geometry,
		KeyPressMask | StructureNotifyMask |
		EnterWindowMask | LeaveWindowMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		menu->tornoff ? false : true
	);
	if (widget->leader != None) {
		(void)XSetTransientForHint(
			widget->app->display,
			menu->window,
			widget->leader
		);
	}
	(void)XSetWMProtocols(
		widget->app->display,
		menu->window,
		(Atom[]){XInternAtom(widget->app->display, "WM_DELETE_WINDOW", False)},
		1
	);
	(void)XmbSetWMProperties(
		widget->app->display,
		menu->window,
		title, title,
		widget->app->argv,
		widget->app->argc,
		&(XSizeHints){ .flags = USPosition|USSize },
		NULL,
		&(XClassHint){
			.res_class = widget->app->class,
			.res_name = widget->app->name,
		}
	);
	(void)XChangeProperty(
		widget->app->display,
		menu->window,
		XInternAtom(widget->app->display, "_NET_WM_NAME", False),
		XInternAtom(widget->app->display, "UTF8_STRING", False),
		8,
		PropModeReplace,
		(unsigned char *)title,
		strlen(title)
	);
	(void)XChangeProperty(
		widget->app->display,
		menu->window,
		XInternAtom(widget->app->display, "_NET_WM_WINDOW_TYPE", False),
		XA_ATOM,
		32,
		PropModeReplace,
		(void *)(Atom[]){XInternAtom(
			widget->app->display,
			menu->tornoff
			? "_NET_WM_WINDOW_TYPE_MENU"
			: "_NET_WM_WINDOW_TYPE_POPUP_MENU",
			False
		)}, 1
	);

	draw_menu(widget, menu);
	draw_commit(widget, menu, -1);
	XMapRaised(widget->app->display, menu->window);
}

static void
menu_unmap(struct widget *widget, struct menu *menu)
{
	if (menu == NULL || menu->tornoff || menu->window == None)
		return;
	XDestroyWindow(widget->app->display, menu->window);
	for (size_t i = 0; i < CANVAS_LAST; i++) {
		if (menu->canvas[i].picture != None)
			XRenderFreePicture(widget->app->display, menu->canvas[i].picture);
		if (menu->canvas[i].pixmap != None)
			XFreePixmap(widget->app->display, menu->canvas[i].pixmap);
	}
}

static void
menu_close(struct widget *widget, struct menu *menu)
{
	menu->tornoff = false;
	menu_unmap(widget, menu);
}

static void
menu_tear_off(struct widget *widget, struct menu *menu)
{
	struct menu *new_menu;

	new_menu = malloc(sizeof(*new_menu));
	if (new_menu == NULL) {
		warnx("%zu", sizeof(*new_menu));
		return;
	}
	(void)XChangeWindowAttributes(
		widget->app->display, menu->window, CWOverrideRedirect,
		&(XSetWindowAttributes){ .override_redirect = false, }
	);
	(void)XChangeProperty(
		widget->app->display, menu->window,
		XInternAtom(widget->app->display, "_NET_WM_WINDOW_TYPE", false),
		XA_ATOM, 32, PropModeReplace,
		(void *)(Atom[]){XInternAtom(
			widget->app->display, "_NET_WM_WINDOW_TYPE_MENU", false
		)}, 1
	);
	(void)XUnmapWindow(widget->app->display, menu->window);
	if (menu->tearable) {
		menu->window_geometry.x += widget->outer_border_width;
		menu->window_geometry.y += widget->outer_border_width;
		menu->window_geometry.y += widget->separatorh;
		menu->window_geometry.height -= widget->separatorh;
		XMoveResizeWindow(
			widget->app->display, menu->window,
			menu->window_geometry.x, menu->window_geometry.y,
			menu->window_geometry.width, menu->window_geometry.height
		);
	}
	menu->tearable = false;
	menu->tornoff = true;
	menu->selected = -1;
	draw_menu(widget, menu);
	draw_commit(widget, menu, menu->selected);
	(void)XMapWindow(widget->app->display, menu->window);

	*new_menu = *menu;
	new_menu->next = widget->menus;
	widget->menus = new_menu;
}

static int
menu_run(struct widget *widget, struct menu *menu, bool grabbed)
{
	static int MOTION_TIMEOUT = 300;
	struct menu submenu = {};
	XEvent xev;
	XMotionEvent last_motion = {0};
	struct entry *entry;
	ssize_t nentry;
	ssize_t prevselected = menu->selected;
	int timeout = 0;
	bool close_all = false;

	for (;;) {
		switch (next_event(widget->app->display, &xev, &timeout)) {
		case ButtonPress:
			if (xev.xbutton.button > Button3)
				break;
			if (xev.xbutton.window != menu->window) {
				XPutBackEvent(widget->app->display, &xev);
				goto done;
			}
			break;
		case ButtonRelease:
			if (xev.xbutton.button > Button3)
				break;
			if (xev.xbutton.window != menu->window) {
				XPutBackEvent(widget->app->display, &xev);
				goto done;
			}
			menu->selected = get_entry_index(widget, menu, xev.xbutton.y);
			if (menu->selected == TEAROFF) {
				menu_tear_off(widget, menu);
				xev.xbutton.window = None;
				close_all = true;
				goto done;
			}
			draw_commit(widget, menu, menu->selected);
			entry = get_entry(menu, menu->selected);
			if (entry == NULL)
				break;
			if (entry->children != NULL) {
				if (submenu.entries != entry->children) {
					if (submenu.window != None)
						menu_unmap(widget, &submenu);
					menu_map(
						widget, menu, &submenu,
						entry->children, entry->nchildren,
						menu->window_geometry.x,
						menu->window_geometry.y + get_entry_ypos(widget, menu, menu->selected),
						menu->window_geometry.width,
						false, xev.xbutton.time, entry->label
					);
				}
				goto recurse;
			} else if (entry->output != NULL) {
				activate_entry(
					widget, menu, menu->selected,
					(xev.xbutton.state & ShiftMask) ||
					(xev.xbutton.button == Button2)
				);
				close_all = true;
				goto done;
			}
			break;
		case ClientMessage:
			if (xev.xclient.window != menu->window) {
				XPutBackEvent(widget->app->display, &xev);
				goto done;
			}
			if (xev.xclient.message_type != XInternAtom(
				widget->app->display, "WM_PROTOCOLS", false
			))
				break;
			if ((Atom)xev.xclient.data.l[0] != XInternAtom(
				widget->app->display, "WM_DELETE_WINDOW", false
			))
				break;
			for (struct menu **ptr = &widget->menus;
			     *ptr != NULL;
			     ptr = &(*ptr)->next) {
				if (*ptr == menu) {
					*ptr = menu->next;
					menu_close(widget, menu);
					menu_unmap(widget, &submenu);
					free(menu);
					return true;
				}
			}
			break;
		case ConfigureNotify:
			if (xev.xconfigure.window != menu->window) {
				XPutBackEvent(widget->app->display, &xev);
				goto done;
			}
			menu->window_geometry.x = xev.xconfigure.x;
			menu->window_geometry.y = xev.xconfigure.y;
			if (menu->window_geometry.width  == xev.xconfigure.width &&
			    menu->window_geometry.height == xev.xconfigure.height)
				break;       /* size not changed */
			menu->window_geometry.width = xev.xconfigure.width;
			menu->window_geometry.height = xev.xconfigure.height;
			draw_menu(widget, menu);
			draw_commit(widget, menu, menu->selected);
			break;
		case KeyPress:
			KeySym ksym = XkbKeycodeToKeysym(widget->app->display, xev.xkey.keycode, 0, 0);

			if (ksym == XK_Tab && FLAG(xev.xkey.state, ShiftMask))
				ksym = XK_ISO_Left_Tab;
			if (ksym == XK_Up || ksym == XK_KP_Up || ksym == XK_ISO_Left_Tab) {
				menu->selected = select_prev_entry(widget, menu, menu->selected);
			} else if (ksym == XK_Down || ksym == XK_KP_Down || ksym == XK_Tab) {
				menu->selected = select_next_entry(widget, menu, menu->selected);
			} else if (ksym == XK_Home || ksym == XK_KP_Home) {
				menu->selected = select_first_entry(widget, menu);
			} else if (ksym == XK_End || ksym == XK_KP_End) {
				menu->selected = select_last_entry(widget, menu);
			} else if (ksym == XK_Right || ksym == XK_KP_Right ||
			           ksym == XK_Return || ksym == XK_KP_Enter) {
				entry = get_entry(menu, menu->selected);
				if (entry == NULL)
					break;
				if (entry->children != NULL) {
					if (submenu.entries != entry->children) {
						if (submenu.window != None)
							menu_unmap(widget, &submenu);
						menu_map(
							widget, menu, &submenu,
							entry->children, entry->nchildren,
							menu->window_geometry.x,
							menu->window_geometry.y + get_entry_ypos(widget, menu, menu->selected),
							menu->window_geometry.width,
							false, xev.xkey.time, entry->label
						);
					}
					submenu.selected = 0;
					draw_commit(widget, &submenu, submenu.selected);
					goto recurse;
				} else {
					activate_entry(
						widget, menu, menu->selected,
						xev.xkey.state & ShiftMask
					);
					xev.xkey.keycode = XKeysymToKeycode(
						widget->app->display,
						XK_Escape
					);
					close_all = true;
					goto done;
				}
			} else if (ksym == XK_Escape) {
				close_all = true;
				goto done;
			} else if (ksym == XK_Left || ksym == XK_KP_Left) {
				goto done;
			}
			break;
		case EnterNotify:
			if (xev.xcrossing.window != submenu.window)
				break;
		recurse:
			if (submenu.window == None)
				break;
			close_all = menu_run(widget, &submenu, true);
			if (close_all)
				goto done;
			break;
		case LeaveNotify:
			if (grabbed)
				break;
			if (xev.xcrossing.window == submenu.window ||
			    xev.xcrossing.window == menu->window) {
				goto done;
			}
			break;
		case MotionNotify:
			for (struct menu *m = menu; m->next != NULL; m = m->next) {
				struct entry *above;
				if (xev.xmotion.window != m->next->window)
					continue;
				above = get_entry(m->next, get_entry_index(
					widget, m->next, xev.xmotion.y
				));
				if (above != NULL && above->children == m->entries)
					break;
				/*
				 * Pointer on ancestor menu, over entry which
				 * does not descends into current menu.
				 */
				XPutBackEvent(widget->app->display, &xev);
				goto done;
			}
			if (xev.xmotion.window != menu->window) {
				if (submenu.window == None)
					menu->selected = -1;
				break;
			}
			nentry = get_entry_index(widget, menu, xev.xmotion.y);
			if (nentry == SCROLLUP || nentry == SCROLLDOWN)
				scroll_loop(widget, menu, nentry);
			if (submenu.window == None ||
			    (double)(xev.xmotion.x - last_motion.x)/
			    (xev.xmotion.time - last_motion.time) < 0.01) {
				menu->selected = nentry;
			}
			last_motion = xev.xmotion;
			break;
		case TimeoutNotify:
			ssize_t pointed_index = get_entry_index(widget, menu, last_motion.y);
			struct entry *sel_entry;

			/*
			 * submenu is being previewed; select entry
			 * below pointer after a delay, to allow user
			 * move pointer into subwindow.
			 */
			if (submenu.window != None &&
			    pointed_index != menu->selected) {
				menu->selected = pointed_index;
				timeout = MOTION_TIMEOUT;
			}

			/*
			 * no submenu is being previewed and a time has passed;
			 * preview a submenu if possible
			 */
			sel_entry = get_entry(menu, menu->selected);
			if (submenu.window == None && sel_entry != NULL &&
			    sel_entry->children != NULL) {
				menu_map(
					widget, menu, &submenu,
					sel_entry->children, sel_entry->nchildren,
					menu->window_geometry.x,
					menu->window_geometry.y + get_entry_ypos(widget, menu, menu->selected),
					menu->window_geometry.width,
					false, last_motion.time, sel_entry->label
				);
			}
			break;
		default:
			if (grabbed)
				break;
			if (xev.xany.window != menu->window &&
			    xev.xany.window != submenu.window) {
				XPutBackEvent(widget->app->display, &xev);
				goto done;
			}
			break;
		}
		struct entry *sel_entry = get_entry(menu, menu->selected);

		if (menu->selected != prevselected) {
			draw_commit(widget, menu, menu->selected);
			prevselected = menu->selected;
			timeout = MOTION_TIMEOUT;
		}

		/*
		 * selected entry changed; unmap previewed submenu
		 */
		if (sel_entry == NULL ||
		    submenu.entries != sel_entry->children) {
			menu_unmap(widget, &submenu);
			timeout = MOTION_TIMEOUT;
			submenu = (struct menu){
				.window = None,
				.entries = NULL,
			};
		}
	}
done:
	if (submenu.window != None)
		menu_unmap(widget, &submenu);
	menu->selected = -1;
	draw_commit(widget, menu, menu->selected);
	return close_all;
}


/*
 * exported routines
 */

void
ctrlmenu_popup(struct widget *widget, struct entry *entries, size_t nentries,
		int x_root, int y_root, bool windowed, Time time)
{
	struct menu menu;
	menu_map(
		widget, NULL, &menu, entries, nentries,
		x_root, y_root, 0, windowed, time, widget->app->title
	);
	(void)menu_run(widget, &menu, true);
	menu_unmap(widget, &menu);
	XUngrabPointer(widget->app->display, CurrentTime);
	XUngrabKeyboard(widget->app->display, CurrentTime);
}

void
ctrlmenu_handle_event(struct widget *widget, struct menu *menu, XEvent *event)
{
	XPutBackEvent(widget->app->display, event);
	(void)menu_run(widget, menu, false);
	XUngrabPointer(widget->app->display, CurrentTime);
	XUngrabKeyboard(widget->app->display, CurrentTime);
}

void
ctrlmenu_reload_resources(struct widget *widget, char const *str)
{
	reload_resources(widget, str);
	for (struct menu *ancestor = widget->menus;
	     ancestor != NULL;
	     ancestor = ancestor->next) {
		draw_commit(widget, ancestor, ancestor->selected);
	}
}

void
ctrlmenu_free(struct widget *widget)
{
	if (widget == NULL)
		return;
	if (widget->fontset != NULL)
		ctrlfnt_free(widget->fontset);
	for (int color = 0; color < COLOR_LAST; color++) {
		if (widget->colors[color].pict != None) {
			XRenderFreePicture(
				widget->app->display,
				widget->colors[color].pict
			);
		}
		if (widget->colors[color].pix != None) {
			XFreePixmap(
				widget->app->display,
				widget->colors[color].pix
			);
		}
	}
	free(widget);
}

struct widget *
ctrlmenu_init(struct ctrlapp *app, Window leader)
{
	struct widget *widget;

	if ((widget = malloc(sizeof(*widget))) == NULL)
		return NULL;
	*widget = (struct widget){
		.app = app,

		.leader = leader,
		.menus = NULL,

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
	XSelectInput(widget->app->display, widget->app->root_window, PropertyChangeMask);

	for (size_t color = 0; color < COLOR_LAST; color++) {
		int retval;
		retval = create_picture(
			widget,
			&widget->colors[color].pict,
			&widget->colors[color].pix,
			&widget->colors[color].chans,
			false
		);
		if (retval == RETURN_FAILURE)
			goto error;
	}
	reload_resources(widget, XResourceManagerString(widget->app->display));
	if (widget->fontset == NULL)
		setfont(widget, NULL, 0.0);
	if (widget->fontset == NULL)
		goto error;
	return widget;
error:
	ctrlmenu_free(widget);
	return NULL;
}
