// SPDX-License-Identifier: MIT
/*
 * view_calc.c - Calcolatrice: tastierino a tocco + tasti fisici.
 *
 * I conti usano double: sul Brain sono chiamate di libreria (niente FPU),
 * ma avvengono una volta per tasto premuto, mai nel disegno.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

#define MAX_HIST 24
#define MAX_DIGITS 12

static struct {
	bui_panel p;
	bui_rect disp;			/* scheda del display, coordinate della finestra */
	char entry[32];			/* cifre che l'utente sta scrivendo */
	char expr[80];			/* riga sopra: "12 x 3 =" */
	double acc;
	int op;				/* operazione in sospeso: + - * / oppure 0 */
	int fresh;			/* si e' appena premuto "=" */
	int error;
	bui_list hist;
	char h_expr[MAX_HIST][80];
	char h_res[MAX_HIST][40];
	double h_val[MAX_HIST];
	int nhist;
} C;

static const char *op_sym(int op)
{
	switch (op) {
	case '+':
		return "+";
	case '-':
		return "\xE2\x88\x92";
	case '*':
		return "\xC3\x97";
	case '/':
		return "\xC3\xB7";
	}
	return "";
}

static void fmt_num(double v, char *buf, size_t len)
{
	char *p;

	if (v == 0)
		v = 0;	/* niente "-0" */
	snprintf(buf, len, "%.10g", v);
	for (p = buf; *p; p++)
		if (*p == '.')
			*p = ',';
}

/* per la visualizzazione: il trattino diventa un vero segno meno */
static void pretty(const char *num, char *buf, size_t len)
{
	if (num[0] == '-')
		snprintf(buf, len, "\xE2\x88\x92%s", num + 1);
	else
		snprintf(buf, len, "%s", num);
}

static double entry_val(void)
{
	char tmp[sizeof(C.entry)], *p;

	if (!C.entry[0])
		return C.acc;
	snprintf(tmp, sizeof(tmp), "%s", C.entry);
	for (p = tmp; *p; p++)
		if (*p == ',')
			*p = '.';
	return strtod(tmp, NULL);
}

static void redraw_display(void)
{
	bs_view_damage(bui_r(C.disp.x, C.disp.y, C.disp.w, 132));
}

static void hist_add(const char *expr, double val)
{
	char num[32];

	if (C.nhist == MAX_HIST)
		C.nhist--;
	memmove(C.h_expr[1], C.h_expr[0], sizeof(C.h_expr[0]) * (size_t)C.nhist);
	memmove(C.h_res[1], C.h_res[0], sizeof(C.h_res[0]) * (size_t)C.nhist);
	memmove(&C.h_val[1], &C.h_val[0], sizeof(C.h_val[0]) * (size_t)C.nhist);
	snprintf(C.h_expr[0], sizeof(C.h_expr[0]), "%s", expr);
	fmt_num(val, num, sizeof(num));
	pretty(num, C.h_res[0], sizeof(C.h_res[0]));
	C.h_val[0] = val;
	C.nhist++;
	C.hist.sel = -1;
	bui_list_set_count(&C.hist, C.nhist);
	bui_list_reveal(&C.hist, 0);
}

static int apply(double a, int op, double b, double *out)
{
	switch (op) {
	case '+':
		*out = a + b;
		break;
	case '-':
		*out = a - b;
		break;
	case '*':
		*out = a * b;
		break;
	case '/':
		if (b == 0)
			return -1;
		*out = a / b;
		break;
	default:
		*out = b;
		break;
	}
	/* fuori scala (infinito o NaN): errore */
	if (*out != *out || *out > 1e300 || *out < -1e300)
		return -1;
	return 0;
}

static void clear_all(void)
{
	C.entry[0] = 0;
	C.expr[0] = 0;
	C.acc = 0;
	C.op = 0;
	C.fresh = 0;
	C.error = 0;
}

static void press(int k)
{
	size_t n = strlen(C.entry);
	char a[32], b[32];
	double r;

	if (C.error && k != 'C')
		clear_all();
	switch (k) {
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		if (C.fresh)
			clear_all();
		if (!strcmp(C.entry, "0"))
			n = 0;
		else if (!strcmp(C.entry, "-0"))
			n = 1;
		if (n - (C.entry[0] == '-') - (strchr(C.entry, ',') != NULL) >= MAX_DIGITS)
			break;
		C.entry[n] = (char)k;
		C.entry[n + 1] = 0;
		break;
	case ',':
		if (C.fresh)
			clear_all();
		if (strchr(C.entry, ','))
			break;
		if (!n || !strcmp(C.entry, "-"))
			snprintf(C.entry + n, sizeof(C.entry) - n, "0,");
		else if (n < sizeof(C.entry) - 2)
			strcat(C.entry, ",");
		break;
	case 'B':	/* cancella l'ultima cifra */
		if (n)
			C.entry[n - 1] = 0;
		if (!strcmp(C.entry, "-"))
			C.entry[0] = 0;
		break;
	case 'C':
		if (C.entry[0] && !C.error)
			C.entry[0] = 0;
		else
			clear_all();
		break;
	case 'N':	/* cambia segno */
		if (C.entry[0]) {
			if (C.entry[0] == '-')
				memmove(C.entry, C.entry + 1, n);
			else if (n < sizeof(C.entry) - 1) {
				memmove(C.entry + 1, C.entry, n + 1);
				C.entry[0] = '-';
			}
		} else {
			C.acc = -C.acc;
			if (C.fresh)
				C.expr[0] = 0;
		}
		break;
	case '%':
		r = C.op ? C.acc * entry_val() / 100 : entry_val() / 100;
		fmt_num(r, C.entry, sizeof(C.entry));
		C.fresh = 0;
		break;
	case '+': case '-': case '*': case '/':
		if (C.entry[0] && C.op) {
			if (apply(C.acc, C.op, entry_val(), &r) < 0) {
				clear_all();
				C.error = 1;
				break;
			}
			C.acc = r;
		} else if (C.entry[0]) {
			C.acc = entry_val();
		}
		C.op = k;
		C.fresh = 0;
		C.entry[0] = 0;
		fmt_num(C.acc, a, sizeof(a));
		snprintf(C.expr, sizeof(C.expr), "%s %s", a, op_sym(k));
		break;
	case '=':
		if (!C.op)
			break;
		fmt_num(C.acc, a, sizeof(a));
		fmt_num(entry_val(), b, sizeof(b));
		snprintf(C.expr, sizeof(C.expr), "%s %s %s =", a, op_sym(C.op), b);
		if (apply(C.acc, C.op, entry_val(), &r) < 0) {
			clear_all();
			C.error = 1;
			break;
		}
		C.acc = r;
		C.op = 0;
		C.entry[0] = 0;
		C.fresh = 1;
		hist_add(C.expr, r);
		break;
	}
	redraw_display();
}

static void on_key(bui_widget *w)
{
	press(w->id);
}

static void hist_row(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	(void)l;
	if (flags & BUI_F_PRESSED)
		bui_rrect(c, bui_rect_inset(r, 6, 2), 12, BUI_C_SURFACE2);
	if (flags & BUI_F_FOCUS)
		bui_rrect_line(c, bui_rect_inset(r, 6, 2), 12, 2, BUI_C_ACCENT);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(r.x + 20, r.y + 6, r.w - 40, 20), C.h_expr[i], BUI_C_TEXT2,
		     BUI_AL_RIGHT);
	bui_text_box(c, BUI_FONT_BODY_B, bui_r(r.x + 20, r.y + 28, r.w - 40, 22), C.h_res[i], BUI_C_TEXT,
		     BUI_AL_RIGHT);
}

static void hist_select(bui_list *l, int i)
{
	(void)l;
	/* riprende un risultato precedente come numero corrente */
	if (C.error)
		clear_all();
	if (C.fresh) {
		C.fresh = 0;
		C.expr[0] = 0;
	}
	fmt_num(C.h_val[i], C.entry, sizeof(C.entry));
	redraw_display();
}

static void draw_display(bui_widget *w, bui_canvas *c, bui_rect r)
{
	char num[40];
	const bui_font *f = BUI_FONT_NUM;
	bui_rect tr = bui_r(r.x + 20, r.y + 52, r.w - 40, 64);

	(void)w;
	bui_rrect(c, r, 16, BUI_C_SURFACE);
	bui_text_box(c, BUI_FONT_BODY, bui_r(r.x + 20, r.y + 14, r.w - 40, 28), C.expr, BUI_C_TEXT2,
		     BUI_AL_RIGHT);
	if (C.error) {
		bui_text_box(c, BUI_FONT_TITLE, tr, "Errore", BUI_C_DANGER, BUI_AL_RIGHT);
	} else {
		char raw[32];

		if (C.entry[0])
			snprintf(raw, sizeof(raw), "%s", C.entry);
		else
			fmt_num(C.acc, raw, sizeof(raw));
		pretty(raw, num, sizeof(num));
		if (bui_text_width(f, num, -1) > tr.w)
			f = BUI_FONT_KEY;
		bui_text_box(c, f, tr, num, BUI_C_TEXT, BUI_AL_RIGHT);
	}
	bui_fill(c, bui_r(r.x + 20, r.y + 131, r.w - 40, 1), BUI_C_LINE);
}

static void cl_layout(bs_view *v, bui_rect a)
{
	static const struct {
		const char *label;
		int id, style;
	} keys[20] = {
		{ "C", 'C', BUI_BS_KEY_OP }, { "\xC2\xB1", 'N', BUI_BS_KEY_OP },
		{ "%", '%', BUI_BS_KEY_OP }, { "\xC3\xB7", '/', BUI_BS_KEY_OP },
		{ "7", '7', BUI_BS_KEY }, { "8", '8', BUI_BS_KEY }, { "9", '9', BUI_BS_KEY },
		{ "\xC3\x97", '*', BUI_BS_KEY_OP },
		{ "4", '4', BUI_BS_KEY }, { "5", '5', BUI_BS_KEY }, { "6", '6', BUI_BS_KEY },
		{ "\xE2\x88\x92", '-', BUI_BS_KEY_OP },
		{ "1", '1', BUI_BS_KEY }, { "2", '2', BUI_BS_KEY }, { "3", '3', BUI_BS_KEY },
		{ "+", '+', BUI_BS_KEY_OP },
		{ "0", '0', BUI_BS_KEY }, { ",", ',', BUI_BS_KEY }, { NULL, 'B', BUI_BS_KEY },
		{ "=", '=', BUI_BS_FILLED },
	};
	bui_panel *p = &C.p;
	int land = a.w > a.h;
	int pad = 16, gap = 8, kw, kh, kx, ky, i;
	bui_rect d;
	bui_widget *w;

	(void)v;
	bui_panel_clear(p);
	bui_panel_set_viewport(p, a, a.h);
	if (land) {
		kw = 96;
		kh = (a.h - 2 * 12 - 4 * gap) / 5;
		kx = a.w - pad - (4 * kw + 3 * gap);
		ky = 12;
		d = bui_r(pad, 12, kx - 2 * pad, a.h - 24);
	} else {
		kw = (a.w - 2 * pad - 3 * gap) / 4;
		kh = (a.h * 58 / 100 - 4 * gap) / 5;
		if (kh > 84)
			kh = 84;
		kx = pad;
		ky = a.h - 12 - (5 * kh + 4 * gap);
		d = bui_r(pad, 12, a.w - 2 * pad, ky - 12 - 12);
	}
	C.disp = bui_r(a.x + d.x, a.y + d.y, d.w, d.h);
	bui_add_custom(p, d, draw_display);
	C.hist.row_h = 56;
	C.hist.count = C.nhist;
	C.hist.draw_row = hist_row;
	C.hist.empty_text = "I risultati compariranno qui";
	C.hist.on_select = hist_select;
	if (d.h > 132 + 56)
		bui_add_list(p, bui_r(d.x, d.y + 136, d.w, d.h - 136 - 8), &C.hist);
	for (i = 0; i < 20; i++) {
		w = bui_add_button(p, bui_r(kx + (i % 4) * (kw + gap), ky + (i / 4) * (kh + gap), kw, kh),
				   keys[i].label, NULL, keys[i].style, on_key);
		w->id = keys[i].id;
		if (keys[i].id == 'B')
			w->icon = BUI_IC(backspace, 24);
		if (keys[i].id == '=')
			w->font = BUI_FONT_KEY;
	}
}

static void cl_enter(bs_view *v)
{
	bui_panel_init(&C.p, v->win);
	C.hist.sel = -1;
}

static void cl_leave(bs_view *v)
{
	(void)v;
	bui_panel_clear(&C.p);
}

static void cl_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&C.p, c, clip);
}

static int cl_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&C.p, ev);
}

static int cl_key(bs_view *v, const bui_key *k)
{
	(void)v;
	switch (k->key) {
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
	case '+': case '-': case '*': case '/': case '%':
		press(k->key);
		return 1;
	case 'x': case 'X':
		press('*');
		return 1;
	case ':':
		press('/');
		return 1;
	case ',': case '.':
		press(',');
		return 1;
	case '=':
		press('=');
		return 1;
	case BUI_K_ENTER:
		/* col fuoco visibile Invio attiva il tasto evidenziato */
		if (C.p.focus_vis)
			return bui_panel_key(&C.p, k);
		press('=');
		return 1;
	case BUI_K_BACKSPACE:
		if (!C.entry[0])
			return 0;	/* niente da cancellare: torna alla home */
		press('B');
		return 1;
	case BUI_K_DELETE: case 'c': case 'C':
		press('C');
		return 1;
	}
	return bui_panel_key(&C.p, k);
}

bs_view bs_view_calc = {
	.enter = cl_enter,
	.leave = cl_leave,
	.layout = cl_layout,
	.draw = cl_draw,
	.pointer = cl_pointer,
	.key = cl_key,
};
