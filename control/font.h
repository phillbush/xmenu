typedef struct ctrlfnt ctrlfnt;

ctrlfnt *
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
	ctrlfnt    *fontset,
	Picture         picture,
	Picture         src,
	XRectangle      rect,
	const char     *text,
	int             nbytes
);

int ctrlfnt_width(ctrlfnt *fontset, const char *text, int nbytes);
int ctrlfnt_height(ctrlfnt *fontset);
int ctrlfnt_ascent(ctrlfnt *fontset);
int ctrlfnt_descent(ctrlfnt *fontset);
void ctrlfnt_free(ctrlfnt *fontset);
