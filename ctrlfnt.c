#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>
#include <fontconfig/fontconfig.h>

#include "ctrlfnt.h"

struct VArray {
	XftFont       **fonts;
	size_t          capacity;
	size_t          nmemb;
};

struct CtrlFontSet {
	Display        *display;
	int             screen;
	Visual         *visual;
	Colormap        colormap;
	struct VArray  *xft_fontset;
	XFontSet        xlfd_fontset;
	XFontStruct    *xlfd_font;
};

#define MAXGLYPHS 1024
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))

static XftFont *
openxftfont(Display *display, const char *fontname, double fontsize)
{
	FcPattern *pattern = NULL;
	FcPattern *match = NULL;
	FcResult result;
	XftFont *font = NULL;

	if ((pattern = FcNameParse((FcChar8 *)fontname)) == NULL)
		goto error;
	if (fontsize > 0.0)
		(void)FcPatternAddDouble(pattern, FC_SIZE, fontsize);
	FcDefaultSubstitute(pattern);
	if ((match = FcFontMatch(NULL, pattern, &result)) == NULL)
		goto error;
	if ((font = XftFontOpenPattern(display, match)) == NULL)
		goto error;
	FcPatternDestroy(pattern);
	FcPatternDestroy(match);
	return font;
error:
	warnx("%s: could not open font", fontname);
	if (pattern != NULL)
		FcPatternDestroy(pattern);
	if (match != NULL)
		FcPatternDestroy(match);
	if (font != NULL)
		XftFontClose(display, font);
	return NULL;
}

static int
addxftfont(struct VArray *fontset, XftFont *font)
{
	XftFont **fonts;

	if (font == NULL)
		return 0;
	if (fontset->nmemb <= fontset->capacity) {
		if (fontset->capacity == 0)
			fontset->capacity = 1;
		else
			fontset->capacity += 2;
		fonts = realloc(fontset->fonts, fontset->capacity * sizeof(*fonts));
		if (fonts == NULL)
			return -1;
		fontset->fonts = fonts;
	}
	fontset->fonts[fontset->nmemb++] = font;
	return 0;
}

static struct VArray *
openxftfontset(Display *display, const char *fontspec, double fontsize)
{
	struct VArray *fontset = NULL;
	XftFont *font = NULL;
	char *t, *last;
	char *s = NULL;

	if ((fontset = malloc(sizeof(*fontset))) == NULL)
		goto error;
	*fontset = (struct VArray){
		.fonts = NULL,
		.capacity = 0,
		.nmemb = 0,
	};
	if ((s = strdup(fontspec)) == NULL)
		goto error;
	if (fontspec[0] == '\0') {
		fontset->capacity = 1;
		fontset->nmemb = 1;
		fontset->fonts = malloc(sizeof(*fontset->fonts));
		if (fontset->fonts == NULL)
			goto error;
		fontset->fonts[0] = openxftfont(display, "", fontsize);
		if (fontset->fonts[0] == NULL)
			goto error;
		goto done;
	}
	for (t = strtok_r(s, ",", &last);
	     t != NULL;
	     t = strtok_r(NULL, ",", &last)) {
		if ((font = openxftfont(display, t, fontsize)) == NULL)
			continue;
		if (addxftfont(fontset, font) == -1)
			goto error;
	}
	if (fontset->nmemb == 0)
		goto error;
done:
	free(s);
	return fontset;
error:
	free(s);
	if (font != NULL)
		XftFontClose(display, font);
	if (fontset != NULL)
		free(fontset->fonts);
	free(fontset);
	return NULL;
}

static XFontSet
openxfontset(Display *display, const char *fontspec, int dowarn)
{
	XFontSet fontset;
	char **mc = NULL;
	int nmc;

	fontset = XCreateFontSet(display, fontspec, &mc, &nmc, NULL);
	XFreeStringList(mc);
	if (fontset == NULL)
		goto error;
	return fontset;
error:
	if (dowarn)
		warnx("%s: could not open fonts", fontspec);
	return NULL;
}

static XFontStruct *
openxfont(Display *display, const char *fontspec, int dowarn)
{
	XFontStruct *font;

	font = XLoadQueryFont(display, fontspec);
	if (font == NULL)
		goto error;
	return font;
error:
	if (dowarn)
		warnx("%s: could not open font", fontspec);
	return NULL;
}

static FcChar32
getnextutf8char(const char *s, const char **next_ret)
{
	static const unsigned char utfbyte[] = {0x80, 0x00, 0xC0, 0xE0, 0xF0};
	static const unsigned char utfmask[] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
	static const FcChar32 utfmin[] = {0, 0x00,  0x80,  0x800,  0x10000};
	static const FcChar32 utfmax[] = {0, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};
	/* 0xFFFD is the replacement character, used to represent unknown characters */
	static const FcChar32 unknown = 0xFFFD;
	FcChar32 ucode;         /* FcChar32 type holds 32 bits */
	size_t usize = 0;       /* n' of bytes of the utf8 character */
	size_t i;

	*next_ret = s+1;

	/* get code of first byte of utf8 character */
	for (i = 0; i < sizeof utfmask; i++) {
		if (((unsigned char)*s & utfmask[i]) == utfbyte[i]) {
			usize = i;
			ucode = (unsigned char)*s & ~utfmask[i];
			break;
		}
	}

	/* if first byte is a continuation byte or is not allowed, return unknown */
	if (i == sizeof utfmask || usize == 0)
		return unknown;

	/* check the other usize-1 bytes */
	s++;
	for (i = 1; i < usize; i++) {
		*next_ret = s+1;
		/* if byte is nul or is not a continuation byte, return unknown */
		if (*s == '\0' || ((unsigned char)*s & utfmask[0]) != utfbyte[0])
			return unknown;
		/* 6 is the number of relevant bits in the continuation byte */
		ucode = (ucode << 6) | ((unsigned char)*s & ~utfmask[0]);
		s++;
	}

	/* check if ucode is invalid or in utf-16 surrogate halves */
	if (!BETWEEN(ucode, utfmin[usize], utfmax[usize])
	    || BETWEEN (ucode, 0xD800, 0xDFFF))
		return unknown;

	return ucode;
}

static XftFont *
opennewfont(CtrlFontSet *fontset, FcChar32 glyph)
{
	XftFont *retfont = fontset->xft_fontset->fonts[0];
#ifndef CTRLFNT_NO_SEARCH
	FcCharSet *fccharset = NULL;
	FcPattern *fcpattern = NULL;
	FcPattern *match = NULL;
	XftFont *font = NULL;
	XftResult result;

	if ((fccharset = FcCharSetCreate()) == NULL)
		goto done;
	if (!FcCharSetAddChar(fccharset, glyph))
		goto done;
	if ((fcpattern = FcPatternCreate()) == NULL)
		goto done;
	if (!FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset))
		goto done;
	if (!FcConfigSubstitute(NULL, fcpattern, FcMatchPattern))
		goto done;
	FcDefaultSubstitute(fcpattern);
	if ((match = XftFontMatch(fontset->display, fontset->screen, fcpattern, &result)) == NULL)
		goto done;
	if ((font = XftFontOpenPattern(fontset->display, match)) == NULL)
		goto done;
	if (XftCharExists(fontset->display, font, glyph) == FcFalse)
		goto done;
	if (addxftfont(fontset->xft_fontset, font) == -1)
		goto done;
	retfont = font;
	font = NULL;
done:
	if (fccharset != NULL)
		FcCharSetDestroy(fccharset);
	if (fcpattern != NULL)
		FcPatternDestroy(fcpattern);
	if (match != NULL)
		XftPatternDestroy(match);
	if (font != NULL)
		XftFontClose(fontset->display, font);
#endif /* CTRLFNT_NO_SEARCH */
	(void)glyph;
	return retfont;
}

static XftFont *
getfontforglyph(CtrlFontSet *fontset, FcChar32 glyph)
{
	size_t i;

	for (i = 0; i < fontset->xft_fontset->nmemb; i++)
		if (XftCharExists(fontset->display, fontset->xft_fontset->fonts[i], glyph) == FcTrue)
			return fontset->xft_fontset->fonts[i];
	return opennewfont(fontset, glyph);
}

static size_t
getfontcoverage(CtrlFontSet *fontset, XftFont *font, FcChar32 *glyphs, size_t nglyphs)
{
	size_t i;

	for (i = 0; i < nglyphs; i++)
		if (!XftCharExists(fontset->display, font, glyphs[i]))
			return i;
	return i;
}

static int
utf8toxchar2b(XChar2b *glyphs, int maxglyphs, const char *text, int nbytes)
{
	int i, nglyphs;
	unsigned char c;
	/*
	 * see http://xopendisplay.hilltopia.ca/2009/Mar/Xlib-tutorial-part-8----a-different-way-to-reach-wide-characters.html
	 */

	for (i = nglyphs = 0; i < nbytes && nglyphs < maxglyphs; i++) {
		c = text[i];
		if (c < 128) {
			glyphs[nglyphs].byte1 = 0;
			glyphs[nglyphs].byte2 = c;
			nglyphs++;
			continue;
		} else if (c < 0xC0) {
			/* we're inside a character we don't know */
			continue;
		} else switch (c & 0xF0) {
		case 0xC0: case 0xD0:
			if (nbytes < i + 1)
				return nglyphs;
			i++;
			glyphs[nglyphs].byte1 = (c & 0x1C) >> 2;
			glyphs[nglyphs].byte2 = ((c & 0x03) << 6) + (text[i] & 0x3F);
			nglyphs++;
			break;
		case 0xE0:
			if (nbytes < i + 2)
				return nglyphs;
			i++;
			glyphs[nglyphs].byte1 = ((c & 0x0F) << 4) + ((text[i] & 0x3C) >> 2);
			c = text[i];
			i++;
			glyphs[nglyphs].byte2 = ((c & 0x03) << 6) + (text[i] & 0x3F);
			nglyphs++;
			break;
		case 0xFF:
			continue;
		}
	}
	return nglyphs;
}

static int
drawxftstring(CtrlFontSet *fontset, Picture picture, Picture src,
             XRectangle rect, const char *text, int nbytes)
{
	FT_UInt glyphs[MAXGLYPHS];
	XftFont *font;
	XGlyphInfo extents;
	const char *end = text;
	size_t nglyphs = 0;
	size_t nwritten = 0;
	size_t n = 0;
	int x = rect.x;
	int w = 0;

	if (nbytes == 0)
		return 0;
	while (end < text + nbytes && end < text + MAXGLYPHS)
		glyphs[nglyphs++] = getnextutf8char(end, &end);
	while (nwritten < nglyphs) {
		font = getfontforglyph(fontset, glyphs[nwritten]);
		n = 1 + getfontcoverage(
			fontset,
			font,
			glyphs + nwritten + 1,
			nglyphs - nwritten - 1
		);
		XftTextRender32(
			fontset->display,
			PictOpOver,
			src,
			font,
			picture,
			0, 0,
			x + w,
			rect.y + rect.height / 2
			       + font->ascent / 2
			       - font->descent / 2,

			glyphs + nwritten,
			n
		);
		XftTextExtents32(
			fontset->display,
			font,
			glyphs + nwritten,
			n,
			&extents
		);
		w += extents.width;
		nwritten += n;
	}
	return w;
}

static int
drawxmbstring(CtrlFontSet *fontset, Pixmap pix, GC gc, XRectangle rect,
              const char *text, int nbytes)
{
	XRectangle box, dummy;
	XmbTextExtents(fontset->xlfd_fontset, text, nbytes, &dummy, &box);
	XmbDrawString(
		fontset->display,
		pix,
		fontset->xlfd_fontset,
		gc,
		- box.x,
		rect.height / 2 - box.height / 2 - box.y,
		text,
		nbytes
	);
	return box.width;
}

static int
drawxstring(CtrlFontSet *fontset, Pixmap pix, GC gc, XRectangle rect,
            const char *text, int nbytes)
{
	XChar2b glyphs[MAXGLYPHS];
	int nglyphs;

	nglyphs = utf8toxchar2b(glyphs, MAXGLYPHS, text, nbytes);
	XSetFont(fontset->display, gc, fontset->xlfd_font->fid);
	XDrawString16(
		fontset->display,
		pix,
		gc,
		0,
		rect.height / 2
		+ fontset->xlfd_font->ascent / 2
		- fontset->xlfd_font->descent / 2,
		glyphs,
		nglyphs
	);
	return XTextWidth16(fontset->xlfd_font, glyphs, nglyphs);
}

static int
drawx(CtrlFontSet *fontset, Picture picture, Picture src,
      XRectangle rect, const char *text, int nbytes)
{
	Pixmap pix = None;
	Picture mask = None;
	GC gc = NULL;
	int retval;

	pix = XCreatePixmap(
		fontset->display,
		RootWindow(fontset->display, fontset->screen),
		rect.width,
		rect.height,
		1
	);
	if (pix == None)
		goto error;
	gc = XCreateGC(fontset->display, pix, 0, NULL);
	if (gc == NULL)
		goto error;
	mask = XRenderCreatePicture(
		fontset->display,
		pix,
		XRenderFindStandardFormat(
			fontset->display,
			PictStandardA1
		),
		0, NULL
	);
	if (mask == None)
		goto error;
	XSetForeground(fontset->display, gc, 0);
	XFillRectangle(
		fontset->display,
		pix,
		gc,
		0, 0,
		rect.width,
		rect.height
	);
	XSetForeground(fontset->display, gc, 1);
	if (fontset->xlfd_font != NULL)
		retval = drawxstring(fontset, pix, gc, rect, text, nbytes);
	else if (fontset->xlfd_fontset != NULL)
		retval = drawxmbstring(fontset, pix, gc, rect, text, nbytes);
	else
		goto error;
	XRenderComposite(
		fontset->display,
		PictOpOver,
		src,
		mask,
		picture,
		0, 0,
		0, 0,
		rect.x, rect.y,
		rect.width, rect.height
	);
	XFreeGC(fontset->display, gc);
	XFreePixmap(fontset->display, pix);
	XRenderFreePicture(fontset->display, mask);
	return retval;
error:
	if (gc != NULL)
		XFreeGC(fontset->display, gc);
	if (pix != None)
		XFreePixmap(fontset->display, pix);
	if (mask != None)
		XRenderFreePicture(fontset->display, mask);
	return -1;
}

static int
widthxftstring(CtrlFontSet *fontset, const char *text, int nbytes)
{
	FT_UInt glyphs[MAXGLYPHS];
	XftFont *font;
	XGlyphInfo extents;
	const char *end = text;
	size_t nglyphs = 0;
	size_t nwritten = 0;
	size_t n = 0;
	int width = 0;

	if (nbytes == 0)
		return 0;
	while (end < text + nbytes)
		glyphs[nglyphs++] = getnextutf8char(end, &end);
	while (nwritten < nglyphs) {
		font = getfontforglyph(fontset, glyphs[nwritten]);
		n = 1 + getfontcoverage(
			fontset,
			font,
			glyphs + nwritten + 1,
			nglyphs - nwritten - 1
		);
		XftTextExtents32(
			fontset->display,
			font,
			glyphs + nwritten,
			n,
			&extents
		);
		nwritten += n;
		width += extents.xOff;
	}
	return width;
}

static int
widthxmbstring(CtrlFontSet *fontset, const char *text, int nbytes)
{
	XRectangle box, dummy;

	XmbTextExtents(fontset->xlfd_fontset, text, nbytes, &dummy, &box);
	return box.width;
}

static int
widthxstring(CtrlFontSet *fontset, const char *text, int nbytes)
{
	XChar2b glyphs[MAXGLYPHS];
	int nglyphs;

	nglyphs = utf8toxchar2b(glyphs, MAXGLYPHS, text, nbytes);
	return XTextWidth16(fontset->xlfd_font, glyphs, nglyphs);
}

CtrlFontSet *
ctrlfnt_open(Display *display, int screen, Visual *visual, Colormap
             colormap, const char *fontspec, double fontsize)
{
	CtrlFontSet *fontset = NULL;
	const char *str = fontspec;
	enum {
		XLFD_FONT,
		XLFD_FONTSET,
		XFT_FONTSET,
		GUESSTYPE,
	} fonttype = GUESSTYPE;
	int hascomma = strchr(fontspec, ',') != NULL;

	if ((fontset = malloc(sizeof(*fontset))) == NULL)
		goto error;
	*fontset = (CtrlFontSet){
		.display = display,
		.screen = screen,
		.visual = visual,
		.colormap = colormap,
		.xft_fontset = NULL,
		.xlfd_fontset = NULL,
		.xlfd_font = NULL,
	};
	if (strncasecmp(fontspec, "x:", 2) == 0) {
		str = fontspec + 2;
		fonttype = hascomma ? XLFD_FONTSET : XLFD_FONT;
	} else if (strncasecmp(fontspec, "x11:", 4) == 0) {
		str = fontspec + 4;
		fonttype = hascomma ? XLFD_FONTSET : XLFD_FONT;
	} else if (strncasecmp(fontspec, "xft:", 4) == 0) {
		str = fontspec + 4;
		fonttype = XFT_FONTSET;
	}
	switch (fonttype) {
	case XLFD_FONT:
		fontset->xlfd_font = openxfont(display, str, 1);
		break;
	case XLFD_FONTSET:
		fontset->xlfd_fontset = openxfontset(display, str, 1);
		break;
	case XFT_FONTSET:
		fontset->xft_fontset = openxftfontset(display, str, fontsize);
		break;
	case GUESSTYPE:
		if (hascomma)
			fontset->xlfd_fontset = openxfontset(display, str, 0);
		else
			fontset->xlfd_font = openxfont(display, str, 0);
		if (fontset->xlfd_font == NULL && fontset->xlfd_fontset == NULL)
			fontset->xft_fontset = openxftfontset(display, str, fontsize);
		break;
	}
	if (fontset->xft_fontset == NULL &&
	    fontset->xlfd_fontset == NULL &&
	    fontset->xlfd_font == NULL)
		goto error;
	return fontset;
error:
	free(fontset);
	return NULL;
}

int
ctrlfnt_draw(CtrlFontSet *fontset, Picture picture, Picture src,
             XRectangle rect, const char *text, int nbytes)
{
	if (fontset == NULL)
		return -1;
	if (fontset->xft_fontset != NULL)
		return drawxftstring(fontset, picture, src, rect, text, nbytes);
	if (fontset->xlfd_fontset != NULL)
		return drawx(fontset, picture, src, rect, text, nbytes);
	if (fontset->xlfd_font != NULL)
		return drawx(fontset, picture, src, rect, text, nbytes);
	return -1;
}

int
ctrlfnt_width(CtrlFontSet *fontset, const char *text, int nbytes)
{
	if (fontset == NULL)
		return 0;
	if (fontset->xft_fontset != NULL)
		return widthxftstring(fontset, text, nbytes);
	if (fontset->xlfd_fontset != NULL)
		return widthxmbstring(fontset, text, nbytes);
	if (fontset->xlfd_font != NULL)
		return widthxstring(fontset, text, nbytes);
	return 0;
}

int
ctrlfnt_height(CtrlFontSet *fontset)
{
	XFontSetExtents *extents;

	if (fontset == NULL)
		return 0;
	if (fontset->xft_fontset != NULL)
		return fontset->xft_fontset->fonts[0]->height;
	if (fontset->xlfd_font != NULL)
		return fontset->xlfd_font->ascent + fontset->xlfd_font->descent;
	if (fontset->xlfd_fontset != NULL) {
		extents = XExtentsOfFontSet(fontset->xlfd_fontset);
		/* return is owned by Xlib, do not free it */
		if (extents != NULL) {
			return extents->max_ink_extent.height;
		}
	}
	return 0;
}

void
ctrlfnt_free(CtrlFontSet *fontset)
{
	size_t i;

	if (fontset == NULL)
		return;
	if (fontset->xft_fontset != NULL) {
		for (i = 0; i < fontset->xft_fontset->nmemb; i++) {
			XftFontClose(
				fontset->display,
				fontset->xft_fontset->fonts[i]
			);
		}
		free(fontset->xft_fontset->fonts);
		free(fontset->xft_fontset);
	}
	if (fontset->xlfd_fontset != NULL) {
		XFreeFontSet(fontset->display, fontset->xlfd_fontset);
	}
	if (fontset->xlfd_font != NULL) {
		XFreeFont(fontset->display, fontset->xlfd_font);
	}
	free(fontset);
}

void
ctrlfnt_init(void)
{
	(void)FcInit();
}

void
ctrlfnt_term(void)
{
	FcFini();
}
