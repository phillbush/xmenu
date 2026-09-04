#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xft/Xft.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <Imlib2.h>

#include "control/application.h"
#include "control/menu.h"
#include "control/resources.h"

#include "util/color.h"
#include "util/defs.h"

static Display *display;

static int
parse_geometry(char *geometry_spec, int *x_root, int *y_root, char **monitor_name)
{
	char *str = geometry_spec;
	long n;
	int retval = XValue|YValue;

	if (str == NULL || *str == '\0') goto error;
	if (*str == '=') str++;

	/* size /([0-9]+[Xx][0-9]+)?/, compatibile with XParseGeometry(3); ignored */
	if (isdigit(*str)) {
		n = strtoul(str, &str, 10);
		if (n <= 0 || n > USHRT_MAX) goto error;

		if (*str != 'X' && *str != 'x') goto error;
		str++;

		if (!isdigit(*str)) goto error;
		n = strtoul(str, &str, 10);
		if (n <= 0 || n > USHRT_MAX) goto error;
		retval |= WidthValue|HeightValue;
	}

	/* x placement /[+-][0-9]+/; obligatory */
	if (*str != '+' && *str != '-') goto error;
	if (*(str++) == '-')
		retval |= XNegative;
	if (!isdigit(*str)) goto error;
	n = strtoul(str, &str, 10);
	if (n < 0 || n > SHRT_MAX) goto error;
	*x_root = n;

	/* y placement /[+-][0-9]+/; obligatory */
	if (*str != '+' && *str != '-') goto error;
	if (*(str++) == '-')
		retval |= YNegative;
	if (!isdigit(*str)) goto error;
	n = strtoul(str, &str, 10);
	if (n < 0 || n > SHRT_MAX) goto error;
	*y_root = n;

	/* monitor /([@:].*)?/; optional */
	*monitor_name = NULL;
	if (*str == '@' || *str == ':') {
		str++;
		if (*str == '\0')
			goto error;
		*monitor_name = str;
		return retval;
	}
	if (*str == '\0')
		return retval;
error:
	errx(EXIT_FAILURE, "invalid geometry specification -- %s", geometry_spec);
}

static void
parse_keychord(char *key_spec, unsigned int *modifier, unsigned int *button)
{
	static struct {
		unsigned int mask;
		char *names[2];
	} modifiers[] = {
		{ ShiftMask,    {"shift-", "s-"} },
		{ ControlMask,  {"ctrl-", "c-"} },
		{ Mod1Mask,     {"alt-", "a-"} },
		{ Mod4Mask,     {"super-", "w-"} },
	};
	size_t spn = 0;
	char *str = key_spec;
	long n;

	*modifier = 0;
	*button = 0;
	while (str[spn = strcspn(str, "-")] == '-') {
		for (size_t i = 0; i < LEN(modifiers); i++) {
			for (size_t j = 0; j < 2; j++) {
				size_t len = strlen(modifiers[i].names[j]);
				if (strncasecmp(str, modifiers[i].names[j], len) == 0) {
					*modifier |= modifiers[i].mask;
					goto next;
				}
			}
		}
		goto error;
	next:
		str += spn + 1;
	}
	if (str[0] == '\0')
		goto error;
	n = strtol(str, &str, 10);
	if (str[0] != '\0')
		goto error;
	if (n < 1 || n > 64)
		goto error;
	*button = n;
	return;
error:
	errx(EXIT_FAILURE, "%s: invalid keychord specification", key_spec);
}

static void
callback(void *, void *self, bool)
{
	printf("%s\n", (char *)self);
	fflush(stdout);
}

static XImage *
load_icon(const char *file)
{
	static bool has_init = false;
	static char *iconpath = NULL;
	Imlib_Image imlimg = NULL;
	Imlib_Load_Error errcode;
	const char *errstr;
	int width, height;
	void *data;

	if (file == NULL)
		return NULL;
	if (*file == '\0') {
		warnx("cannot load icon (file name is blank)");
		return NULL;
	}
	if (!has_init) {
		has_init = true;
		imlib_set_cache_size(2048 * 1024);
		imlib_context_set_display(display);
		iconpath = getenv("ICONPATH");
	}
	if (strncmp(file, "/", 1) &&
	    strncmp(file, "./", 1) &&
	    strncmp(file, "../", 1)) {
		char buf[PATH_MAX];

		if (iconpath == NULL) {
			warnx("%s, cannot load icon ($ICONPATH unset)", file);
			return NULL;
		}
		for (char *next, *s = iconpath;
		     (s += strspn(s, ":"))[0] != '\0';
		     s = next) {
			next = s + strcspn(s, ":");
			if (next - s == 0)
				continue;
			snprintf(buf, sizeof(buf), "%.*s/%s", (int)(next-s), s, file);
			imlimg = imlib_load_image_with_error_return(buf, &errcode);
			if (imlimg != NULL)
				break;
			if (errcode != IMLIB_LOAD_ERROR_FILE_DOES_NOT_EXIST)
				break;
		}
	} else {
		imlimg = imlib_load_image_with_error_return(file, &errcode);
	}
	if (imlimg == NULL) {
		switch (errcode) {
		case IMLIB_LOAD_ERROR_FILE_DOES_NOT_EXIST:
			errstr = "file does not exist";
			break;
		case IMLIB_LOAD_ERROR_FILE_IS_DIRECTORY:
			errstr = "file is directory";
			break;
		case IMLIB_LOAD_ERROR_PERMISSION_DENIED_TO_READ:
		case IMLIB_LOAD_ERROR_PERMISSION_DENIED_TO_WRITE:
			errstr = "permission denied";
			break;
		case IMLIB_LOAD_ERROR_NO_LOADER_FOR_FILE_FORMAT:
			errstr = "unknown file format";
			break;
		case IMLIB_LOAD_ERROR_PATH_TOO_LONG:
			errstr = "path too long";
			break;
		case IMLIB_LOAD_ERROR_PATH_COMPONENT_NON_EXISTANT:
		case IMLIB_LOAD_ERROR_PATH_COMPONENT_NOT_DIRECTORY:
		case IMLIB_LOAD_ERROR_PATH_POINTS_OUTSIDE_ADDRESS_SPACE:
			errstr = "improper path";
			break;
		case IMLIB_LOAD_ERROR_TOO_MANY_SYMBOLIC_LINKS:
			errstr = "too many symbolic links";
			break;
		case IMLIB_LOAD_ERROR_OUT_OF_MEMORY:
			errstr = "out of memory";
			break;
		case IMLIB_LOAD_ERROR_OUT_OF_FILE_DESCRIPTORS:
			errstr = "out of file descriptors";
			break;
		default:
			errstr = "unknown error";
			break;
		}
		warnx("cannot load icon (%s): %s", errstr, file);
		return NULL;
	}

	imlib_context_set_image(imlimg);
	width = imlib_image_get_width();
	height = imlib_image_get_height();
	if ((data = malloc(4 * width * height)) == NULL) {
		imlib_free_image();
		return NULL;
	}
	memcpy(data, imlib_image_get_data_for_reading_only(), 4 * width * height);
	imlib_free_image();
	return XCreateImage(
		display, NULL, 32, ZPixmap, 0, data, width, height, 32, 0
	);
}

static size_t
fill_menu(struct ctrlmenu **menus, struct ctrlmenu_entry **next_entry,
	size_t menu_indentlvl, char *data, size_t datasize, size_t *from, char *title)
{
	size_t nentries;
	struct ctrlmenu *menu = *menus;
	struct ctrlmenu_entry *prev_entry = NULL;

	*menu = (struct ctrlmenu){
		.entries = *next_entry,
		.title = title
	};
	for (size_t len = *from; len < datasize; len++) {
		size_t entry_indentlvl;

		entry_indentlvl = strspn(data+len, "\t");
		len += strcspn(data+len, "\n");
		if (entry_indentlvl < menu_indentlvl)
			break;
		if (entry_indentlvl == menu_indentlvl)
			(*next_entry)++;
	}
	(*menus)++;

	nentries = 0;
	while (*from < datasize) {
		struct ctrlmenu_entry *entry = &menu->entries[nentries];
		size_t entry_indentlvl;
		char *line = data + *from;
		size_t linelen = strcspn(line, "\n");

		entry_indentlvl = strspn(line, "\t");
		if (entry_indentlvl < menu_indentlvl) {
			break;
		} else if (entry_indentlvl > menu_indentlvl) {
			assert(prev_entry != NULL);
			prev_entry->type = CTRLMENU_CASCADE;
			prev_entry->self = NULL;
			prev_entry->submenu = *menus;
			prev_entry->submenu->nentries = fill_menu(
				menus, next_entry, entry_indentlvl,
				data, datasize, from, prev_entry->label
			);
		} else {
			char *output;
			size_t labellen;

			line[linelen] = '\0';
			*from += linelen + 1;
			entry->label = line + entry_indentlvl;
			if (strncmp(entry->label, "IMG:", 4) == 0) {
				size_t pathlen;
				char *path;

				path = entry->label + 4;
				pathlen = strcspn(path, "\t");
				entry->label = path + pathlen;
				entry->label += strspn(entry->label, "\t");
				path[pathlen] = '\0';
				entry->icons = load_icon(path);
				if (entry->icons != NULL)
					entry->nicons = 1;
			}
			labellen = strcspn(entry->label, "\t");
			output = entry->label + labellen;
			output += strspn(output, "\t");
			entry->label[labellen] = '\0';
			entry->callback = callback;

			if (*entry->label == '\0')
				entry->label = NULL;
			if (*output == '\0')
				output = entry->label;
			else if (strcmp(output, ":") == 0)
				output = NULL;
			entry->self = output;
			if (entry->label == NULL)
				entry->type = CTRLMENU_SEPARATOR;
			else if (entry->self == NULL)
				entry->disabled = true;
			else
				entry->type = CTRLMENU_COMMAND;
			prev_entry = entry;
			nentries++;
		}
	}
	return nentries;
}

static size_t
parse_stdin(char **data, struct ctrlmenu **menus_ret, size_t *nlines, char *title)
{
	size_t last_indentlvl;
	size_t nmenus;
	struct ctrlmenu *menus;
	struct ctrlmenu_entry *entries;
	FILE *stream;
	char buf[BUFSIZ];
	size_t size;
	ssize_t nread;

	if ((stream = open_memstream(data, &size)) == NULL)
		err(EXIT_FAILURE, "open_memstream");
	while ((nread = read(STDIN_FILENO, buf, sizeof(buf))) != 0) {
		if (nread < 0)
			err(EXIT_FAILURE, "read");
		if (fwrite(buf, 1, nread, stream) != (size_t)nread)
			err(EXIT_FAILURE, "fwrite");
	}
	fflush(stream);
	if (size == 0)
		return 0;
	if ((*data)[size-1] != '\n' && (*data)[size-1] != '\0')
		if (fwrite("\0", 1, 1, stream) != 1)
			err(EXIT_FAILURE, "fwrite");
	(*data)[size-1] = '\0';
	if (fclose(stream) == EOF)
			err(EXIT_FAILURE, "fclose");

	*nlines = 0;
	nmenus = 1;
	last_indentlvl = strspn(*data, "\t");
	for (size_t len = 0; len < size; len += strcspn((*data)+len, "\n") + 1) {
		size_t curr_indentlvl = strspn((*data)+len, "\t");
		if (curr_indentlvl > last_indentlvl)
			nmenus++;
		last_indentlvl = curr_indentlvl;
		(*nlines)++;
	}
	if (*nlines == 0 || nmenus == 0)
		return 0;
	if ((menus = calloc(nmenus, sizeof(*menus))) == NULL)
		err(EXIT_FAILURE, "calloc");
	if ((entries = calloc(*nlines, sizeof(*entries))) == NULL)
		err(EXIT_FAILURE, "calloc");
	*menus_ret = &menus[0];
	/*
	 * menus[0].entries is a large array that contains all the
	 * entries for all the other menus, not only for the root
	 * menu (menus[0]).  Therefore, menus[0].nentries (the number
	 * of entries only for the root menu) is lesser than nlines
	 * (count of all entries for all menus).
	 *
	 * We do that to keep only two dynamic memory addresses we
	 * need to free: the array of all entries (menus[0].entries),
	 * and the array of all menus (menus).  Those are freed before
	 * main() returns.
	 */
	(*menus_ret)[0].nentries = fill_menu(
		&menus, &entries,
		strspn(*data, "\t"), *data, size, &(size_t){0}, title
	);
	return nmenus;
}

static struct ctrlmenu *
find_menu(struct ctrlmenu *menus, size_t nmenus, Window window)
{
	for (size_t i = 0; i < nmenus; i++)
		if (window == menus[i].window)
			return &menus[i];
	return NULL;
}

static bool
has_tornoff(struct ctrlmenu *menus, size_t nmenus)
{
	for (size_t i = 0; i < nmenus; i++)
		if (menus[i].tornoff)
			return true;
	return false;
}

int
main(int argc, char *argv[])
{
	int x_root = 0;
	int y_root = 0;
	int geometry_flags = 0;
	char *name = "xmenu";
	char *title = "XMenu";
	char *read_data = NULL;
	XrmDatabase resources = NULL;
	Window reference;
	Window leader = None;
	bool rootmode = false;
	struct ctrlmenu *menus = NULL;
	size_t nmenus = 0;
	size_t nentries = 0;
	unsigned int button = 0;
	unsigned int modifier = 0;
	bool windowed = false;
	char *monitor_name = NULL;

	if (argv[0] != NULL && argv[0][0] != '\0') {
		name = strchr(argv[0], '/');
		if (name != NULL)
			name++;
		else
			name = argv[0];
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-windowed") || !strcmp(argv[i], "-w")) {
			windowed = true;
		} else if (!strcmp(argv[i], "-leader")) {
			if (++i < argc)
				leader = strtoul(argv[i], NULL, 0);
		} else if (!strcmp(argv[i], "-name") || !strcmp(argv[i], "-N")) {
			name = argv[++i];
		} else if (!strcmp(argv[i], "-position") || !strcmp(argv[i], "-p")) {
			if (++i < argc) geometry_flags = parse_geometry(
				argv[i], &x_root, &y_root, &monitor_name
			);
		} else if (!strcmp(argv[i], "-rootgrab") || !strcmp(argv[i], "-x")) {
			if (++i < argc)
				parse_keychord(argv[i], &modifier, &button);
			rootmode = true;
		} else if (!strcmp(argv[i], "-title")) {
			title = argv[++i];
		} else if (!strcmp(argv[i], "-xrm")) {
			XrmDatabase db = NULL;
			if (++i < argc)
				db = XrmGetStringDatabase(argv[i]);
			if (db != NULL)
				XrmCombineDatabase(db, &resources, True);
		} else {
			warnx("unknown option -- %s", argv[i]);
		usage:
			(void)fprintf(
				stderr,
				"usage: xmenu"
				" [-leader WINDOWID]"
				" [-monitor MONITOR]"
				" [-name NAME]"
				" [-position POSITION]"
				" [-rootgrab BUTTON]"
				" [-title TITLE]"
				" [-windowed]"
				" [-xrm RESOURCES]"
				"\n"
			);
			exit(EXIT_FAILURE);
		}
		if (i == argc) {
			warnx("option requires an argument -- %s", argv[i-1]);
			goto usage;
		}
	}
	if (windowed)
		rootmode = false;
	if (rootmode)
		leader = None;

	ctrlapp_set(name, "XMenu", argc, argv);
	if ((display = ctrlapp_open()) == NULL)
		errx(EXIT_FAILURE, "cannot open application");
	if (!XRenderQueryExtension(display, &(int){}, &(int){}))
		errx(EXIT_FAILURE, "X server does not support XRender extension");
	{
		Visual *visual;
		Colormap colormap;
		int depth;
		(void)ctrlapp_visuals(
			display, DefaultScreen(display),
			&visual, &depth, &colormap
		);
		reference = XCreateWindow(
			display, DefaultRootWindow(display),
			0, 0, 1, 1, 1, depth, InputOutput, visual,
			CWBackPixel|CWColormap|CWBorderPixel,
			&(XSetWindowAttributes){.colormap = colormap}
		);
		if (reference == None)
			errx(EXIT_FAILURE, "cannot create reference window");
	}
	nmenus = parse_stdin(&read_data, &menus, &nentries, title);
	if (nmenus == 0)
		errx(EXIT_FAILURE, "no menu generated");

	if (rootmode) {
		XGrabButton(
			display, button, AnyModifier,
			DefaultRootWindow(display), False,
			ButtonPressMask, GrabModeSync, GrabModeSync,
			None, None
		);
	}

	if (FLAG(geometry_flags, XValue|YValue)) {
		int nmons = 0;
		int x = 0;
		int y = 0;
		int width = DisplayWidth(display, DefaultScreen(display));
		int height = DisplayHeight(display, DefaultScreen(display));
		XRRMonitorInfo *infos = XRRGetMonitors(
			display, DefaultRootWindow(display), True, &nmons
		);
		
		if (monitor_name != NULL) {
			Atom name = None;
			long num = -1;
			int x = -1, y = -1;

			if (strcmp(monitor_name, "cursor") == 0) {
				XQueryPointer(
					display, DefaultRootWindow(display),
					&(Window){}, &(Window){},
					&x, &y, &(int){}, &(int){}, &(unsigned){}
				);
			} else if (isdigit(*monitor_name)) {
				char *endp;
				num = strtoul(monitor_name, &endp, 10);
				if (*endp != '\0' || num < 0 || num > INT_MAX)
					num = -1;
			} else {
				name = XInternAtom(display, monitor_name, true);
			}

			for (int i = 0; i < nmons && infos != NULL; i++) {
				if (i == num || infos[i].name == name ||
				    (x >= infos[i].x && x < infos[i].x + infos[i].width) ||
				    (y >= infos[i].y && y < infos[i].y + infos[i].height)) {
					x = infos[i].x;
					y = infos[i].y;
					width = infos[i].width;
					height = infos[i].height;
					goto found;
				}
			}
			errx(EXIT_FAILURE, "cannot find monitor -- %s", monitor_name);
		}
	found:
		if (geometry_flags & XNegative)
			x_root = x + width - x_root;
		else
			x_root += x;
		if (geometry_flags & YNegative)
			y_root = y + height - y_root;
		else
			y_root += y;
		XRRFreeMonitors(infos);
	}


	{
		char *xrm_str = XResourceManagerString(display);
		XrmDatabase db = NULL;

		if (xrm_str != NULL)
			db = XrmGetStringDatabase(xrm_str);
		if (db != NULL)
			XrmCombineDatabase(db, &resources, False);
	}
	if (windowed) {
		ctrlmenu_tearoff(
			display, reference, resources,
			leader, x_root, y_root, menus
		);
	} else if (!rootmode) {
		goto popup;
	}
	for (;;) {
		XEvent event;
		struct ctrlmenu *menu;
		static unsigned int MODS = ShiftMask|ControlMask|Mod1Mask|Mod4Mask;

		(void)XNextEvent(display, &event);
		XAllowEvents(display, ReplayPointer, event.xbutton.time);
		if (rootmode && event.xbutton.button == button && (
			(modifier != 0 && modifier == (event.xbutton.state & MODS)) ||
			event.xbutton.subwindow == None
		)) {
popup:
			if (!FLAG(geometry_flags, XValue|YValue)) {
				XQueryPointer(
					display, DefaultRootWindow(display),
					&(Window){}, &(Window){},
					&x_root, &y_root, &(int){}, &(int){}, &(unsigned){}
				);
			}
			ctrlmenu_popup(
				NULL, display, reference, resources,
				leader, x_root, y_root, CurrentTime, menus
			);
		}
		menu = find_menu(menus, nmenus, event.xany.window);
		if (menu != NULL) {
			ctrlmenu_handle_event(
				NULL, display, resources, leader, &event, menu
			);
		}
		if (!rootmode && !has_tornoff(menus, nmenus))
			break;
	}

	free(read_data);
	for (size_t i = 0; i < nentries; i++)
		if (menus[0].entries[i].icons != NULL)
			XDestroyImage(menus[0].entries[i].icons);
	free(menus[0].entries);
	for (size_t i = 0; i < nmenus; i++)
		ctrlmenu_close(display, &menus[i]);
	free(menus);
	XDestroyWindow(display, reference);
	XrmDestroyDatabase(resources);
	XCloseDisplay(display);
	return EXIT_SUCCESS;
}
