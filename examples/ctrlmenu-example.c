/*
 * This program illustrates the creation of a menu hierarchy, popping
 * it up on user event, and dispatching events to torn-off menus.
 *
 * It creates a dummy empty window, which is supposed to be some kind
 * of text input where the user can select a typed in text and open a
 * context menu for that selection.
 *
 * For practical purposes, suppose the selected text is "lorem ipsum".
 *
 * The menu hierarchy is as follows:
 *      ┌────────────┐
 *      │ Copy       │
 *      │ Cut        ├───────────────────┐
 *      │ Search at >│ Internet Archive  │
 *      │────────────│ Wikipedia         │
 *      │ Save       │ Wiktionary        │
 *      └────────────┤ Marginalia Search │
 *                   └───────────────────┘
 *
 * First, we list the entries for both menus in the hierarchy (the root
 * menu and the submenu), and create the menus themselves.  Then, at the
 * main loop, we call ctrlmenu_popup() when a user action triggers the
 * context menu.  This function blocks until the pop-up menu closes or
 * is torn-off (if enabled).
 *
 * If a menu has been torn-off by the user, events for it are dispatched
 * asynchronously with ctrlmenu_handle_event().
 *
 * At the end, torn-off menus (if any) are closed with ctrlmenu_close().
 */

#include <X11/Xlib.h>
#include <control/menu.h>

#include <stdio.h>
#include <X11/keysym.h>

Display *display;

struct ctrlmenu search_menu;

void snarf(void *ctx, void *also_delete, bool);
void search(void *ctx, void *, bool);
void plumb(void *ctx, void *, bool);

#define countof(x) (sizeof(x)/sizeof((x)[0]))

struct ctrlmenu_entry context_menu_entries[] = {
	{CTRLMENU_COMMAND, "Copy",      .callback = snarf,  .self = &(int){0}},
	{CTRLMENU_COMMAND, "Cut",       .callback = snarf,  .self = &(int){1}},
	{CTRLMENU_CASCADE, "Search at", .submenu = &search_menu},
	{CTRLMENU_SEPARATOR},
	{CTRLMENU_COMMAND, "Open",      .callback = plumb,  .self = NULL},
};

struct ctrlmenu_entry search_menu_entries[] = {
	{CTRLMENU_COMMAND, "Wikipedia",  .callback = search, .self = "wikipedia"},
	{CTRLMENU_COMMAND, "Wiktionary", .callback = search, .self = "wiktionary"},
	{CTRLMENU_COMMAND, "Marginalia", .callback = search, .self = "marginalia"},
};

struct ctrlmenu context_menu = {
	.title = "Context Menu",
	.entries = context_menu_entries,
	.nentries = countof(context_menu_entries),
};

struct ctrlmenu search_menu = {
	.title = "Search at",
	.entries = search_menu_entries,
	.nentries = countof(search_menu_entries),
};

/* cut or copy selection; depending if *self is non-zero */
void snarf(void *ctx, void *self, bool ignore) {
	int also_delete = *(int *)self;
	char *selection = ctx;

	(void)ignore;
	if (also_delete)
		printf("Cutting selection: \"%s\"\n", selection);
	else
		printf("Copying selection: \"%s\"\n", selection);
	fflush(stdout);
}

/* search selection on given web search engine */
void search(void *ctx, void *self, bool ignore) {
	char *selection = ctx;
	char *engine = self;

	(void)ignore;
	printf("Searching string \"%s\" at %s\n", selection, engine);
	fflush(stdout);
}

/* open selection; on middle-click, open on new tab */
void plumb(void *ctx, void *self, bool new_tab) {
	char *selection = ctx;

	(void)self;
	if (new_tab)
		printf("Opening \"%s\" at new tab\n", selection);
	else
		printf("Opening \"%s\"\n", selection);
	fflush(stdout);
}

int main(void) {
	Window window;
	XEvent event;
	char *selection = "lorem ipsum";

	display = XOpenDisplay(NULL);
	window = XCreateSimpleWindow(display, DefaultRootWindow(display),
	                             0, 0, 100, 100, 0, 0, 0);
	XSelectInput(display, window, ButtonPressMask|KeyPressMask);
	XMapWindow(display, window);
	for (;;) {
		XNextEvent(display, &event);
		if (event.type == ButtonPress &&
		    event.xbutton.button == Button3 && 
		    event.xbutton.window == window) {
			ctrlmenu_popup(
				selection, display, window, NULL, window,
				event.xbutton.x_root, event.xbutton.y_root,
				event.xbutton.time, &context_menu
			);
		} else if (event.xany.window == context_menu.window) {
			ctrlmenu_handle_event(
				selection, display, NULL, window,
				&event, &context_menu
			);
		} else if (event.xany.window == search_menu.window) {
			ctrlmenu_handle_event(
				selection, display, NULL, window,
				&event, &search_menu
			);
		} else if (event.type == KeyPress &&
		           event.xkey.keycode == XKeysymToKeycode(display, XK_q)) {
			break;
		}
	}
	ctrlmenu_close(display, &context_menu);
	ctrlmenu_close(display, &search_menu);
	XDestroyWindow(display, window);
	XCloseDisplay(display);
}
