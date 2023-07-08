typedef struct CtrlFontSet CtrlFontSet;

CtrlFontSet *
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
	CtrlFontSet    *fontset,
	Picture         picture,
	Picture         src,
	XRectangle      rect,
	const char     *text,
	int             nbytes
);

int ctrlfnt_width(CtrlFontSet *fontset, const char *text, int nbytes);
int ctrlfnt_height(CtrlFontSet *fontset);
void ctrlfnt_free(CtrlFontSet *fontset);
void ctrlfnt_init(void);
void ctrlfnt_term(void);
