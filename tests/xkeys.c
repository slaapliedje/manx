/*
 * xkeys - send keys and wheel turns to a window found by its name, for
 * testing xmanx where there's no xdotool (the TT):
 *
 *   xkeys [-d MS] NAME STEP...
 *
 * NAME: part of the window's name. Each STEP is a keysym name (j, space,
 * Next, Down, Return...), ctrl+KEY, wheelup or wheeldown, or sleep:MS;
 * MS (default 1000) is the wait after each one. Events are sent with
 * XSendEvent, which xmanx takes like any other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <poll.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "os.h"

static Display *dpy;

static Display *open_it(void)
{
	struct utsname u;
	char buf[300];
	Display *p = XOpenDisplay(NULL);

	if (p || uname(&u) < 0)
		return p;
	sprintf(buf, "%s:0", u.nodename);
	return XOpenDisplay(buf);
}

/* the window under w whose name has part in it (depth first), or 0 */
static Window find(Window w, const char *part)
{
	Window root, parent, *kids = NULL, f = 0;
	unsigned int n, i;
	char *name = NULL;

	if (XFetchName(dpy, w, &name) && name) {
		int hit = strstr(name, part) != NULL;

		XFree(name);
		if (hit)
			return w;
	}
	if (!XQueryTree(dpy, w, &root, &parent, &kids, &n))
		return 0;
	for (i = 0; i < n && !f; i++)
		f = find(kids[i], part);
	if (kids)
		XFree((char *)kids);
	return f;
}

static void pause_ms(unsigned long ms)
{
	poll(NULL, 0, (int)ms);
}

static void send_key(Window w, KeySym ks, unsigned int state)
{
	XKeyEvent e;

	memset(&e, 0, sizeof e);
	e.display = dpy;
	e.window = w;
	e.root = DefaultRootWindow(dpy);
	e.time = CurrentTime;
	e.x = e.y = 100;
	e.same_screen = True;
	e.state = state;
	e.keycode = XKeysymToKeycode(dpy, ks);
	e.type = KeyPress;
	XSendEvent(dpy, w, True, KeyPressMask, (XEvent *)&e);
	e.type = KeyRelease;
	XSendEvent(dpy, w, True, KeyReleaseMask, (XEvent *)&e);
}

static void send_button(Window w, unsigned int button)
{
	XButtonEvent e;

	memset(&e, 0, sizeof e);
	e.display = dpy;
	e.window = w;
	e.root = DefaultRootWindow(dpy);
	e.time = CurrentTime;
	e.x = 200;			/* (over the page) */
	e.y = 200;
	e.same_screen = True;
	e.button = button;
	e.type = ButtonPress;
	XSendEvent(dpy, w, True, ButtonPressMask, (XEvent *)&e);
	e.type = ButtonRelease;
	XSendEvent(dpy, w, True, ButtonReleaseMask, (XEvent *)&e);
}

int main(int argc, char **argv)
{
	unsigned long wait = 1000;
	Window w;
	int i = 1;

	if (argc > 2 && strcmp(argv[1], "-d") == 0) {
		wait = strtoul(argv[2], NULL, 10);
		i = 3;
	}
	if (argc - i < 1) {
		fprintf(stderr, "usage: xkeys [-d MS] NAME STEP...\n");
		return 2;
	}
	if ((dpy = open_it()) == NULL) {
		fprintf(stderr, "xkeys: no display\n");
		return 1;
	}
	if ((w = find(DefaultRootWindow(dpy), argv[i])) == 0) {
		fprintf(stderr, "xkeys: no window named like \"%s\"\n", argv[i]);
		return 1;
	}
	for (i++; i < argc; i++) {
		const char *s = argv[i];
		unsigned int state = 0;
		KeySym ks;

		if (strncmp(s, "sleep:", 6) == 0) {
			pause_ms(strtoul(s + 6, NULL, 10));
			continue;
		}
		if (strcmp(s, "wheelup") == 0 || strcmp(s, "wheeldown") == 0)
			send_button(w, s[5] == 'u' ? 4 : 5);
		else {
			if (strncmp(s, "ctrl+", 5) == 0) {
				state = ControlMask;
				s += 5;
			}
			if ((ks = XStringToKeysym((char *)s)) == NoSymbol) {
				fprintf(stderr, "xkeys: no key %s\n", s);
				return 1;
			}
			send_key(w, ks, state);
		}
		XFlush(dpy);
		pause_ms(wait);
	}
	XCloseDisplay(dpy);
	return 0;
}
