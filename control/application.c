#include <err.h>
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>

#include <control/application.h>

static struct ctrlapp *ctrlapp = NULL;

struct ctrlapp *
ctrlapp_open(XrmDatabase resourcesdb,
		char *title, char *name, char *class, int argc, char *argv[])
{
	static struct ctrlapp app;
	XVisualInfo vinfo;
	char const *dpyname;

	if (ctrlapp != NULL) {
		warnx("ctrlapp: application already open");
		return NULL;
	}
	app = (struct ctrlapp){
		.widgets        = NULL,
		.resourcesdb    = resourcesdb,
		.name           = name,
		.class          = class,
		.title          = title,
		.argc           = argc,
		.argv           = argv,
	};
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support for X11");
	if ((dpyname = XDisplayName(NULL)) == NULL || dpyname[0] == '\0')
		errx(EXIT_FAILURE, "DISPLAY is not set");
	if ((app.display = XOpenDisplay(dpyname)) == NULL)
		errx(EXIT_FAILURE, "%s: cannot connect to X server", dpyname);
	if (fcntl(XConnectionNumber(app.display), F_SETFD, FD_CLOEXEC) == -1)
		err(EXIT_FAILURE, "connection to X server \"%s\"", dpyname);
	app.screen_number = DefaultScreen(app.display);
	app.root_window = RootWindow(app.display, app.screen_number);
	app.cursor = XCreateFontCursor(app.display, XC_left_ptr);
	if (XMatchVisualInfo(app.display, app.screen_number, 32, TrueColor, &vinfo)) {
		app.colormap = XCreateColormap(
			app.display,
			DefaultRootWindow(app.display),
			vinfo.visual,
			AllocNone
		);
		app.visual = vinfo.visual;
		app.depth = vinfo.depth;
	} else {
		app.colormap = DefaultColormap(app.display, DefaultScreen(app.display));
		app.visual = DefaultVisual(app.display, DefaultScreen(app.display));
		app.depth = DefaultDepth(app.display, DefaultScreen(app.display));
	}
	ctrlapp = &app;
	return ctrlapp;
}

void
ctrlapp_close(struct ctrlapp *ctrlapp)
{
	if (ctrlapp == NULL)
		return;
	if (ctrlapp->colormap != None)
		XFreeColormap(ctrlapp->display, ctrlapp->colormap);
	if (ctrlapp->cursor != None)
		XFreeCursor(ctrlapp->display, ctrlapp->cursor);
	if (ctrlapp->display != NULL)
		XCloseDisplay(ctrlapp->display);
}
