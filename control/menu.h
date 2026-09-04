#include <X11/Xresource.h>

struct ctrlmenu_entry {
	enum {
		CTRLMENU_COMMAND,
		CTRLMENU_SEPARATOR,
		CTRLMENU_CASCADE,
	} type;

	char *label;

#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112)
	union {
	struct {
#endif
		void (*callback)(void *ctx, void *self, bool alt);
		void *self;
#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112)
	};
	struct {
#endif
		struct ctrlmenu *submenu;
#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112)
	};
	};
#endif

	XImage *icons;
	size_t nicons;

	bool disabled;
};

enum {
	CANVAS_NORMAL,
	CANVAS_SELECT,
	CANVAS_FINAL,
	CANVAS_LAST
};

struct ctrlmenu {
	/* caller program must set these: */
	char const *title;
	struct ctrlmenu_entry *entries;
	size_t nentries;

	/* caller program can get these: */
	Window window;

	/* caller program shall ignore these: */
	struct ctrlmenu *next;
	struct ctrlmenu_entry *selected;
	XRectangle window_geometry;
	int canvas_width, canvas_height;
	int total_height;
	struct {
		size_t first_entry;
		int offset;
	} curr_screenful, last_screenful;
	struct Canvas {
		XID pixmap;
		XID picture;
	} canvas_orig[CANVAS_LAST], *canvas;
	bool tearable;
	bool overflow;
	bool hasicon;
	bool hassubmenu;
	bool tornoff;
};

void ctrlmenu_popup(
	void *context,
	Display *display,
	Window reference,
	XrmDatabase resources,
	Window leader,
	int x_root,
	int y_root,
	Time timestamp,
	struct ctrlmenu *menu
);

void ctrlmenu_tearoff(
	Display *display,
	Window reference,
	XrmDatabase resources,
	Window leader,
	int x_root,
	int y_root,
	struct ctrlmenu *menu
);

void ctrlmenu_handle_event(
	void *context,
	Display *display,
	XrmDatabase resources,
	Window leader,
	XEvent *event,
	struct ctrlmenu *menu
);

void ctrlmenu_close(Display *display, struct ctrlmenu *menu);
