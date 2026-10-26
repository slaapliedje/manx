/*
 * sysv_rt.c - run-time pieces a statically linked AMIX program built by
 * toolchain/sysv4-cc needs and no static AMIX library provides.
 * (Adapted from OpenUA platform/unix/sysv_rt.c.)
 *
 * The binary is AMIX's, and runs on Atari System V through
 * atari-sysv-sp1's amx module.
 */
#include <string.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <netconfig.h>

int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);

/*
 * The memory limits. AMIX and Atari System V give a process 16 MB of
 * data (RLIMIT_DATA, what brk may grow to) and 16 MB of address space
 * in all (RLIMIT_VMEM, what the kernel maps for it), soft and hard
 * limits alike: a page's images and the server's copies of them reach
 * that. Both are raised to RT_MEM_MB where the process may (root may
 * raise the hard limits; anyone the soft ones up to the hard ones), and
 * a higher limit from the shell is kept.
 */
#define RT_MEM_MB	128

static void raise_limit(int res)
{
	struct rlimit rl;
	rlim_t want = (rlim_t)RT_MEM_MB << 20;

	if (getrlimit(res, &rl) != 0 || rl.rlim_cur >= want)
		return;
	if (rl.rlim_max < want) {
		rl.rlim_max = want;
		rl.rlim_cur = want;
		if (setrlimit(res, &rl) == 0)
			return;
		getrlimit(res, &rl);		/* not root: the hard limit stays */
	}
	rl.rlim_cur = rl.rlim_max < want ? rl.rlim_max : want;
	setrlimit(res, &rl);
}

/* GCC for an a.out target calls __main at the top of main() to run
 * constructors; there are none. The memory limits are raised here, before
 * anything of size is allocated. */
void __main(void)
{
	raise_limit(RLIMIT_DATA);
	raise_limit(RLIMIT_VMEM);
}

/*
 * libsocket.a's socket() finds its transport device (/dev/tcp ...) in the
 * netconfig database, whose reader is only in the shared libnsl.so. These
 * are the inet entries of /etc/netconfig. The system's name resolver is a
 * shared object too, so the browser carries its own (net/dns.c).
 */
static struct netconfig nc_inet[] = {
	{ "tcp", NC_TPI_COTS_ORD, NC_VISIBLE, NC_INET, "tcp", "/dev/tcp", 0, 0, { 0 } },
	{ "udp", NC_TPI_CLTS,     NC_VISIBLE, NC_INET, "udp", "/dev/udp", 0, 0, { 0 } },
};
#define NNC	(int)(sizeof nc_inet / sizeof nc_inet[0])

void *setnetconfig_c(void)
{
	static int pos[8];		/* 1 + index of the next entry */
	int i;

	for (i = 0; i < 8; i++)
		if (pos[i] == 0) {
			pos[i] = 1;
			return &pos[i];
		}
	return 0;
}

struct netconfig *getnetconfig_c(void *h)
{
	int *p = h;

	if (p == 0 || *p < 1 || *p > NNC)
		return 0;
	return &nc_inet[(*p)++ - 1];
}

int endnetconfig(void *h)
{
	if (h)
		*(int *)h = 0;
	return 0;
}

/*
 * Called by the SVR4 libraries, and returning pointers: SVR4 code reads a
 * pointer result from a0, while GCC for an a.out target leaves it in d0
 * alone. So the entry points are these, which copy d0 to a0.
 */
__asm__(
"	.text\n"
"	.globl	setnetconfig\n"
"setnetconfig:\n"
"	jsr	setnetconfig_c\n"
"	move.l	%d0,%a0\n"
"	rts\n"
"	.globl	getnetconfig\n"
"getnetconfig:\n"
"	move.l	4(%sp),-(%sp)\n"
"	jsr	getnetconfig_c\n"
"	lea	4(%sp),%sp\n"
"	move.l	%d0,%a0\n"
"	rts\n");

int gethostname(char *name, int len)
{
	struct utsname u;

	if (uname(&u) < 0)
		return -1;
	strncpy(name, u.nodename, len);
	if (len > 0)
		name[len - 1] = '\0';
	return 0;
}
