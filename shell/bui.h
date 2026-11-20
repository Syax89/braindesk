// SPDX-License-Identifier: MIT
/*
 * bui - Brain UI: toolkit minimo per schermi touch RGB565.
 *
 * Tutto il disegno e' software, su tele RGB565 (una per finestra). Solo i
 * rettangoli modificati vengono ridisegnati e inviati. Niente float, niente
 * font a runtime: glifi e icone sono tabelle A8 generate da tools/bake.c.
 *
 * bui.c       tela, primitive, testo, icone
 * bui_core.c  scelta del backend, ciclo eventi (poll), timer, animazioni, descrittori
 * bui_x.c     backend X11: finestre, XShm, popup
 * bui_comp.c  compositore software per drm/mem: finestre come livelli
 * bui_drm.c   backend DRM (pannello) + bui_evdev.c (tocco e tastiera)
 * bui_mem.c   backend in memoria guidato da script (prove)
 * bui_w.c     widget, pannelli, scorrimento cinetico, liste, dialoghi, toast
 */
#ifndef BUI_H
#define BUI_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Colore                                                             */
/* ------------------------------------------------------------------ */

typedef uint16_t bui_px;	/* un pixel RGB565 */

#define BUI_RGB(r, g, b) \
	((bui_px)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xFF) >> 3)))
#define BUI_HEX(h) BUI_RGB(((h) >> 16) & 0xFF, ((h) >> 8) & 0xFF, (h) & 0xFF)

/*
 * Tema: i colori dell'interfaccia sono campi del tema corrente (bui_th),
 * scelto a caldo con bui_theme_set(). Le macro BUI_C_* restano il modo di
 * usarli: sono espressioni lette al momento del disegno, quindi NON vanno
 * usate in inizializzatori statici ne' conservate oltre un cambio di tema
 * (dopo un cambio la shell richiama layout() della vista attiva).
 */
enum { BUI_TILE_FILLED, BUI_TILE_GLASS, BUI_TILE_OUTLINE };

typedef struct bui_blob {
	uint8_t x, y, r;	/* centro e raggio in percentuale del lato */
	uint32_t rgb;		/* luce aggiunta (0xRRGGBB); 0 = nessuna */
} bui_blob;

typedef struct bui_theme {
	const char *id;		/* nome nel file di configurazione */
	const char *name;	/* nome mostrato */
	bui_px bg, bar, surface, surface2, surface3, line;
	bui_px text, text2, text3;
	bui_px accent, accent_p, accent_bg, on_accent;
	bui_px danger, danger_p, on_danger, warn;
	uint8_t r_tile, r_card, r_btn;	/* raggi di riquadri, schede, pulsanti */
	uint8_t tile_style;	/* BUI_TILE_* */
	uint8_t light;		/* tema chiaro */
	uint8_t tile_shadow;	/* ombra sotto i riquadri (px, 0 = nessuna) */
	bui_px tint[6];		/* tinte delle icone delle app; tint[0] == 0: solo accento */
	/* sfondo procedurale: sfumatura diagonale + aloni + grana */
	uint32_t wall0, wall1;
	bui_blob blob[3];
	uint8_t grain;		/* ampiezza del rumore (livelli su 255) */
} bui_theme;

extern bui_theme bui_th;	/* tema corrente */

int bui_theme_count(void);
const bui_theme *bui_theme_get(int i);
int bui_theme_find(const char *id);	/* -1 se non esiste */
int bui_theme_current(void);
void bui_theme_set(int i);		/* chi chiama ridisegna le finestre */

#define BUI_C_BG	(bui_th.bg)		/* sfondo delle viste */
#define BUI_C_BAR	(bui_th.bar)		/* barra di stato */
#define BUI_C_SURFACE	(bui_th.surface)	/* schede, riquadri */
#define BUI_C_SURFACE2	(bui_th.surface2)	/* controlli */
#define BUI_C_SURFACE3	(bui_th.surface3)	/* controlli premuti */
#define BUI_C_LINE	(bui_th.line)		/* separatori */
#define BUI_C_TEXT	(bui_th.text)		/* testo principale */
#define BUI_C_TEXT2	(bui_th.text2)		/* testo secondario */
#define BUI_C_TEXT3	(bui_th.text3)		/* testo disattivato */
#define BUI_C_ACCENT	(bui_th.accent)		/* accento */
#define BUI_C_ACCENT_P	(bui_th.accent_p)	/* accento premuto */
#define BUI_C_ACCENT_BG	(bui_th.accent_bg)	/* fondo tinto d'accento */
#define BUI_C_ON_ACCENT	(bui_th.on_accent)	/* testo sopra l'accento */
#define BUI_C_DANGER	(bui_th.danger)		/* azioni distruttive */
#define BUI_C_DANGER_P	(bui_th.danger_p)
#define BUI_C_ON_DANGER	(bui_th.on_danger)
#define BUI_C_WARN	(bui_th.warn)		/* avvisi (batteria bassa) */

#define BUI_U		8	/* passo della griglia */
#define BUI_TOUCH	56	/* lato minimo di un bersaglio per il dito */
#define BUI_DRAG_SLOP	12	/* px di movimento prima che un tocco diventi trascinamento */
#define BUI_FRAME_MS	22	/* ~45 fotogrammi al secondo mentre qualcosa si muove: il
				 * pannello riceve un quadro intero in 17 ms (bui_comp) */

/* ------------------------------------------------------------------ */
/* Geometria                                                          */
/* ------------------------------------------------------------------ */

typedef struct bui_rect {
	int x, y, w, h;
} bui_rect;

static inline bui_rect bui_r(int x, int y, int w, int h)
{
	bui_rect r = { x, y, w, h };
	return r;
}

static inline int bui_rect_has(bui_rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline bui_rect bui_rect_inset(bui_rect r, int dx, int dy)
{
	r.x += dx;
	r.y += dy;
	r.w -= 2 * dx;
	r.h -= 2 * dy;
	return r;
}

/* intersezione; ritorna 0 se vuota */
int bui_rect_isect(bui_rect a, bui_rect b, bui_rect *out);
bui_rect bui_rect_union(bui_rect a, bui_rect b);

/* ------------------------------------------------------------------ */
/* Tela e primitive                                                   */
/* ------------------------------------------------------------------ */

typedef struct bui_canvas {
	bui_px *px;
	int w, h;
	int stride;		/* pixel per riga */
	bui_rect clip;		/* ogni primitiva e' limitata a questo rettangolo */
} bui_canvas;

/* restringe il clip (interseca) e ritorna quello precedente da ripristinare */
bui_rect bui_clip_push(bui_canvas *c, bui_rect r);
static inline void bui_clip_pop(bui_canvas *c, bui_rect saved)
{
	c->clip = saved;
}

void bui_fill(bui_canvas *c, bui_rect r, bui_px col);
void bui_fill_a(bui_canvas *c, bui_rect r, bui_px col, int alpha);	/* alpha 0..255 */
/* sfumatura verticale da rgb0 (0xRRGGBB, in alto) a rgb1, con dithering ordinato */
void bui_grad_v(bui_canvas *c, bui_rect r, uint32_t rgb0, uint32_t rgb1);
/* rettangolo arrotondato pieno, angoli con antialiasing */
void bui_rrect(bui_canvas *c, bui_rect r, int radius, bui_px col);
void bui_rrect_a(bui_canvas *c, bui_rect r, int radius, bui_px col, int alpha);
/* solo il bordo, spesso t pixel */
void bui_rrect_line(bui_canvas *c, bui_rect r, int radius, int t, bui_px col);
/* maschera A8 colorata (testo, icone) */
void bui_mask(bui_canvas *c, int x, int y, const uint8_t *a8, int w, int h, int stride,
	      bui_px col, int alpha);
/* immagine RGB565 opaca */
void bui_blit(bui_canvas *c, int x, int y, const bui_px *src, int w, int h, int stride);
/* come bui_blit, miscelata col fondo: alpha 0..255 */
void bui_blit_a(bui_canvas *c, int x, int y, const bui_px *src, int w, int h, int stride, int alpha);
/* copia dentro il rettangolo arrotondato r i pixel che `src` (immagine grande
 * quanto la finestra) ha nella stessa posizione: il "vetro" dei riquadri */
void bui_blit_rr(bui_canvas *c, bui_rect r, int radius, const bui_px *src, int sw, int sh, int stride);
/* ombra morbida attorno a un rettangolo arrotondato. La forma (angolo e
 * profilo dei bordi) e' una maschera A8 calcolata una volta per ogni coppia
 * raggio/ampiezza: disegnarla costa solo la miscela dell'anello esterno */
void bui_shadow(bui_canvas *c, bui_rect r, int radius, int spread);
/* pixel vuoti a sinistra nella riga `row` di un angolo di raggio `radius` */
int bui_corner_inset(int radius, int row);

/* miscela due colori: a = 0 (tutto bg) .. 255 (tutto fg) */
bui_px bui_mix(bui_px bg, bui_px fg, int a);

/* matrice di Bayer 8x8 (0..63) e conversione con dithering da canali a 8.12 bit */
extern const uint8_t bui_bayer8[64];
static inline bui_px bui_pack_dither(int r, int g, int b, int x, int y)
{
	int t = bui_bayer8[((y & 7) << 3) | (x & 7)];

	r = (r + (t << 9)) >> 15;
	g = (g + (t << 8)) >> 14;
	b = (b + (t << 9)) >> 15;
	if (r > 31)
		r = 31;
	if (g > 63)
		g = 63;
	if (b > 31)
		b = 31;
	return (bui_px)((r << 11) | (g << 5) | b);
}

/* ------------------------------------------------------------------ */
/* Testo e icone (dati generati in gen/)                              */
/* ------------------------------------------------------------------ */

typedef struct bui_glyph {
	uint16_t cp;		/* codice Unicode */
	uint16_t adv;		/* avanzamento in 1/64 di pixel */
	uint32_t off;		/* posizione nella tabella A8 */
	uint8_t w, h;
	int8_t bx, by;		/* origine: sinistra, righe sopra la linea di base */
} bui_glyph;

typedef struct bui_font {
	const bui_glyph *glyphs;	/* ordinati per codice */
	const uint8_t *data;
	const uint16_t *kpair;		/* coppie (sinistra << 8 | destra), ordinate */
	const int8_t *kdx;		/* correzione in 1/64 di pixel */
	uint16_t nglyphs, nkern;
	uint8_t size;			/* dimensione nominale (px per em) */
	uint8_t ascent, descent, line_h;
	uint8_t cap_h, x_h;		/* altezza delle maiuscole e della x */
} bui_font;

typedef struct bui_icon {
	uint8_t size;		/* lato della cella (24, 40, ...) */
	uint8_t w, h, ox, oy;	/* maschera ritagliata e sua posizione nella cella */
	const uint8_t *a8;
} bui_icon;

#include "gen/bui_assets.h"

#define BUI_FONT_SMALL		(&bui_font_sans15)
#define BUI_FONT_BODY		(&bui_font_sans18)
#define BUI_FONT_SMALL_B	(&bui_font_bold15)
#define BUI_FONT_BODY_B		(&bui_font_bold18)
#define BUI_FONT_TITLE		(&bui_font_bold22)
#define BUI_FONT_KEY		(&bui_font_key26)	/* solo cifre e operatori */
#define BUI_FONT_NUM		(&bui_font_num44)	/* solo cifre e operatori */
#define BUI_FONT_HUGE		(&bui_font_num88)	/* solo cifre : . , - */
#define BUI_FONT_MONO		(&bui_font_mono13)	/* terminale: celle da 8x16 */
#define BUI_FONT_MONO_B		(&bui_font_monob13)

#define BUI_IC(name, px)	(&bui_ic_##name##_##px)

enum {
	BUI_AL_LEFT = 0,
	BUI_AL_CENTER = 1,
	BUI_AL_RIGHT = 2,
	BUI_AL_TOP = 4,		/* in verticale il default e' centrato */
	BUI_AL_BOTTOM = 8,
};

/* larghezza in pixel di una stringa UTF-8 (len < 0: fino al terminatore) */
int bui_text_width(const bui_font *f, const char *s, int len);
/* disegna con la linea di base in y; ritorna la larghezza */
int bui_text(bui_canvas *c, const bui_font *f, int x, int y, const char *s, bui_px col);
/* una riga dentro un rettangolo, allineata; se non entra viene troncata con "..." */
void bui_text_box(bui_canvas *c, const bui_font *f, bui_rect r, const char *s, bui_px col, int align);
/* testo su piu' righe con a capo automatico; ritorna l'altezza usata.
 * Con c == NULL misura soltanto. */
int bui_text_wrap(bui_canvas *c, const bui_font *f, bui_rect r, const char *s, bui_px col,
		  int line_h, int align);

void bui_icon_draw(bui_canvas *c, const bui_icon *ic, int x, int y, bui_px col);
/* icona centrata nel rettangolo */
void bui_icon_in(bui_canvas *c, const bui_icon *ic, bui_rect r, bui_px col);

/* ------------------------------------------------------------------ */
/* Eventi di ingresso                                                 */
/* ------------------------------------------------------------------ */

enum { BUI_PRESS, BUI_MOVE, BUI_RELEASE, BUI_WHEEL, BUI_CANCEL };

typedef struct bui_pointer {
	int type;
	int x, y;		/* coordinate della finestra */
	int dy;			/* BUI_WHEEL: -1 su, +1 giu' */
	uint32_t time;		/* ms (orologio del server X) */
} bui_pointer;

enum {
	/* i caratteri stampabili arrivano col loro codice Unicode (Latin-1, piu' l'euro
	 * e gli altri simboli sotto 0x10000 che la mappa dei tasti produce) */
	BUI_K_UP = 0x10000, BUI_K_DOWN, BUI_K_LEFT, BUI_K_RIGHT,
	BUI_K_ENTER, BUI_K_ESC, BUI_K_BACKSPACE, BUI_K_TAB, BUI_K_DELETE,
	BUI_K_PGUP, BUI_K_PGDN, BUI_K_HOME, BUI_K_END,
	/* tasto di accensione (evdev KEY_POWER): pressione e rilascio separati,
	 * mai ripetuto; lo usa la shell (power.c) prima che arrivi alle finestre */
	BUI_K_POWER, BUI_K_POWER_UP,
	/* tasto Home del Brain (evdev KEY_HOMEPAGE, X XF86HomePage): torna alla
	 * griglia da ovunque; anche questo lo intercetta la shell */
	BUI_K_HOMEPAGE,
	BUI_K_INSERT,		/* Ins (sul Brain: il tasto "My Dictionary") */
};
/*
 * Rilascio di un tasto del dizionario (PagSu, PagGiu', Tab, Ins, Canc: sul
 * Brain Kokugo, Eiwa, Brain English, My Dictionary, Rireki): key = tasto |
 * BUI_K_RELEASED. Lo vede solo l'aggancio della shell (bui_set_key_hook),
 * mai una finestra; serve a distinguere tocco breve e pressione lunga. Solo
 * da evdev (backend drm e mem). Con Ctrl i tasti di lettera arrivano come
 * codice di controllo (1..31, 127) e BUI_MOD_CTRL: le viste che scrivono
 * testo li ignorano gia' (< 0x20), il terminale li manda cosi' come sono.
 */
#define BUI_K_RELEASED 0x01000000
enum { BUI_MOD_SHIFT = 1, BUI_MOD_CTRL = 2, BUI_MOD_ALT = 4 };

typedef struct bui_key {
	int key;
	unsigned mods;
} bui_key;

/* ------------------------------------------------------------------ */
/* Finestre e ciclo eventi                                            */
/* ------------------------------------------------------------------ */

enum { BUI_WIN_NORMAL, BUI_WIN_DOCK, BUI_WIN_DESKTOP, BUI_WIN_POPUP };

#define BUI_MAX_DIRTY 8

typedef struct bui_win bui_win;
struct bui_win {
	int x, y, w, h;		/* posizione sullo schermo e dimensione */
	int kind;
	bui_canvas cv;
	void *user;
	/* ridisegna tutto cio' che interseca clip (il clip della tela e' gia' impostato) */
	void (*paint)(bui_win *w, bui_canvas *c, bui_rect clip);
	void (*pointer)(bui_win *w, const bui_pointer *ev);
	void (*key)(bui_win *w, const bui_key *ev);
	/* dimensione o posizione sullo schermo (x, y) cambiate */
	void (*resized)(bui_win *w);
	/* fondo catturato sotto un popup (NULL se non disponibile) */
	bui_px *backdrop;

	/* --- privato --- */
	unsigned long xid;
	void *img, *shm;	/* immagine condivisa che fa da tela */
	void *img2, *shm2;	/* conversione per visual non RGB565 */
	bui_rect dirty[BUI_MAX_DIRTY];
	int ndirty;
	bui_rect expose;
	uint32_t last_flush, put_time;
	uint8_t has_expose, put_pending, mapped, grabbed, own_px, down, skip_expose;
	bui_rect want;		/* geometria richiesta da bui_win_move_resize */
	int remap;
	/* compositore (drm/mem) */
	uint8_t radius;		/* popup arrotondato: angoli trasparenti */
	uint8_t input;		/* puo' ricevere la tastiera (desktop) */
};

/*
 * Backend (scelto a bui_init): BRAINSHELL_BACKEND=drm|x11|mem; senza la
 * variabile "drm" se $DISPLAY non c'e' e /dev/dri/card0 si puo' usare,
 * altrimenti "x11". Ritorna 0, -1 se il backend non parte, BUI_INIT_NEED_X11
 * se serve X11 ma questo binario e' stato compilato senza (vedi brainshell-x11).
 */
#define BUI_INIT_NEED_X11 (-2)
int bui_init(void);
const char *bui_backend_name(void);	/* "x11", "drm", "mem" */
int bui_backend_is_x11(void);
/*
 * Cede lo schermo e l'ingresso (on = 1: master DRM lasciato, dispositivi
 * evdev non piu' catturati) oppure li riprende (0: tutto ridisegnato).
 * Solo drm/mem; ritorna 0 se riuscito.
 */
int bui_suspend(int on);
int bui_is_suspended(void);
/*
 * Filtri d'ingresso: chiamati per ogni tocco (kind BUI_PRESS/MOVE/RELEASE,
 * k NULL) e tasto (kind BUI_IN_KEY), anche a schermo ceduto (suspended = 1,
 * nel qual caso l'evento non arriva comunque alle finestre). Ritornano 1
 * per scartare l'evento. Al massimo 4.
 */
#define BUI_IN_KEY 100
int bui_add_input_filter(int (*f)(int kind, const bui_key *k, int suspended));
/*
 * Dopo una sospensione: gli eventi d'ingresso con un tempo precedente a `t`
 * (accodati dal kernel mentre il sistema dormiva o si svegliava) vengono
 * scartati.
 */
void bui_input_ignore_before(uint32_t t);
/*
 * Aggancio dei tasti: chiamato per ogni tasto che i filtri non hanno
 * scartato (mai a schermo ceduto), prima della finestra che lo riceverebbe.
 * Ritorna 1 se l'ha usato. Riceve anche i rilasci BUI_K_RELEASED.
 */
void bui_set_key_hook(int (*hook)(const bui_key *k));
/* traduce ogni testo intero che il toolkit disegna o misura (NULL: nessuna) */
void bui_set_translator(const char *(*tr)(const char *s));
/* ridisegna e reinvia tutte le finestre (il pannello puo' aver perso l'immagine) */
void bui_redraw_all(void);
/* porta a termine subito tutte le animazioni in corso */
void bui_anim_finish_all(void);
/* backend "mem": riga di stato confrontata dal comando "expect" degli script */
void bui_set_state_query(void (*q)(char *buf, int len));

void bui_run(void);			/* ciclo eventi: ritorna dopo bui_quit() */
void bui_quit(void);
void bui_shutdown(void);		/* distrugge le finestre e chiude il display */
void bui_sync(void);			/* attende che il server abbia eseguito tutto (solo misure) */
/* ms, orologio monotono che conta anche il tempo in sospensione
 * (CLOCK_BOOTTIME): i timer scaduti durante il sonno scattano al risveglio */
/* tempi dei fotogrammi su stderr (file /tmp/bui-stats all'avvio) */
int bui_stats_on(void);
uint32_t bui_now(void);
int bui_screen_w(void);
int bui_screen_h(void);

bui_win *bui_win_create(int kind, bui_rect r, const char *name);
void bui_win_destroy(bui_win *w);
void bui_win_show(bui_win *w);
void bui_win_hide(bui_win *w);
void bui_win_move_resize(bui_win *w, bui_rect r);
/* riserva spazio in alto (EWMH _NET_WM_STRUT) per una finestra dock */
void bui_win_set_strut_top(bui_win *w, int h);
void bui_win_damage(bui_win *w, bui_rect r);
void bui_win_damage_all(bui_win *w);
/* i pixel di r sono gia' giusti nella tela (scritti fuori da paint, es. righe
 * spostate con memmove): inviarli senza ridisegnare */
void bui_win_present(bui_win *w, bui_rect r);

/* popup: finestra senza gestore, sopra tutto. Cattura lo schermo sotto di se'
 * (backdrop) cosi' angoli arrotondati e ombre si fondono con cio' che c'e'. */
#define BUI_POPUP_MODAL	1	/* cattura puntatore e tastiera */
#define BUI_POPUP_DIM	2	/* scurisce il fondo catturato */
bui_win *bui_popup_open(bui_rect r, unsigned flags);
/* popup non modale a forma di rettangolo arrotondato (estensione Shape): non
 * cattura il fondo, quindi resta corretto anche se sotto lo schermo cambia */
bui_win *bui_popup_open_round(bui_rect r, int radius);
void bui_popup_close(bui_win *w);
/* istante (bui_now) in cui e' stato chiuso l'ultimo popup */
uint32_t bui_popup_last_close(void);
/* disegna il fondo catturato (o un colore pieno se manca) */
void bui_popup_backdrop(bui_win *w, bui_canvas *c, bui_rect clip);

/* timer: cb dopo delay_ms, poi ogni period_ms (0 = una volta). Ritorna un id > 0 */
int bui_timer_add(int delay_ms, int period_ms, void (*cb)(void *user), void *user);
void bui_timer_del(int id);
/* animazione: step chiamata a ogni fotogramma finche' ritorna non zero */
void bui_anim_add(int (*step)(void *user, uint32_t now), void *user);
void bui_anim_del(int (*step)(void *user, uint32_t now), void *user);
/*
 * Livello delle animazioni dell'interfaccia. NONE: tutto istantaneo;
 * REDUCED: solo quelle che toccano poche righe (pressione, cursori, toast);
 * FULL: anche transizioni e pannelli. Ogni animazione e' una funzione del
 * tempo trascorso: se un fotogramma tarda, il successivo e' gia' piu' avanti.
 */
enum { BUI_ANIM_NONE, BUI_ANIM_REDUCED, BUI_ANIM_FULL };
void bui_anim_set_level(int level);
int bui_anim_level(void);
/* avanzamento 0..1024 di un'animazione partita a `start` e lunga `dur` ms */
int bui_tween(uint32_t start, int dur, uint32_t now);
/* curve (0..1024 -> 0..1024), solo interi */
int bui_ease_out(int t);	/* parte veloce, rallenta (cubica) */
int bui_ease_in_out(int t);
static inline int bui_lerp(int a, int b, int t)
{
	return a + (b - a) * t / 1024;
}
/* descrittori aggiuntivi nel poll() */
int bui_fd_add(int fd, void (*cb)(int fd, void *user), void *user);
void bui_fd_del(int fd);

/* ------------------------------------------------------------------ */
/* Scorrimento cinetico (solo interi)                                 */
/* ------------------------------------------------------------------ */

typedef struct bui_scroll {
	int pos, max;		/* posizione e limite, in pixel */
	int vel;		/* px/s durante l'inerzia */
	int frac;		/* resto in 1/1024 px */
	uint32_t last;
	int grab_pos, grab_y;
	int hy[4];
	uint32_t ht[4];
	int hn;
	uint8_t fling;
	/* --- aggiunti: elastico ai bordi e barra che svanisce --- */
	uint8_t elastic;	/* il trascinamento puo' superare i limiti (lo imposta il pannello) */
	uint8_t spring;		/* sta tornando al limite dopo il rilascio */
	uint8_t bar;		/* opacita' della barra di scorrimento (0..110) */
	int over, over0;	/* px oltre il limite (negativo: sopra l'inizio) */
	uint32_t act;		/* ultimo movimento (per far svanire la barra) */
} bui_scroll;

void bui_scroll_grab(bui_scroll *s, int y, uint32_t t);
int bui_scroll_drag(bui_scroll *s, int y, uint32_t t);	/* 1 se la posizione cambia */
int bui_scroll_release(bui_scroll *s, uint32_t t);	/* 1 se parte l'inerzia */
int bui_scroll_step(bui_scroll *s, uint32_t now);	/* 1 se la posizione cambia */
int bui_scroll_by(bui_scroll *s, int dy);

/* ------------------------------------------------------------------ */
/* Widget                                                             */
/* ------------------------------------------------------------------ */

enum {
	BUI_W_LABEL,	/* testo */
	BUI_W_BUTTON,	/* pulsante (testo e/o icona) */
	BUI_W_TILE,	/* riquadro grande: icona in alto, etichetta sotto */
	BUI_W_SLIDER,	/* cursore orizzontale */
	BUI_W_TOGGLE,	/* interruttore con etichetta */
	BUI_W_ROW,	/* riga: icona, etichetta a sinistra, valore a destra */
	BUI_W_CARD,	/* scheda di sfondo */
	BUI_W_LIST,	/* lista virtuale con scorrimento cinetico */
	BUI_W_CUSTOM,	/* disegnato dal chiamante */
	BUI_W_SEGS,	/* scelta fra poche opzioni: text = "Uno\nDue\nTre", value = indice */
};

/* flag */
#define BUI_F_HIDDEN	0x0001
#define BUI_F_DISABLED	0x0002
#define BUI_F_PRESSED	0x0004	/* gestito dal pannello */
#define BUI_F_FOCUS	0x0008	/* gestito dal pannello */
#define BUI_F_SELECTED	0x0010	/* scheda attiva, opzione scelta */
#define BUI_F_NOFOCUS	0x0020	/* escluso dalla navigazione da tastiera */
#define BUI_F_ICON_TOP	0x0040	/* pulsante: icona sopra il testo */
#define BUI_F_DIVIDER	0x0080	/* riga: linea di separazione in basso */
#define BUI_F_DIM	0x0100	/* riquadro: icona attenuata (app non ancora pronta) */

/* stili dei pulsanti */
enum {
	BUI_BS_TONAL,	/* neutro */
	BUI_BS_FILLED,	/* azione principale (accento) */
	BUI_BS_DANGER,	/* azione distruttiva */
	BUI_BS_GHOST,	/* senza fondo */
	BUI_BS_KEY,	/* tasto numerico */
	BUI_BS_KEY_OP,	/* tasto operatore */
	BUI_BS_SEG,	/* scheda di un selettore */
	BUI_BS_ROUND,	/* solo icona, tondo */
};

/* stili delle etichette */
enum {
	BUI_LS_BODY,	/* testo normale */
	BUI_LS_SMALL,	/* secondario, piccolo */
	BUI_LS_TITLE,	/* titolo */
	BUI_LS_SECTION,	/* titolo di sezione (piccolo, accento) */
	BUI_LS_WRAP,	/* testo normale secondario su piu' righe */
};

typedef struct bui_widget bui_widget;
typedef struct bui_panel bui_panel;
typedef struct bui_list bui_list;

struct bui_widget {
	uint8_t type, style, align;
	uint16_t flags;
	bui_rect r;			/* coordinate del contenuto del pannello */
	const char *text;		/* non copiato: deve restare valido */
	const char *text2;		/* valore a destra (riga) */
	const bui_icon *icon;
	const bui_font *font;		/* NULL = quello dello stile */
	bui_px color;			/* 0 = quello dello stile */
	int value, vmin, vmax;		/* cursore, interruttore (0/1) */
	int id;
	void *user;
	void (*on_click)(bui_widget *w);
	/* cursore: chiamata durante il trascinamento (final = 0) e al rilascio (1) */
	void (*on_change)(bui_widget *w, int final);
	/* BUI_W_CUSTOM: r e' in coordinate della finestra */
	void (*draw)(bui_widget *w, bui_canvas *c, bui_rect r);
	bui_list *list;
	bui_panel *panel;
};

struct bui_list {
	int count;
	int row_h;
	void (*draw_row)(bui_list *l, bui_canvas *c, int index, bui_rect r, unsigned flags);
	void (*on_select)(bui_list *l, int index);
	void *user;
	const char *empty_text;	/* mostrato al centro quando la lista e' vuota */
	int sel;		/* riga evidenziata dalla tastiera (-1 nessuna) */
	/* gestiti dal toolkit */
	bui_scroll sc;
	int pressed;
	bui_widget *w;
};

#define BUI_PANEL_MAX 64

struct bui_panel {
	bui_win *win;
	bui_rect vp;			/* area visibile, coordinate della finestra */
	int content_h;			/* > vp.h: il pannello scorre in verticale */
	/* sfondo opzionale (es. lo sfondo della home); altrimenti trasparente */
	void (*paint_bg)(bui_panel *p, bui_canvas *c, bui_rect clip);
	void *user;
	bui_widget w[BUI_PANEL_MAX];
	int n;
	bui_scroll sc;
	/* stato del tocco */
	bui_widget *hot, *focus;
	int press_x, press_y;
	uint8_t captured, mode, focus_vis, anim;
	/* --- aggiunti in coda --- */
	/* tocco tenuto fermo ~0,55 s su un widget (il tocco in corso viene annullato) */
	void (*on_long_press)(bui_panel *p, bui_widget *w);
	uint8_t glass;		/* i riquadri possono usare lo stile "vetro" (bui_glass_set) */
	uint8_t no_scrollbar;	/* niente barra di scorrimento del pannello */
	/* privato: animazione breve di un widget (rilascio, cursore, interruttore) */
	uint8_t fx_kind;
	bui_widget *fx_w;
	uint32_t fx_start;
	int fx_from, fx_val;
	int lp_timer, sb_timer;
};

void bui_panel_init(bui_panel *p, bui_win *win);
/* toglie tutti i widget (lo scorrimento torna a zero) */
void bui_panel_clear(bui_panel *p);
void bui_panel_set_viewport(bui_panel *p, bui_rect vp, int content_h);
void bui_panel_draw(bui_panel *p, bui_canvas *c, bui_rect clip);
/* ritornano 1 se l'evento e' stato usato */
int bui_panel_pointer(bui_panel *p, const bui_pointer *ev);
int bui_panel_key(bui_panel *p, const bui_key *ev);
void bui_panel_damage(bui_panel *p);
void bui_panel_focus(bui_panel *p, bui_widget *w, int visible);
/* rettangolo del widget in coordinate della finestra */
bui_rect bui_widget_rect(const bui_widget *w);
/* chiede il ridisegno del solo widget */
void bui_widget_damage(bui_widget *w);
void bui_widget_set_text(bui_widget *w, const char *text);
void bui_widget_set_value(bui_widget *w, int value);
void bui_widget_set_flag(bui_widget *w, unsigned flag, int on);

/* costruttori: aggiungono un widget al pannello e ne ritornano il puntatore */
bui_widget *bui_add(bui_panel *p, int type, bui_rect r);
bui_widget *bui_add_label(bui_panel *p, bui_rect r, const char *text, int style, int align);
bui_widget *bui_add_button(bui_panel *p, bui_rect r, const char *text, const bui_icon *icon,
			   int style, void (*on_click)(bui_widget *));
bui_widget *bui_add_tile(bui_panel *p, bui_rect r, const char *text, const bui_icon *icon,
			 void (*on_click)(bui_widget *));
bui_widget *bui_add_slider(bui_panel *p, bui_rect r, int vmin, int vmax, int value,
			   void (*on_change)(bui_widget *, int final));
bui_widget *bui_add_toggle(bui_panel *p, bui_rect r, const char *text, int on,
			   void (*on_change)(bui_widget *, int final));
bui_widget *bui_add_row(bui_panel *p, bui_rect r, const bui_icon *icon, const char *text,
			const char *value, void (*on_click)(bui_widget *));
bui_widget *bui_add_card(bui_panel *p, bui_rect r);
bui_widget *bui_add_list(bui_panel *p, bui_rect r, bui_list *l);
bui_widget *bui_add_custom(bui_panel *p, bui_rect r,
			   void (*draw)(bui_widget *, bui_canvas *, bui_rect));
/* selettore a segmenti: labels = "Uno\nDue\nTre" (non copiata), value = indice scelto;
 * on_change(w, 1) quando cambia */
bui_widget *bui_add_segs(bui_panel *p, bui_rect r, const char *labels, int value,
			 void (*on_change)(bui_widget *, int final));
/* disegna un widget in un rettangolo qualsiasi della finestra (es. un
 * riquadro trascinato) */
void bui_widget_draw(bui_widget *w, bui_canvas *c, bui_rect r);
/* immagine grande quanto la finestra da cui i riquadri "vetro" copiano il
 * fondo (NULL: tornano pieni). Non viene copiata */
void bui_glass_set(const bui_px *px, int w, int h, int stride);
/* ferma subito l'animazione breve in corso nel pannello */
void bui_panel_fx_cancel(bui_panel *p);

/* riga standard per le liste: icona, titolo, sottotitolo, testo a destra */
void bui_list_row(bui_canvas *c, bui_rect r, unsigned flags, const bui_icon *icon,
		  const char *title, const char *subtitle, const char *right);
void bui_list_set_count(bui_list *l, int count);
void bui_list_reveal(bui_list *l, int index);

/* intestazione di pagina: pulsante indietro + titolo. Occupa il pannello dato */
#define BUI_HEADER_H 56
void bui_header_build(bui_panel *p, bui_rect r, const char *title, void (*on_back)(bui_widget *));

/* ------------------------------------------------------------------ */
/* Dialogo di conferma e toast                                        */
/* ------------------------------------------------------------------ */

typedef struct bui_dialog {
	const char *title;
	const char *message;
	const char *buttons[3];		/* etichette, da sinistra; l'ultima e' la principale */
	uint8_t styles[3];		/* BUI_BS_* */
	int cancel;			/* indice restituito da Esc / tocco fuori (-1 di solito) */
	void (*done)(int button, void *user);
	void *user;
} bui_dialog;

void bui_dialog_open(const bui_dialog *d);
/* chiude il dialogo aperto come se si annullasse */
void bui_dialog_close(void);
int bui_dialog_active(void);
/* messaggio breve in basso, sparisce da solo */
void bui_toast(const char *text, int ms);
/* chiamata a ogni toast mostrato (per tenerne una cronologia) */
void bui_toast_set_hook(void (*hook)(const char *text));

#endif /* BUI_H */
