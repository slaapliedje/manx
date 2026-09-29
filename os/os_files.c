/*
 * os_files.c - the browser's data directory and whole-file helpers
 * (every target: plain stdio and POSIX calls SVR4 has too).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "os.h"

#ifdef MANX_SYSV4
int rename(const char *, const char *);
int unlink(const char *);
int chmod(const char *, mode_t);
#else
#include <unistd.h>
#endif

const char *os_datadir(void)
{
	static char dir[512];
	static int done;
	const char *e;

	if (done)
		return dir[0] ? dir : NULL;
	done = 1;
	if ((e = getenv("MANX_HOME")) != NULL && *e && strlen(e) < sizeof dir)
		strcpy(dir, e);
	else if ((e = getenv("HOME")) != NULL && *e && strlen(e) + sizeof "/.manx" < sizeof dir)
		sprintf(dir, "%s/.manx", strcmp(e, "/") == 0 ? "" : e);
	else
		return NULL;
	mkdir(dir, 0700);		/* fails harmlessly if it exists */
	{
		struct stat st;

		if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
			dir[0] = '\0';
			return NULL;
		}
	}
	return dir;
}

char *os_datapath(char *buf, size_t n, const char *name)
{
	const char *d = os_datadir();

	if (d == NULL || strlen(d) + 1 + strlen(name) + 1 > n)
		return NULL;
	sprintf(buf, "%s/%s", d, name);
	return buf;
}

int os_file_info(const char *path, long *size, long *mtime)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return -1;
	if (size)
		*size = (long)st.st_size;
	if (mtime)
		*mtime = (long)st.st_mtime;
	return 0;
}

int os_write_file(const char *path, const void *data, size_t len, int mode)
{
	char tmp[600];
	FILE *f;
	int ok;

	if (strlen(path) + 5 > sizeof tmp)
		return -1;
	sprintf(tmp, "%s.tmp", path);
	f = fopen(tmp, "wb");
	if (f == NULL)
		return -1;
	chmod(tmp, (mode_t)mode);
	ok = fwrite(data, 1, len, f) == len;
	ok = (fclose(f) == 0) && ok;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

unsigned char *os_read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long size;

	if (f == NULL)
		return NULL;
	if (os_file_info(path, &size, NULL) < 0 || size < 0) {
		fclose(f);
		return NULL;
	}
	buf = xmalloc((size_t)size + 1);
	if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		xfree(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[size] = '\0';
	*len = (size_t)size;
	return buf;
}

void os_x509_now(unsigned long *days, unsigned long *seconds)
{
	unsigned long t = (unsigned long)time(NULL);

	/* 719528 days from 1 January 0 to 1 January 1970 */
	*days = t / 86400UL + 719528UL;
	*seconds = t % 86400UL;
}
