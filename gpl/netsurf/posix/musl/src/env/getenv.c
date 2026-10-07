/* getenv -- ROSGD overlay: the task's environment, then RISC OS's system
 * variables (design 22, U-c).
 *
 * The environment is the task's own, seeded at start (abi/x32/crt/crt1.c).
 * A name it lacks is read from the system variable of that name, expanded
 * as OS_ReadVarVal expands it, and kept in the environment -- so the
 * pointer returned stays valid, as getenv's must, and a later getenv of the
 * same name finds it there. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

hidden unsigned long long __rosgd_swi(unsigned swi, const unsigned in[10], unsigned out[10]);

#define XOS_ReadVarVal 0x20023u

static char *from_environ(const char *name, size_t l)
{
	if (__environ)
		for (char **e = __environ; *e; e++)
			if (!strncmp(name, *e, l) && l[*e] == '=')
				return *e + l+1;
	return 0;
}

char *getenv(const char *name)
{
	size_t l = __strchrnul(name, '=') - name;
	if (!l || name[l])
		return 0;
	char *v = from_environ(name, l);
	if (v || l > 255)
		return v;

	/* NAME=value, read into place: the name, '=', then as much value as
	 * OS_ReadVarVal gives (it says how long it was). */
	char buf[1024];
	memcpy(buf, name, l);
	buf[l] = '=';
	unsigned r[10] = { 0 };
	r[0] = (unsigned)(unsigned long)buf;          /* R0: the name, as typed */
	r[1] = (unsigned)(unsigned long)(buf + l + 1);
	r[2] = sizeof buf - l - 2;
	r[3] = 0;
	r[4] = 3;                                      /* expanded, as a string */
	buf[l] = 0;                                    /* the name ends here for the SWI */
	if (__rosgd_swi(XOS_ReadVarVal, r, r) & 0xffffffffu)
		return 0;                                  /* no such variable */
	buf[l] = '=';
	buf[l + 1 + r[2]] = 0;
	/* overwrite 1: setenv with 0 asks getenv first, which would ask again */
	if (setenv(name, buf + l + 1, 1))
		return 0;
	return from_environ(name, l);
}
