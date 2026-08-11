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

struct widget {
	struct ctrlapp *app;

	Window leader;
	struct menu *menus;
	struct ctrlfnt *fontset;
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
	struct menu *next;
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

struct widget *ctrlmenu_init(struct ctrlapp *ctrlapp, Window leader);
void ctrlmenu_free(struct widget *widget);
void ctrlmenu_popup(
	struct widget *widget, struct entry *entries, size_t nentries,
	int x_root, int y_root, bool windowed, Time time);
void ctrlmenu_handle_event(struct widget *widget, struct menu *menu, XEvent *event);
void ctrlmenu_reload_resources(struct widget *widget, char const *str);
