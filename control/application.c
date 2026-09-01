#include <err.h>
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/extensions/Xrandr.h>

#include <control/application.h>

static char const *saved_name = NULL;
static char const *saved_class = NULL;
static char **saved_argv = NULL;
static int saved_argc = 0;

void
ctrlapp_set(char const *name, char const *class, int argc, char **argv)
{
	saved_name = name;
	saved_class = class;
	saved_argc = argc;
	saved_argv = argv;
}

char const *
ctrlapp_getname(void)
{
	if (saved_name == NULL)
		return getprogname();
	return saved_name;
}

char const *
ctrlapp_getclass(void)
{
	if (saved_class == NULL)
		return getprogname();
	return saved_class;
}

char **
ctrlapp_getargv(void)
{
	return saved_argv;
}

int
ctrlapp_getargc(void)
{
	return saved_argc;
}

Display *
ctrlapp_open(void)
{
	Display *display = NULL;
	char const *display_name = XDisplayName(NULL);

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support for X11");
	if (display_name == NULL || display_name[0] == '\0') {
		warnx("DISPLAY is not set");
		return NULL;
	}
	if ((display = XOpenDisplay(display_name)) == NULL) {
		warnx("%s: cannot connect to X server", display_name);
		return NULL;
	}
	if (fcntl(XConnectionNumber(display), F_SETFD, FD_CLOEXEC) == -1) {
		warn("connection to X server \"%s\"", display_name);
		XCloseDisplay(display);
		return NULL;
	}
	return display;
}

int
ctrlapp_visuals(Display *display, int screen_number, Visual **visual, int *depth, Colormap *colormap)
{
	XVisualInfo vinfo;

	if (XMatchVisualInfo(display, screen_number, 32, TrueColor, &vinfo)) {
		*colormap = XCreateColormap(
			display, RootWindow(display, screen_number),
			vinfo.visual, AllocNone
		);
		*visual = vinfo.visual;
		*depth = vinfo.depth;
		return 1;
	} else {
		*colormap = DefaultColormap(display, screen_number);
		*visual = DefaultVisual(display, screen_number);
		*depth = DefaultDepth(display, screen_number);
		return 0;
	}
}
