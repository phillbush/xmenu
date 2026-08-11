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

struct ctrlapp *ctrlapp = NULL;

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

static size_t
fill_entries(struct entry *entries, size_t menu_indentlvl, char *data, size_t datasize, size_t *from)
{
	size_t nentries;
	struct entry *next_entry = entries;
	struct entry *prev_entry = NULL;

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
		struct entry *entry = &entries[nentries];
		size_t entry_indentlvl;
		char *line = data + *from;
		size_t linelen = strcspn(line, "\n");

		entry_indentlvl = strspn(line, "\t");
		if (entry_indentlvl < menu_indentlvl) {
			break;
		} else if (entry_indentlvl > menu_indentlvl) {
			assert(prev_entry != NULL);
			prev_entry->output = NULL;
			prev_entry->children = next_entry;
			prev_entry->nchildren = fill_entries(
				prev_entry->children,
				entry_indentlvl,
				data, datasize, from
			);
			next_entry += prev_entry->nchildren;
		} else {
			line[linelen] = '\0';
			*from += linelen + 1;
			entry->label = line + entry_indentlvl;
			if (strncmp(entry->label, "IMG:", 4) == 0) {
				size_t pathlen;
				entry->file = entry->label + 4;
				pathlen = strcspn(entry->file, "\t");
				entry->label = entry->file + pathlen;
				entry->label += strspn(entry->label, "\t");
				entry->file[pathlen] = '\0';
			}
			entry->labellen = strcspn(entry->label, "\t");
			entry->output = entry->label + entry->labellen;
			entry->output += strspn(entry->output, "\t");
			entry->label[entry->labellen] = '\0';

			if (*entry->label == '\0')
				entry->label = NULL;
			if (*entry->output == '\0')
				entry->output = entry->label;
			else if (strcmp(entry->output, ":") == 0)
				entry->output = NULL;
			entry->altoutput = entry->output;
			if (prev_entry != NULL && prev_entry->output != NULL &&
			    entry->label != NULL && strcmp(entry->label, "''") == 0) {
				prev_entry->altoutput = entry->output;
				continue;
			}
			prev_entry = entry;
			nentries++;
		}
	}
	return nentries;
}

static size_t
parsestdin(struct entry **entries, char **data)
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

static int
grabpointer(struct widget *widget)
{
	struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000, };

	/* we may have to wait for another process to ungrab */
	for (int i = 0; i < 1000; i++) {
		if (XGrabPointer(
			ctrlapp->display, ctrlapp->root_window, True,
			ButtonPressMask|PointerMotionMask,
			GrabModeAsync, GrabModeAsync,
			None, ctrlapp->cursor, CurrentTime
		) == GrabSuccess)
			return RETURN_SUCCESS;
		nanosleep(&ts, NULL);
	}
	warnx("cannot grab keyboard");
	return RETURN_FAILURE;
}

static int
grabkeyboard(struct widget *widget)
{
	struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000, };

	/* we may have to wait for another process to ungrab */
	for (int i = 0; i < 1000; i++) {
		if (XGrabKeyboard(
			ctrlapp->display, ctrlapp->root_window, True,
			GrabModeAsync, GrabModeAsync,
			CurrentTime
		) == GrabSuccess) return RETURN_SUCCESS;
		nanosleep(&ts, NULL);
	}
	warnx("cannot grab keyboard");
	return RETURN_FAILURE;
}

static void
initimlib(struct widget *widget)
{
	if (widget->initimlib)
		return;
	widget->initimlib = true;
	imlib_set_cache_size(2048 * 1024);
	imlib_context_set_dither(1);
	imlib_context_set_blend(1);
	imlib_context_set_display(ctrlapp->display);
	imlib_context_set_visual(ctrlapp->visual);
	imlib_context_set_colormap(ctrlapp->colormap);
}

static Imlib_Image
loadicon(struct widget *widget, const char *file, int size, int *width_ret, int *height_ret)
{
	Imlib_Image icon = NULL;
	Imlib_Load_Error errcode;
	const char *errstr;
	int width;
	int height;

	if (file == NULL)
		return NULL;
	if (*file == '\0') {
		warnx("could not load icon (file name is blank)");
		return NULL;
	}
	initimlib(widget);
	icon = imlib_load_image_with_error_return(file, &errcode);
	if (icon == NULL) {
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
		warnx("could not load icon (%s): %s", errstr, file);
		return NULL;
	}

	imlib_context_set_image(icon);

	width = imlib_image_get_width();
	height = imlib_image_get_height();

	if (width > height) {
		*width_ret = size;
		*height_ret = (height * size) / width;
	} else {
		*width_ret = (width * size) / height;
		*height_ret = size;
	}

	icon = imlib_create_cropped_scaled_image(
		0, 0,
		width, height,
		*width_ret, *height_ret
	);
	return icon;
}

static void
waitrootclick(unsigned int modifier, unsigned int button)
{
	XEvent xev;
	static unsigned int MODS = ShiftMask|ControlMask|Mod1Mask|Mod4Mask;

	for (;;) {
		(void)XNextEvent(ctrlapp->display, &xev);
		if (xev.type != ButtonPress)
			continue;
		if (xev.xbutton.button == button && (
			(modifier != 0 && modifier == (xev.xbutton.state & MODS)) ||
			xev.xbutton.subwindow == None
		)) {
			XAllowEvents(
				ctrlapp->display,
				AsyncPointer,
				xev.xbutton.time
			);
			return;
		}
		XAllowEvents(
			ctrlapp->display,
			ReplayPointer,
			xev.xbutton.time
		);
	}
}

static struct menu *
get_menu(struct widget *widget, Window window)
{
	for (struct menu *menu = widget->menus; menu != NULL; menu = menu->next)
		if (menu->window == window)
			return menu;
	return NULL;
}

int
main(int argc, char *argv[])
{
	struct widget *widget = NULL;
	int x_root, y_root;
	int geometry_flags = 0;
	char *name = "xmenu";
	char *title = "XMenu";
	char *read_data = NULL;
	XrmDatabase xrm = NULL;
	Window leader = None;
	bool rootmode = false;
	struct entry *entries = NULL;
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

	if ((ctrlapp = ctrlapp_open(xrm, title, name, "XMenu", argc, argv)) == NULL)
		errx(EXIT_FAILURE, "cannot open application");
	if ((widget = ctrlmenu_init(ctrlapp, leader)) == NULL)
		errx(EXIT_FAILURE, "cannot create widget");

	if (rootmode) {
		XGrabButton(
			ctrlapp->display,
			button,
			AnyModifier,
			ctrlapp->root_window,
			False,
			ButtonPressMask,
			GrabModeSync,
			GrabModeSync,
			None,
			None
		);
	}

	if (FLAG(geometry_flags, XValue|YValue)) {
		int nmons = 0;
		int x = 0;
		int y = 0;
		int width = DisplayWidth(ctrlapp->display, ctrlapp->screen_number);
		int height = DisplayHeight(ctrlapp->display, ctrlapp->screen_number);
		XRRMonitorInfo *infos = XRRGetMonitors(
			ctrlapp->display, ctrlapp->root_window, True, &nmons
		);
		
		if (monitor_name != NULL) {
			Atom name = None;
			long num = -1;
			int x = -1, y = -1;

			if (strcmp(monitor_name, "cursor") == 0) {
				XQueryPointer(
					ctrlapp->display, ctrlapp->root_window,
					&(Window){}, &(Window){},
					&x, &y, &(int){}, &(int){}, &(unsigned){}
				);
			} else if (isdigit(*monitor_name)) {
				char *endp;
				num = strtoul(monitor_name, &endp, 10);
				if (*endp != '\0' || num < 0 || num > INT_MAX)
					num = -1;
			} else {
				name = XInternAtom(ctrlapp->display, monitor_name, true);
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
		if (rootmode) {
			waitrootclick(modifier, button);
		//} else if (grabpointer(widget) == RETURN_FAILURE ||
		//           grabkeyboard(widget) == RETURN_FAILURE) {
		//	break;
		}
		if (!FLAG(geometry_flags, XValue|YValue)) {
			XQueryPointer(
				ctrlapp->display, ctrlapp->root_window,
				&(Window){}, &(Window){},
				&x_root, &y_root, &(int){}, &(int){}, &(unsigned){}
			);
		}
		ctrlmenu_popup(
			widget, entries, nentries,
			x_root, y_root, windowed,
			CurrentTime
		);
	} while (rootmode);

	while (widget->menus != NULL) {
		XEvent event;
		struct menu *menu;

		(void)XNextEvent(ctrlapp->display, &event);
		menu = get_menu(widget, event.xany.window);
		if (menu != NULL) {
			ctrlmenu_handle_event(widget, menu, &event);
		} else if (event.type == PropertyNotify &&
		           event.xproperty.state == PropertyNewValue &&
		           event.xproperty.atom == XA_RESOURCE_MANAGER &&
		           event.xproperty.window == ctrlapp->root_window) {
			char *str = gettextprop(
				ctrlapp->display,
				ctrlapp->root_window,
				XA_RESOURCE_MANAGER
			);
			if (str != NULL)
				ctrlmenu_reload_resources(widget, str);
			free(str);
		} else if (event.type == UnmapNotify && leader != None &&
		           event.xdestroywindow.window == leader) {
			break;
		}
	}

	ctrlmenu_free(widget);
	free(read_data);
	free(entries);
	ctrlapp_close(ctrlapp);
	XrmDestroyDatabase(xrm);
	return EXIT_SUCCESS;
}
