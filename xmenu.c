#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

static char *
gettextprop(Display *dpy, Window win, Atom atom)
{
	XTextProperty tprop = { .value = NULL };
	int count;
	char **list = NULL;
	char *s = NULL;

	if (!XGetTextProperty(dpy, win, &tprop, atom))
		goto error;
	if (tprop.nitems == 0)
		goto error;
	if (XmbTextPropertyToTextList(dpy, &tprop, &list, &count) != Success)
		goto error;
	if (count < 1 || list == NULL || *list == NULL)
		goto error;
	s = strdup(list[0]);
error:
	XFreeStringList(list);
	XFree(tprop.value);
	return s;
}

static void
callback(void *ctx, void *self, bool alt)
{
	if (alt)
		printf("%s", (char *)self);
	else
		printf("%s", (char *)ctx);
}

static size_t
fill_entries(struct ctrlmenu_entry *entries, size_t menu_indentlvl, char *data, size_t datasize, size_t *from)
{
	size_t nentries;
	struct ctrlmenu_entry *next_entry = entries;
	struct ctrlmenu_entry *prev_entry = NULL;

	for (size_t len = *from; len < datasize; len++) {
		size_t entry_indentlvl;

		entry_indentlvl = strspn(data+len, "\t");
		len += strcspn(data+len, "\n");
		if (entry_indentlvl < menu_indentlvl)
			break;
		if (entry_indentlvl == menu_indentlvl)
			next_entry++;
	}

	nentries = 0;
	while (*from < datasize) {
		struct ctrlmenu_entry *entry = &entries[nentries];
		size_t entry_indentlvl;
		char *line = data + *from;
		size_t linelen = strcspn(line, "\n");

		entry_indentlvl = strspn(line, "\t");
		entry->callback = callback;
		if (entry_indentlvl < menu_indentlvl) {
			break;
		} else if (entry_indentlvl > menu_indentlvl) {
			assert(prev_entry != NULL);
			prev_entry->type = CTRLMENU_SUBMENU;
			prev_entry->self = NULL;
			prev_entry->children = next_entry;
			prev_entry->nchildren = fill_entries(
				prev_entry->children,
				entry_indentlvl,
				data, datasize, from
			);
			next_entry += prev_entry->nchildren;
		} else {
			char *output;

			line[linelen] = '\0';
			*from += linelen + 1;
			entry->label = line + entry_indentlvl;
			//if (strncmp(entry->label, "IMG:", 4) == 0) {
			//	size_t pathlen;
			//	entry->file = entry->label + 4;
			//	pathlen = strcspn(entry->file, "\t");
			//	entry->label = entry->file + pathlen;
			//	entry->label += strspn(entry->label, "\t");
			//	entry->file[pathlen] = '\0';
			//}
			entry->labellen = strcspn(entry->label, "\t");
			output = entry->label + entry->labellen;
			output += strspn(output, "\t");
			entry->label[entry->labellen] = '\0';

			if (*entry->label == '\0')
				entry->label = NULL;
			if (*output == '\0')
				output = entry->label;
			else if (strcmp(output, ":") == 0)
				output = NULL;
			entry->self = output;
			entry->ctx = entry->self;
			if (entry->label == NULL)
				entry->type = CTRLMENU_SEPARATOR;
			else if (entry->self == NULL)
				entry->type = CTRLMENU_INACTIVE;
			else
				entry->type = CTRLMENU_COMMAND;
			if (prev_entry != NULL && prev_entry->self != NULL &&
			    entry->label != NULL && strcmp(entry->label, "''") == 0) {
				prev_entry->ctx = entry->self;
				continue;
			}
			prev_entry = entry;
			nentries++;
		}
	}
	return nentries;
}

static size_t
parsestdin(struct ctrlmenu_entry **entries, char **data)
{
	size_t nlines;
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

	nlines = 0;
	for (size_t len = 0; len < size; len += strcspn((*data)+len, "\n") + 1)
		nlines++;
	if ((*entries = calloc(nlines, sizeof(**entries))) == NULL)
		err(EXIT_FAILURE, "calloc");

	return fill_entries(*entries, strspn(*data, "\t"), *data, size, &(size_t){0});
}

static void
waitrootclick(Display *display, unsigned int modifier, unsigned int button)
{
	XEvent xev;
	static unsigned int MODS = ShiftMask|ControlMask|Mod1Mask|Mod4Mask;

	for (;;) {
		(void)XNextEvent(display, &xev);
		if (xev.type != ButtonPress)
			continue;
		if (xev.xbutton.button == button && (
			(modifier != 0 && modifier == (xev.xbutton.state & MODS)) ||
			xev.xbutton.subwindow == None
		)) {
			XAllowEvents(display, AsyncPointer, xev.xbutton.time);
			return;
		}
		XAllowEvents(display, ReplayPointer, xev.xbutton.time);
	}
}

int
main(int argc, char *argv[])
{
	Display *display;
	Visual *visual;
	Colormap colormap;
	int depth;
	int x_root, y_root;
	int geometry_flags = 0;
	char *name = "xmenu";
	char *title = "XMenu";
	char *read_data = NULL;
	XrmDatabase xrm = NULL;
	Window leader = None;
	bool rootmode = false;
	struct ctrlmenu_entry *entries = NULL;
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
			leader = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-name") || !strcmp(argv[i], "-N")) {
			name = argv[++i];
		} else if (!strcmp(argv[i], "-position") || !strcmp(argv[i], "-p")) {
			geometry_flags = parse_geometry(
				argv[++i], &x_root, &y_root, &monitor_name
			);
		} else if (!strcmp(argv[i], "-rootgrab") || !strcmp(argv[i], "-x")) {
			parse_keychord(argv[++i], &modifier, &button);
			rootmode = true;
		} else if (!strcmp(argv[i], "-title")) {
			title = argv[++i];
		} else if (!strcmp(argv[i], "-xrm")) {
			XrmDatabase db = XrmGetStringDatabase(argv[++i]);
			if (db != NULL)
				XrmCombineDatabase(db, &xrm, True);
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
	if (rootmode)
		leader = None;
	else if (windowed)
		rootmode = false;
	nentries = parsestdin(&entries, &read_data);
	if (nentries == 0)
		errx(EXIT_FAILURE, "no menu generated");

	ctrlapp_set(name, "XMenu", argc, argv);
	if ((display = ctrlapp_open()) == NULL)
		errx(EXIT_FAILURE, "cannot open application");
	(void)ctrlapp_visuals(
		display, DefaultScreen(display),
		&visual, &depth, &colormap
	);

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

	do {
		if (rootmode)
			waitrootclick(display, modifier, button);
		if (!FLAG(geometry_flags, XValue|YValue)) {
			XQueryPointer(
				display, DefaultRootWindow(display),
				&(Window){}, &(Window){},
				&x_root, &y_root, &(int){}, &(int){}, &(unsigned){}
			);
		}
		ctrlmenu_popup(
			display, DefaultScreen(display), xrm,
			visual, depth, colormap,
			entries, nentries, title, leader,
			x_root, y_root, CurrentTime
		);
	} while (rootmode);

	//while (widget->menus != NULL) {
	//	XEvent event;
	//	struct menu *menu;

	//	(void)XNextEvent(ctrlapp->display, &event);
	//	menu = get_menu(widget, event.xany.window);
	//	if (menu != NULL) {
	//		ctrlmenu_handle_event(widget, menu, &event);
	//	} else if (event.type == PropertyNotify &&
	//	           event.xproperty.state == PropertyNewValue &&
	//	           event.xproperty.atom == XA_RESOURCE_MANAGER &&
	//	           event.xproperty.window == ctrlapp->root_window) {
	//		char *str = gettextprop(
	//			ctrlapp->display,
	//			ctrlapp->root_window,
	//			XA_RESOURCE_MANAGER
	//		);
	//		if (str != NULL)
	//			ctrlmenu_reload_resources(widget, str);
	//		free(str);
	//	} else if (event.type == UnmapNotify && leader != None &&
	//	           event.xdestroywindow.window == leader) {
	//		break;
	//	}
	//}

	free(read_data);
	free(entries);
	XrmDestroyDatabase(xrm);
	return EXIT_SUCCESS;
}
