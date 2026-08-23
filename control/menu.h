struct ctrlmenu_entry {
	enum {
		CTRLMENU_COMMAND,
		CTRLMENU_SEPARATOR,
		CTRLMENU_CASCADE,
	} type;

	char *label;
	size_t labellen;

	bool disabled;

#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112)
	union {
	struct {
#endif
		void (*callback)(void *ctx, void *self, bool alt);
		void *ctx, *self;
#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112)
	};
	struct {
#endif
		struct ctrlmenu_menu *submenu;
#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112)
	};
	};
#endif
};

enum {
	CANVAS_NORMAL,
	CANVAS_SELECT,
	CANVAS_FINAL,
	CANVAS_LAST
};

struct ctrlmenu_menu {
	Window window;
	struct ctrlmenu_entry *entries;
	size_t nentries;

	struct ctrlmenu_menu *next;
	ssize_t selected;
	char const *title;
	XRectangle window_geometry;
	int canvas_width, canvas_height;
	int total_height;
	struct {
		size_t first_entry;
		int offset;
	} curr_screenful, last_screenful;
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

void ctrlmenu_popup(
	Display *display,
	Drawable reference,
	XrmDatabase xdb,
	struct ctrlmenu_menu *menus,
	char const *title,
	Window leader,
	int x_root,
	int y_root,
	Time time
);
