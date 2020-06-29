static struct Config config = {
	/* font */
	.font = "monospace:size=9", /* for regular items */

	/* colors */
	.background_color = "#FFFFFF",
	.foreground_color = "#2E3436",
	.selbackground_color = "#3584E4",
	.selforeground_color = "#FFFFFF",
	.separator_color = "#CDC7C2",
	.border_color = "#E6E6E6",

	/* sizes in pixels */
	.width_pixels = 130,        /* minimum width of a menu */
	.height_pixels = 25,        /* height of a single menu item */
	.border_pixels = 1,         /* menu border */
	.separator_pixels = 3,      /* space around separator */
	.gap_pixels = 0,            /* gap between menus */

	/* the variables below cannot be set by X resources */

	/* geometry of the right-pointing isoceles triangle for submenus */
	.triangle_width = 3,
	.triangle_height = 7,

	/* padding of the area around the icon */
	.iconpadding = 4,
};
