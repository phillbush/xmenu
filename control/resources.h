#include <X11/Xresource.h>

struct ctrlres {
	char **name_path, **class_path;
	size_t nnodes;

	void *addr;
	int (*setter)(void *ctx, void *addr, const char *value);
};

void ctrlres_parsexrdb(
	void *ctx,
	XrmDatabase *xrdbs,
	size_t nxrdbs,
	struct ctrlres resources[],
	size_t nresources
);

char const *ctrlres_lookup(
	XrmDatabase *xrdbs,
	size_t nxrdbs,
	char *name_path[],
	char *class_path[],
	size_t nnodes
);

int ctrlres_istrue(char const *value);

int CTRLRES_SETBOOL(void *ctx, void *addr, const char *value);
int CTRLRES_SETSTR(void *ctx, void *addr, const char *value);
