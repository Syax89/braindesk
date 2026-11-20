// SPDX-License-Identifier: GPL-2.0
/*
 * brainbar: indicatore batteria per la barra di JWM sul SHARP Brain.
 * Una finestra Xft minuscola che JWM "ingoia" nel vassoio; legge il driver
 * brain-battery ogni 10 secondi. Pochi KB di RAM, nessuna dipendenza GTK.
 *
 *   cc -O2 -o brainbar brainbar.c $(pkg-config --cflags --libs xft)
 */
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#define PS "/sys/class/power_supply/"

static int read_line(const char *path, char *buf, int len)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (!fgets(buf, len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static void battery_text(char *out, int len, int *level)
{
	char cap[16] = "?", st[32] = "", usb[8] = "0";

	read_line(PS "brain-battery/capacity", cap, sizeof(cap));
	read_line(PS "brain-battery/status", st, sizeof(st));
	read_line(PS "brain-usb/online", usb, sizeof(usb));
	*level = 0;
	sscanf(cap, "%d", level);
	snprintf(out, len, "%s %s%%",
		 !strcmp(st, "Charging") ? "⚡" : usb[0] == '1' ? "⏚" : "▮",
		 cap);
}

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	XftFont *font;
	XftDraw *draw;
	XftColor fg, warn, bg;
	XRenderColor c;
	XClassHint ch = { "brainbar", "Brainbar" };
	Window w;
	int scr, W = 64, H = 22;
	char text[64];

	if (!d)
		return 1;
	scr = DefaultScreen(d);
	font = XftFontOpenName(d, scr, "DejaVu Sans:bold:pixelsize=13");
	if (!font)
		return 1;

	w = XCreateSimpleWindow(d, RootWindow(d, scr), 0, 0, W, H, 0, 0, 0);
	XSetClassHint(d, w, &ch);
	XStoreName(d, w, "brainbar");
	XSelectInput(d, w, ExposureMask | StructureNotifyMask);

	draw = XftDrawCreate(d, w, DefaultVisual(d, scr), DefaultColormap(d, scr));
	c = (XRenderColor){ 0xffff, 0xffff, 0xffff, 0xffff };
	XftColorAllocValue(d, DefaultVisual(d, scr), DefaultColormap(d, scr), &c, &fg);
	c = (XRenderColor){ 0xffff, 0x6000, 0x4000, 0xffff };
	XftColorAllocValue(d, DefaultVisual(d, scr), DefaultColormap(d, scr), &c, &warn);
	c = (XRenderColor){ 0x2200, 0x2a00, 0x3800, 0xffff };
	XftColorAllocValue(d, DefaultVisual(d, scr), DefaultColormap(d, scr), &c, &bg);
	XSetWindowBackground(d, w, bg.pixel);
	XMapWindow(d, w);

	for (;;) {
		struct pollfd p = { ConnectionNumber(d), POLLIN, 0 };
		XGlyphInfo gi;
		int level;

		battery_text(text, sizeof(text), &level);
		XClearWindow(d, w);
		XftTextExtentsUtf8(d, font, (FcChar8 *)text, strlen(text), &gi);
		XftDrawStringUtf8(draw, level < 15 ? &warn : &fg, font,
				  (W - gi.xOff) / 2,
				  (H + font->ascent - font->descent) / 2,
				  (FcChar8 *)text, strlen(text));
		XFlush(d);

		poll(&p, 1, 10000);
		while (XPending(d)) {
			XEvent e;

			XNextEvent(d, &e);
			if (e.type == ConfigureNotify) {
				W = e.xconfigure.width;
				H = e.xconfigure.height;
			}
		}
	}
}
