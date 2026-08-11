/* color.c: RGB to/from YUV color models conversion routines
 *
 * Colors described in RGB (encoded in a `XColor` structure) are
 * converted into YUV to change its luminance and create lighter
 * and darker shades.
 *
 * RGB colors are converted to HSY (Hue-Saturation/Chroma-Luma), rather
 * than to HSV or HSL models. Luma is the weighted sum of all three
 * color channels, with coefficients based on the percepted brightness
 * of each primary color.
 *
 * Actually, the cylindrical deformation of any linear transformation of
 * the RGB color model or a line could work.  Even HSV or HSL, which are
 * cylindrical deformations of the untransformed RGB cube itself.  But
 * HSY has the advantage of any coordinate looking as bright as any
 * other with the same luma value.
 *
 *                             * * *
 *
 * (This file, exceptionally, is in public domain.)
 *
 * © 2024 Lucas de Sena <lucas AT seninha DOT org>
 *
 * Permission is hereby granted to anyone to use and redistribute
 * this software for any purpose in source and binary forms, with
 * or without modification.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY.
 */

#include <stdlib.h>
#include "color.h"

static const double RED_COEFF       = 0.2126;
static const double GREEN_COEFF     = 0.7152;
static const double BLUE_COEFF      = 0.0722;

enum { MIN, MID, MAX };

struct pair {
	double component;
	double weight;
};

static int
cmp(const void *a, const void *b)
{
	struct pair const *x = a;
	struct pair const *y = b;

	if (x->component < y->component)
		return -1;
	else
		return x->component > y->component;
}

static double
clip(double x)
{
	if (x <= 0)
		return 0;
	else if (x >= 1.0)
		return 1.0;
	return x;
}

struct hsy
to_hsy(struct rgb rgb)
{
	struct pair sorted[3] = {
		{.component = rgb.red,		.weight = RED_COEFF},
		{.component = rgb.green,	.weight = GREEN_COEFF},
		{.component = rgb.blue,		.weight = BLUE_COEFF},
	};
	double critical_luma;
	struct hsy hsy;
	int sector;

	qsort(sorted, 3, sizeof(*sorted), cmp);
	if (sorted[MAX].component == sorted[MIN].component) {
		return (struct hsy){
			.hue = 0,
			.saturation = 0,
			.luma = sorted[MAX].component,
		};
	}

	if (sorted[MAX].component == rgb.red && sorted[MIN].component == rgb.blue)
		sector = 0;
	else if (sorted[MAX].component == rgb.green && sorted[MIN].component == rgb.blue)
		sector = 1;
	else if (sorted[MAX].component == rgb.green && sorted[MIN].component == rgb.red)
		sector = 2;
	else if (sorted[MAX].component == rgb.blue && sorted[MIN].component == rgb.red)
		sector = 3;
	else if (sorted[MAX].component == rgb.blue && sorted[MIN].component == rgb.green)
		sector = 4;
	else /* (sorted[MAX].component == rgb.red && sorted[MIN].component == rgb.green) */
		sector = 5;
	if (sector % 2 == 0)
		hsy.hue = sorted[MID].component - sorted[MIN].component;
	else
		hsy.hue = sorted[MAX].component - sorted[MID].component;
	hsy.hue /= sorted[MAX].component - sorted[MIN].component;
	hsy.hue += sector;
	hsy.hue /= 6.0;

	hsy.luma = RED_COEFF*rgb.red
	         + GREEN_COEFF*rgb.green
	         + BLUE_COEFF*rgb.blue;

	critical_luma = sorted[MID].weight
	              * (sorted[MID].component - sorted[MIN].component)
	              / (sorted[MAX].component - sorted[MIN].component)
	              + sorted[MAX].weight;
	if (hsy.luma > critical_luma)
		hsy.saturation = (sorted[MAX].component - hsy.luma) / (1.0 - hsy.luma);
	else
		hsy.saturation = (hsy.luma - sorted[MIN].component) / hsy.luma;

	return hsy;
}

struct rgb
to_rgb(struct hsy hsy)
{
#define NSECTORS 6
	int sector;
	double end, hue, critical_luma;
	struct pair sorted[3];
	static double weights[NSECTORS][3] = {
		[0] = {BLUE_COEFF, GREEN_COEFF, RED_COEFF},
		[1] = {BLUE_COEFF, RED_COEFF, GREEN_COEFF},
		[2] = {RED_COEFF, BLUE_COEFF, GREEN_COEFF},
		[3] = {RED_COEFF, GREEN_COEFF, BLUE_COEFF},
		[4] = {GREEN_COEFF, RED_COEFF, BLUE_COEFF},
		[5] = {GREEN_COEFF, BLUE_COEFF, RED_COEFF},
	};
	static int component_order[NSECTORS][3] = {
		[0] = {MAX, MID, MIN},
		[1] = {MID, MAX, MIN},
		[2] = {MIN, MAX, MID},
		[3] = {MIN, MID, MAX},
		[4] = {MID, MIN, MAX},
		[5] = {MAX, MIN, MID},
	};

	if (hsy.saturation == 0) {
		/* achromatic case */
		return (struct rgb){
			.red	= hsy.luma,
			.green	= hsy.luma,
			.blue	= hsy.luma,
		};
	}

	sector = hsy.hue * 6.0;
	hue = hsy.hue * 6.0 - (int)sector;
	if (sector % 2 != 0)
		hue = 1.0 - hue;

	critical_luma = (weights[sector][MID] * hue) + weights[sector][MAX];

	end = (hsy.luma > critical_luma) ? 1.0 : 0.0;
	sorted[0].component = hsy.luma + (end-hsy.luma) * hsy.saturation;
	sorted[1].component = hsy.luma + (end-hsy.luma) * hsy.saturation
                            * (hue - critical_luma)
                            / (end - critical_luma);
	sorted[2].component = hsy.luma + (end-hsy.luma) * hsy.saturation
                            * (1.0 - end - critical_luma)
                            / (end - critical_luma);

	qsort(sorted, 3, sizeof(*sorted), cmp);

	return (struct rgb){
		.red   = clip(sorted[component_order[sector][0]].component),
		.green = clip(sorted[component_order[sector][1]].component),
		.blue  = clip(sorted[component_order[sector][2]].component),
	};
}
