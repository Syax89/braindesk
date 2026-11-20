// SPDX-License-Identifier: MIT
/*
 * shell.h - interfacce interne di brainshell.
 *
 * Chi aggiunge una vista ha bisogno solo di: bui.h, la struttura bs_view
 * e le funzioni bs_view_*() qui sotto, piu' una riga in apps.c.
 * Vedi README.md per un esempio completo.
 */
#ifndef SHELL_H
#define SHELL_H

#include "bui.h"

#define BS_VERSION "0.3"
#define BS_BAR_H 40		/* altezza della barra di stato */
/* codice di uscita quando il backend DRM non parte: brain-shell-session
 * ripiega allora sulla sessione X (os/early/brain-ui) */
#define BS_EXIT_NODRM 75
#define BS_EXIT_RESTART 76	/* riavviami subito (cambio di lingua) */
/* chiude la shell in modo pulito chiedendo alla sessione di riavviarla */
void bs_restart(void);

/* ------------------------------------------------------------------ */
/* Viste (app integrate)                                              */
/* ------------------------------------------------------------------ */

typedef struct bs_view bs_view;
typedef struct bs_app bs_app;

#define BS_VIEW_NO_HEADER 1	/* la vista occupa tutta l'area e gestisce da se' il ritorno */

struct bs_view {
	const char *title;	/* titolo dell'intestazione; NULL = etichetta dell'app */
	unsigned flags;

	/* la vista diventa visibile: prepara lo stato (chiamata prima di layout) */
	void (*enter)(bs_view *v);
	/* la vista viene nascosta: ferma i tick, libera cio' che non serve */
	void (*leave)(bs_view *v);
	/* calcola la geometria per l'area assegnata (dopo enter e a ogni cambio di dimensione) */
	void (*layout)(bs_view *v, bui_rect area);
	/* ridisegna cio' che interseca clip; lo sfondo (BUI_C_BG) e' gia' stato riempito */
	void (*draw)(bs_view *v, bui_canvas *c, bui_rect clip);
	/* tocco: ritorna 1 se usato */
	int (*pointer)(bs_view *v, const bui_pointer *ev);
	/* tastiera: ritorna 1 se usato; Esc e Backspace non usati riportano alla home */
	int (*key)(bs_view *v, const bui_key *ev);
	/* scadenza richiesta con bs_view_tick_in() / bs_view_tick_every() */
	void (*tick)(bs_view *v, uint32_t now_ms);
	/* cambiamenti di sistema: BS_EV_* */
	void (*notify)(bs_view *v, int what);
	/* 1 se in questo momento la vista scrive testo (editor, campo, terminale):
	 * i tasti del dizionario restano allora PagSu/PagGiu'/Tab/Ins/Canc
	 * invece delle funzioni della shell (keys.c). NULL = mai */
	int (*text_input)(bs_view *v);

	/* impostati dalla shell prima di enter() */
	const bs_app *app;
	bui_win *win;		/* finestra che ospita la vista */
	bui_rect area;		/* area assegnata, coordinate della finestra */
	void *priv;		/* a disposizione della vista */
};

/* torna alla griglia delle app */
void bs_view_close(void);
/* chiama tick() una volta fra ms millisecondi (0 annulla) */
void bs_view_tick_in(int ms);
/* chiama tick() ogni ms millisecondi (0 annulla); BUI_FRAME_MS per le animazioni */
void bs_view_tick_every(int ms);
/* chiede il ridisegno di un rettangolo della finestra della vista */
void bs_view_damage(bui_rect r);
/* cambia il titolo dell'intestazione */
void bs_view_set_title(const char *title);
/* vero se la vista e' quella mostrata in questo momento */
int bs_view_is_active(const bs_view *v);
/* vista mostrata e ferma (nessuna transizione): i suoi pixel nella tela sono
 * quelli che ha disegnato, si possono spostare e inviare (bui_win_present) */
int bs_view_settled(const bs_view *v);
/* la vista attiva sta ricevendo testo (text_input) */
int bs_home_text_input(void);
/* apre un'app del registro dato il suo id; ritorna 0 se esiste */
int bs_open(const char *id);

/* ------------------------------------------------------------------ */
/* Registro delle app (apps.c)                                        */
/* ------------------------------------------------------------------ */

#define BS_APP_STUB 1		/* riquadro riservato: vista non ancora pronta */
#define BS_APP_HIDDEN 2		/* niente riquadro: si apre solo con --open ID */

struct bs_app {
	const char *id;
	const char *label;
	const bui_icon *icon;		/* 40 px, per il riquadro */
	bs_view *view;			/* vista integrata, oppure NULL */
	const char *const *argv;	/* programma esterno (se view == NULL) */
	unsigned flags;
};

extern const bs_app bs_apps[];
extern const int bs_napps;

extern bs_view bs_view_settings, bs_view_clock, bs_view_calc, bs_view_stub, bs_view_demo;
extern bs_view bs_view_notes, bs_view_music, bs_view_video;
extern bs_view bs_view_files, bs_view_gallery;
extern bs_view bs_view_term, bs_view_keys, bs_view_time, bs_view_dict, bs_view_games;

/* ------------------------------------------------------------------ */
/* Terminale (view_term.c)                                            */
/* ------------------------------------------------------------------ */

/* un programma in primo piano nel terminale ha scritto negli ultimi 30 s:
 * la sospensione automatica aspetta (power.c) */
int bs_term_keep_awake(void);
/* "none", "running", "exited" (per le prove) */
const char *bs_term_state(void);

/* ------------------------------------------------------------------ */
/* Tasti del dizionario e app recenti (keys.c)                        */
/* ------------------------------------------------------------------ */

enum { BS_KEY_KOKUGO, BS_KEY_EIWA, BS_KEY_ENGLISH, BS_KEY_MYDICT, BS_KEY_HISTORY, BS_NKEYS };
enum {
	BS_KA_STANDARD,		/* tasto normale (PagSu, PagGiu', Tab, Ins, Canc) */
	BS_KA_NONE,		/* nessuna azione */
	BS_KA_SWITCHER,		/* app recenti */
	BS_KA_QUICK,		/* impostazioni rapide */
	BS_KA_SCREENSHOT,
	BS_KA_SAVER,		/* risparmio energetico si'/no */
	BS_KA_MUSIC,		/* Musica: riproduci/pausa (o apre Musica) */
	BS_KA_NOTE,		/* Note; tenuto premuto: nuova nota */
	BS_KA_APP,		/* apre un'app del registro */
	BS_KA_COUNT
};

void bs_keys_init(void);
/* l'app e' stata aperta (elenco delle recenti) */
void bs_keys_recent(const bs_app *a);
/* azione e app (per BS_KA_APP) del tasto; app = "" se nessuna */
int bs_key_action(int key, const char **app);
void bs_key_set(int key, int action, const char *app);
void bs_keys_defaults(void);
const char *bs_key_name(int key);		/* "Kokugo" */
const char *bs_key_std_name(int key);		/* "PagSu" */
const char *bs_key_action_label(int action);	/* "App recenti" */
/* testo per Impostazioni: "App recenti", "Apri Calcolatrice" */
void bs_key_describe(int key, char *buf, int len);
void bs_switcher_close(void);
int bs_switcher_is_open(void);

/* ------------------------------------------------------------------ */
/* Sistema (sys.c)                                                    */
/* ------------------------------------------------------------------ */

enum { BS_BAT_UNKNOWN, BS_BAT_CHARGING, BS_BAT_DISCHARGING, BS_BAT_FULL, BS_BAT_NOT_CHARGING };

typedef struct bs_battery {
	int capacity;		/* 0..100, -1 sconosciuta */
	int status;		/* BS_BAT_* */
	int voltage_mv;		/* -1 sconosciuta */
	int usb;		/* 1 collegata, 0 no, -1 sconosciuto */
} bs_battery;

/* eventi distribuiti con bs_notify() a barra e vista attiva */
enum {
	BS_EV_BATTERY,		/* stato della batteria cambiato */
	BS_EV_BRIGHTNESS,	/* luminosita' (bs_brightness) cambiata */
	BS_EV_VOLUME,		/* volume (bs_volume) cambiato */
	BS_EV_MINUTE,		/* e' scattato un nuovo minuto */
	BS_EV_WINDOWS,		/* elenco o titolo delle finestre esterne cambiato */
	BS_EV_PREFS,		/* preferenze della shell cambiate */
	BS_EV_THEME,		/* tema cambiato: i colori BUI_C_* sono nuovi (segue un layout()) */
	BS_EV_SCREEN,		/* schermo acceso / attenuato / spento (bs_screen_state) */
};

void bs_sys_init(void);
void bs_notify(int ev);

const bs_battery *bs_battery_get(void);
/* livelli 0..100 noti alla shell (-1 finche' brain-ctl non risponde) */
int bs_brightness(void);
int bs_volume(void);
/* imposta tramite brain-ctl, in modo asincrono; durante un trascinamento
 * (final = 0) le chiamate sono diradate e conta solo l'ultimo valore */
void bs_set_brightness(int v, int final);
void bs_set_volume(int v, int final);
/* rilegge luminosita' e volume da brain-ctl (asincrono) */
void bs_refresh_levels(void);
/* esegue "brain-ctl <verbo>" senza bloccare; done riceve codice di uscita e stdout */
void bs_ctl(const char *verb, void (*done)(int status, const char *out, void *user), void *user);
/* avvia un programma esterno (fork + setsid + exec) */
int bs_launch(const char *const argv[], const char *label);

/* legge la prima riga di un file sotto $BRAIN_SYSROOT; 0 se riuscito */
int bs_read_line(const char *path, char *buf, int len);
void bs_sys_kernel(char *buf, int len);
int bs_sys_meminfo(long *total_kb, long *avail_kb);
int bs_sys_cpu(void);
int bs_sys_storage(long long *total_bytes, long long *free_bytes);

/* ------------------------------------------------------------------ */
/* Preferenze (conf.c): salvate in ~/.config/brainshell/config        */
/* ------------------------------------------------------------------ */

/* lingue (i18n.c): il codice e' scritto in italiano */
enum { BS_LANG_IT, BS_LANG_EN, BS_LANG_JA, BS_NLANGS };
extern int bs_lang;
/* la traduzione di una frase italiana intera (se c'e'), altrimenti la frase */
const char *bs_tr(const char *s);
#define T(s) bs_tr(s)
void bs_i18n_init(int lang);
const char *bs_lang_code(int lang);
int bs_lang_find(const char *code);
const char *bs_month_name(int m, int abbrev);	/* 0..11 */
const char *bs_weekday_initial(int i);		/* 0 = lunedi' */

typedef struct bs_prefs {
	int theme;		/* indice di bui_theme_get() */
	int anim;		/* BUI_ANIM_FULL / _REDUCED / _NONE */
	int tile_style;		/* -1: quello del tema; altrimenti BUI_TILE_* */
	int dim_s, off_s;	/* secondi di inattivita' prima di attenuare / spegnere; 0 = mai */
	int sleep_s;		/* secondi a schermo spento (o di inattivita' se off_s = 0) prima di
				 * sospendere, solo a batteria; 0 = mai */
	int saver;		/* risparmio energetico: animazioni ridotte, luminosita' <= 40%,
				 * schermo spento dopo 30 s, sospensione dopo 1 min */
	char order[256];	/* id delle app nell'ordine della griglia, separati da virgole */
	char hidden[256];	/* id delle app nascoste */
	int lang;		/* BS_LANG_* */
	char keys[5][24];	/* azioni dei tasti del dizionario: "switcher", "app:calc"... (keys.c) */
} bs_prefs;

extern bs_prefs bs_pref;
extern int bs_pref_battery_percent;

void bs_conf_load(void);
/* scrive il file fra poco (le modifiche ravvicinate diventano una scrittura sola),
 * in modo atomico: file temporaneo + rename */
void bs_conf_save(void);
void bs_conf_flush(void);
/* applica tema, stile dei riquadri e livello delle animazioni al toolkit */
void bs_prefs_apply(void);
/* tempi effettivi (il risparmio energetico li accorcia), in secondi */
int bs_eff_dim(void);
int bs_eff_off(void);
int bs_eff_sleep(void);
#define BS_SAVER_BRIGHTNESS 40

/* ------------------------------------------------------------------ */
/* Schermo e inattivita' (sys.c, idle.c)                              */
/* ------------------------------------------------------------------ */

enum { BS_SCREEN_ON, BS_SCREEN_DIM, BS_SCREEN_OFF };

int bs_screen_state(void);
/* cambia lo stato dello schermo tramite brain-ctl (luminosita' / "screen off|on") */
void bs_screen_set(int state);
void bs_idle_init(void);
/* rilegge bs_pref.dim_s / off_s */
void bs_idle_configure(void);
void bs_idle_shutdown(void);

/* testo in italiano per data e stato della batteria.
 * stile: 0 "Giovedi' 19 novembre", 1 con l'anno, BS_DATE_SHORT "Gio 19 novembre" */
#define BS_DATE_SHORT 2
void bs_fmt_date(char *buf, int len, int style);
const char *bs_battery_status_text(const bs_battery *b);

/* ------------------------------------------------------------------ */
/* Gestore di finestre (wm.c)                                         */
/* ------------------------------------------------------------------ */

#define BS_MAX_CLIENTS 16

typedef struct bs_client {
	unsigned long id;
	char title[64];
} bs_client;

void bs_wm_init(void);
void bs_wm_own(bui_win *w);			/* finestra nostra: mai tra le app */
void bs_wm_set_home(bui_win *home, bui_win *bar);
void bs_wm_refresh(void);
int bs_wm_clients(const bs_client **list);
unsigned long bs_wm_active(void);
void bs_wm_activate(unsigned long id);
void bs_wm_close(unsigned long id);
/* porta in primo piano la home (nasconde le finestre esterne) */
void bs_wm_show_home(void);
void bs_wm_focus_home(void);

/*
 * Le funzioni bs_wm_* e bs_idle_* qui sopra scelgono l'implementazione
 * secondo il backend (wmsel.c): con X11 wm.c (EWMH) e idle.c (SYNC); con
 * drm/mem xapp.c (app X esterne su richiesta) e idle_ev.c (tempi di evdev).
 */
void bs_wmx_init(void);
void bs_wmx_own(bui_win *w);
void bs_wmx_set_home(bui_win *home, bui_win *bar);
void bs_wmx_refresh(void);
int bs_wmx_clients(const bs_client **list);
unsigned long bs_wmx_active(void);
void bs_wmx_activate(unsigned long id);
void bs_wmx_close(unsigned long id);
void bs_wmx_show_home(void);
void bs_wmx_focus_home(void);
void bs_idlex_init(void);
void bs_idlex_configure(void);
void bs_idlex_shutdown(void);
void bs_idlev_init(void);
void bs_idlev_configure(void);
void bs_idlev_shutdown(void);
void bs_idlev_screen_off(void);	/* schermo spento adesso (tasto di accensione col cavo) */
void bs_idlev_wake(void);	/* schermo acceso, inattivita' azzerata */
void bs_idlev_resumed(void);	/* dopo una sospensione */

/* ------------------------------------------------------------------ */
/* Energia (power.c)                                                  */
/* ------------------------------------------------------------------ */

void bs_power_init(void);
void bs_power_shutdown(void);
/* sospende (a batteria) o spegne solo lo schermo (col cavo USB);
 * explicit_request: scelta dal menu (avvisa se si spegne solo lo schermo) */
int bs_power_sleep(int explicit_request);
int bs_power_sleeping(void);	/* brain-ctl suspend in corso */
int bs_power_can_autosleep(void);
void bs_power_menu(void);
/* risparmio energetico acceso/spento (preferenza salvata) */
void bs_power_saver_set(int on);	/* Spegni / Riavvia / Sospendi / Annulla a tutto schermo */
int bs_power_menu_open(void);
/* ogni lettura della batteria (sys.c): stima e avvisi di batteria scarica */
void bs_power_battery(const bs_battery *old, const bs_battery *n);
int bs_power_minutes(void);	/* autonomia stimata, -1 se non si sa */
/* "87% · 3 h 20 min" oppure "87% · In carica" */
void bs_battery_text(char *buf, int len);
/* rilegge subito la batteria (dopo un risveglio) */
void bs_battery_refresh(void);
/* "brain-ctl VERBO [ARG]" senza bloccare */
void bs_ctl_arg(const char *verb, const char *arg, void (*done)(int, const char *, void *), void *user);
/* ms al termine del timer dell'Orologio in corso, -1 se nessuno */
int bs_clock_timer_ms(void);

/* ------------------------------------------------------------------ */
/* App X esterne su richiesta (xapp.c, solo drm/mem)                  */
/* ------------------------------------------------------------------ */

/*
 * Senza X la shell possiede tutto lo schermo. Un'app esterna (Terminale,
 * il lettore video...) fa partire un server X solo per lei: la shell cede
 * il master DRM e l'ingresso, esegue "$BRAINSHELL_XORG ARGV..." (default
 * "sudo -n /usr/local/sbin/brain-xapp") e, quando quel processo finisce
 * (l'app e' uscita e X con lei), riprende schermo e ingresso e ridisegna.
 * Ctrl+Alt+Esc durante l'app chiede di chiuderla.
 */
enum { BS_XAPP_IDLE, BS_XAPP_STARTING, BS_XAPP_RUNNING, BS_XAPP_RETURNING };
int bs_xapp_launch(const char *const argv[], const char *label);
int bs_xapp_state(void);
void bs_xapp_stop(void);		/* chiede la chiusura dell'app in corso */
void bs_xapp_shutdown(void);		/* all'uscita della shell */

/* sorveglia un figlio: cb riceve lo stato di waitpid() (sys.c) */
int bs_child_watch(int pid, void (*cb)(int status, void *user), void *user);
/* nel figlio dopo fork(): segnali di default, stdin da /dev/null, stdout su
 * out_fd (o /dev/null se < 0, salvo BRAINSHELL_CHILD_OUTPUT) */
void bs_child_setup(int out_fd);
/* id dell'app aperta nella home ("grid" se nessuna) */
const char *bs_home_view_id(void);
/* l'ora di sistema e' cambiata: la barra ridisegna subito e riallinea il minuto */
void bs_bar_clock_changed(void);
/* fuso orario nuovo per la shell e i suoi figli (TZ + tzset) */
void bs_set_timezone(const char *zone);

/* ------------------------------------------------------------------ */
/* Parti della shell                                                  */
/* ------------------------------------------------------------------ */

bui_win *bs_bar_init(void);
void bs_bar_notify(int ev);
/* impostazioni rapide: apre, o chiude se gia' aperte */
void bs_bar_quick_toggle(void);
int bs_bar_quick_open(void);	/* impostazioni rapide o calendario aperti */
void bs_bar_screen_changed(void);
void bs_bar_close_popups(void);	/* impostazioni rapide e calendario, senza animazione */

bui_win *bs_home_init(void);
void bs_home_notify(int ev);
void bs_home_screen_changed(void);
void bs_home_go_grid(void);
void bs_home_shutdown(void);

/* "In riproduzione" nella striscia della home: la vista Musica dice cosa
 * suona; title NULL o state BS_NP_NONE la toglie. Il testo viene copiato.
 * Un tocco sul riquadro apre l'app "music". */
enum { BS_NP_NONE, BS_NP_PLAYING, BS_NP_PAUSED };
void bs_home_set_now_playing(const char *title, int state);

/* cronologia degli ultimi toast (per il popup del calendario) */
#define BS_NOTES_MAX 4
int bs_notes_get(int i, const char **text, const char **when);

/* Note: apre la vista con una nota nuova (tasto My Dictionary tenuto) */
void bs_notes_new(void);

/* azioni comuni con conferma */
void bs_ask_rotate(void);
void bs_ask_power(void);
void bs_screenshot(void);

#endif
