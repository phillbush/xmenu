struct entry {
	char *label;
	size_t labellen;

	char *output;
	char *altoutput;
	char *file;

	struct entry *children;
	size_t nchildren;
};

void ctrlmenu_popup(
	Display *display,
	int screen_number,
	XrmDatabase xdb,
	Visual *visual,
	int depth,
	Colormap colormap,
	struct entry *entries,
	size_t nentries,
	char const *title,
	Window leader,
	int x_root,
	int y_root,
	bool windowed,
	Time time
);
