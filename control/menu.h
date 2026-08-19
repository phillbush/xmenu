struct ctrlmenu_entry {
	enum {
		CTRLMENU_INACTIVE,
		CTRLMENU_COMMAND,
		CTRLMENU_SEPARATOR,
		CTRLMENU_SUBMENU,
	} type;

	char *label;
	size_t labellen;

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
		struct ctrlmenu_entry *children;
		size_t nchildren;
#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112)
	};
	};
#endif
};

void ctrlmenu_popup(
	Display *display,
	int screen_number,
	XrmDatabase xdb,
	Visual *visual,
	int depth,
	Colormap colormap,
	struct ctrlmenu_entry *entries,
	size_t nentries,
	char const *title,
	Window leader,
	int x_root,
	int y_root,
	Time time
);
