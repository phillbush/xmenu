#include <X11/extensions/Xrender.h>

struct ctrlfnt *
ctrlfnt_open(
	Display        *display,
	int             screen,
	Visual         *visual,
	Colormap        colormap,
	const char     *fontset_spec,
	double          fontsize
);

int
ctrlfnt_draw(
	struct ctrlfnt    *fontset,
	Picture         picture,
	Picture         src,
	XRectangle      rect,
	const char     *text,
	int             nbytes
);

int ctrlfnt_width(struct ctrlfnt *fontset, const char *text, int nbytes);
int ctrlfnt_height(struct ctrlfnt *fontset);
int ctrlfnt_ascent(struct ctrlfnt *fontset);
int ctrlfnt_descent(struct ctrlfnt *fontset);
void ctrlfnt_free(struct ctrlfnt *fontset);
