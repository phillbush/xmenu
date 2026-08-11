struct ctrlapp {
	Display *display;
	int screen_number;
	Window root_window;

	Visual *visual;
	Colormap colormap;
	int depth;

	struct _widget {
		struct _widget *prev, *next;
		Window window;
		void *p;
	} *widgets;

	XrmDatabase resourcesdb;

	char *name;
	char *class;
	char *title;
	int argc;
	char **argv;
};

struct ctrlapp *ctrlapp_open(
	XrmDatabase resourcesdb,
	char *title,
	char *name,
	char *class,
	int argc,
	char *argv[]
);

void ctrlapp_close(
	struct ctrlapp *app
);
