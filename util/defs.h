#define LEN(a)                  (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b)               ((a)>(b)?(a):(b))
#define MIN(a, b)               ((a)<(b)?(a):(b))
#define FLAG(f, b)              (((f) & (b)) == (b))
#define RETURN_FAILURE          (-1)
#define RETURN_SUCCESS          0

enum events {
	/*
	 * The .type field in XEvent(3) structures start from 2 because
	 * 0 and 1 are reserved by the X11 protocol for errors and
	 * replies.  XNextEvent(3) and related functions never return
	 * an error or reply (or so it is expected).  So we use those
	 * two values for our own purposes.
	 */
	CloseNotify     = 0,
	TimeoutNotify   = 1,
};
