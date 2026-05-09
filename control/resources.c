#include <err.h>
#include <stdlib.h>
#include <string.h>

#include <control/resources.h>

#define MAXDBDEPTH 100
#define BETWEEN(a,b,c) ((a) <= (b) && (b) <= (c))
#define toupper(c) (BETWEEN('a', (c), 'z') ? (c)-('a'-'A') : (c))
#define tolower(c) (BETWEEN('A', (c), 'Z') ? (c)+('a'-'A') : (c))
#define strcasecmp(a,b) my_strcasecmp((a),(b))

#ifndef Bool
#define Bool	int
#define False	0
#define True	1
#endif

static void
init(void)
{
	static Bool done = False;

	if (done)
		return;
	XrmInitialize();
	done = True;
}

static int
my_strcasecmp(char const *s, char const *t)
{
	unsigned char const *a = (unsigned char const *)s;
	unsigned char const *b = (unsigned char const *)t;

	for (; tolower(*a) == tolower(*b); a++, b++)
		if (*a == '\0')
			return 0;
	return *a - *b;
}

Bool
ctrlres_istrue(char const *value)
{
	return value != NULL && (
		strcasecmp(value, "1") == 0 ||
		strcasecmp(value, "enable") == 0 ||
		strcasecmp(value, "enabled") == 0 ||
		strcasecmp(value, "on") == 0 ||
		strcasecmp(value, "true") == 0 ||
		strcasecmp(value, "yes") == 0
	);
}

Bool
CTRLRES_SETBOOL(void *ctx, void *addr, const char *value)
{
	Bool *dst = addr;

	(void)ctx;
	if (addr == NULL)
		return True;
	if (ctrlres_istrue(value))
		*dst = True;
	else
		*dst = False;
	return True;
}

Bool
CTRLRES_SETSTR(void *ctx, void *addr, const char *value)
{
	char **dst = addr;

	(void)ctx;
	if (addr == NULL)
		return True;
	*dst = (char *)value;
	return True;
}

char const *
ctrlres_lookup(XrmDatabase *dbs, size_t ndbs,
		char *name_path[], char *class_path[], size_t nnodes)
{
	XrmValue val;
	XrmClass class[MAXDBDEPTH+1];
	XrmName name[MAXDBDEPTH+1];
	size_t node;
	char buf[64];

	init();
	if (dbs == NULL || nnodes < 1)
		return NULL;
	for (node = 0; node < nnodes && node < MAXDBDEPTH; node++) {
		name[node] = XrmStringToName(name_path[node]);
		if (class_path[node] != NULL) {
			class[node] = XrmStringToClass(class_path[node]);
		} else {

			strlcpy(buf, name_path[node], sizeof(buf));
			buf[0] = toupper(buf[0]);
			class[node] = XrmStringToClass(buf);
		}
	}
	name[node] = NULLQUARK;
	class[node] = NULLQUARK;
	for (size_t i = 0; i < ndbs; i++)
		if (dbs[i] != NULL && XrmQGetResource(dbs[i], name, class, &(XrmRepresentation){}, &val))
			return val.addr;
	return NULL;
}

void
ctrlres_parsexrdb(void *ctx, XrmDatabase *dbs, size_t ndbs,
		struct ctrlres resources[], size_t nresources)
{
	init();
	for (size_t res = 0; res < nresources; res++) {
		char const *value = ctrlres_lookup(
			dbs, ndbs,
			resources[res].name_path,
			resources[res].class_path,
			resources[res].nnodes
		);

		if (value == NULL)
			continue;
		if (resources[res].setter == NULL)
			continue;
		if (resources[res].setter(ctx, resources[res].addr, value))
			continue;
		warnx(
			"%s: invalid argument for resource %s",
			value,
			resources[res].name_path[resources[res].nnodes-1]
		);
	}
}
