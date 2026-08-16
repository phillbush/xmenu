void ctrlapp_set(
	char const *name,
	char const *class,
	int argc,
	char **argv
);

char const *ctrlapp_getname(void);
char const *ctrlapp_getclass(void);
char **ctrlapp_getargv();
int ctrlapp_getargc();

Display *ctrlapp_open(void);

int ctrlapp_visuals(
	Display *display,
	int screen_number,
	Visual **visual_ret,
	int *depth_ret,
	Colormap *colormap_ret
);
