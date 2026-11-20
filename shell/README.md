# brainshell — l'interfaccia touch di BrainDesk

Un solo programma C (`brainshell`, ~600 kB, solo libc) che fa da interfaccia
touch dello SHARP Brain PW-SH5. Dalla 0.3 disegna **direttamente sul pannello
(DRM) e legge tocco e tasti da evdev, senza server X**: X parte solo quando
serve, per una singola app esterna (un video, il terminale st di ripiego), e si chiude con lei
(vedi *Senza X*). La variante `brainshell-x11` (stessi sorgenti, + libX11 e
libXext) gira sotto X come prima, accanto a JWM: serve allo sviluppo, alle
prove sotto Xvfb e alla sessione X di ripiego.

- **barra di stato** in alto (finestra *dock*, 40 px): Home, linguette delle
  finestre esterne, volume, batteria, orologio, impostazioni rapide;
- **home** (finestra *desktop*): sfondo del tema, striscia con ora, data e
  indicatori, griglia delle app riordinabile e filtrabile da tastiera;
- **app integrate** come *viste* dentro la home: Impostazioni, Orologio,
  Calcolatrice, Esplora (gestore di file), Galleria, Musica, Video, Note,
  **Terminale** (emulatore integrato su un PTY, senza X: vedi *Terminale*);
- **tasti del dizionario** (Kokugo, Eiwa, Brain English, My Dictionary,
  Rireki) con funzioni della shell configurabili e un selettore delle app
  recenti (vedi *Tasti del dizionario*);
- **sei temi**, animazioni brevi e misurate, schermo che si attenua e si
  spegne da solo; preferenze salvate in `~/.config/brainshell/config`;
- **bui**, il piccolo toolkit su cui tutto è costruito.

Sotto X (`brainshell-x11`) JWM resta in esecuzione per le app esterne (st,
xfe, ...) e la shell ci parla con EWMH. Le schermate di ogni pagina, in orizzontale e in verticale,
sono in `shots/`: 01–20 la shell, 21–33 Esplora e Galleria, 34–45 Note,
Musica e Video, 51–56 i sei temi, 60–65 calendario, modo modifica, ricerca,
una transizione a metà, "in riproduzione", sfondo dell'utente.

Vincoli che hanno guidato ogni scelta: ARM926 a 454 MHz **senza FPU** (niente
float nel codice che gira sul Brain, salvo i conti della calcolatrice), 112 MB
di RAM, nessuna GPU, pannello aggiornato a righe (ridisegnare poco costa
quasi nulla, ridisegnare tutto costa ~17 ms di bus).

## Compilare

Servono due immagini Docker: `brain-userland` (già nel progetto,
`docker/userland/Dockerfile`: compilatore incrociato armel) e
`brain-shelltest` (`test/Dockerfile`: gcc nativo, Xvfb, JWM, xdotool,
ImageMagick, qemu-user). `test/run.sh` costruisce la seconda da solo.

```sh
cd shell
# binari per il Brain (ARMv5TE soft-float, senza simboli) -> build/armel/
#   brainshell      backend drm (+ mem): NEEDED solo libc.so.6
#   brainshell-x11  anche il backend x11: NEEDED libX11, libXext, libc
docker run --rm -v "$PWD":/src brain-userland make -C /src armel
cp build/armel/brainshell build/armel/brainshell-x11 ../os/

# gli stessi due binari per il PC (prove) e con AddressSanitizer + UBSan
docker run --rm -v "$PWD":/src brain-shelltest make native debug
# prove unitarie: traduzione evdev, emulatore del terminale (nessun X, nessun dispositivo)
docker run --rm -v "$PWD":/src brain-shelltest sh -c 'make tests && build/debug/evdev_test && build/debug/vt_test'

# rigenerare font e icone (solo se cambi tools/bake.c): scrive gen/*.c
docker run --rm -v "$PWD":/src brain-shelltest make bake
```

`gen/bui_fonts.c`, `gen/bui_icons.c` e `gen/bui_assets.h` sono generati ma
fanno parte dei sorgenti: la compilazione incrociata non ha bisogno di rete
né di font installati. Tutto compila senza avvisi con
`-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`.

## Mettere in funzione sul Brain

Senza X (default dalla 0.3): vedi *Senza X* → *Installazione*. La sessione X
classica, che resta come ripiego, richiede invece:

1. `os/brainshell` e `os/brainshell-x11` in `/usr/local/bin/` (sotto X
   `brainshell` passa da solo a `brainshell-x11`, quindi `~/.xinitrc` non
   cambia);
2. `brain-ctl` (vedi *Contratto col sistema*);
3. in `~/.xinitrc`, prima di `exec jwm`: `brainshell &`
   (l'ordine non conta: la shell si adatta se JWM parte dopo o viene riavviato);
4. in `~/.jwmrc`: togliere i due `<Tray>` (la barra è la shell) e
   `<Swallow>brainbar`; il gruppo `<Option>maximized</Option>` può restare, le
   finestre esterne si massimizzano nell'area sotto la barra (la shell
   pubblica `_NET_WM_STRUT`). `test/jwmrc` è un esempio minimo funzionante.

Opzioni: `brainshell --open ID` apre subito una vista (`settings`, `clock`,
`calc`, `demo`...), `--version`. Il backend si sceglie con
`BRAINSHELL_BACKEND=drm|x11|mem`; senza la variabile: `drm` se `$DISPLAY` non
c'è e `/dev/dri/card0` è accessibile, altrimenti `x11`.

## Architettura

```
main.c           avvio, distribuzione degli eventi di sistema (bs_notify)
apps.c           REGISTRO delle app: l'unico file da toccare per aggiungere una vista
home.c           finestra desktop: sfondo, striscia delle informazioni, griglia (modifica, ricerca),
                 ospite delle viste (intestazione + area) e transizione griglia <-> vista
bar.c            finestra dock: barra di stato, impostazioni rapide, calendario, conferme
conf.c           preferenze: lettura e scrittura atomica di ~/.config/brainshell/config
idle.c           [x11] schermo attenuato/spento per inattivita' (contatore IDLETIME di SYNC)
wm.c             [x11] EWMH: elenco finestre, attiva/chiudi, "mostra la home", fuoco della tastiera
wmsel.c          sceglie wm.c/idle.c (x11) oppure xapp.c/idle_ev.c (drm, mem)
xapp.c           [drm] app X esterne su richiesta: cede schermo e ingresso, X per una sola app, ritorno
idle_ev.c        [drm] inattivita' dai tempi degli eventi evdev (anche durante un'app X)
sys.c            batteria (sysfs), brain-ctl asincrono, avvio programmi, info di sistema
view_*.c         le viste: settings, clock, calc, stub (segnaposto "in arrivo", non piu' usato
                 nella griglia: resta come esempio minimo), demo (campionario),
                 notes (+ view_notes_edit.c), music (+ _player.c, _tags.c), video,
                 term (Terminale), keys (Impostazioni -> Tasti)
vt.c             emulatore VT100/xterm del Terminale: solo stato e conti, provato da solo (test/vt_test.c)
keys.c           tasti del dizionario come tasti della shell, azioni, selettore delle app recenti
media.h          interfacce comuni a Note, Musica e Video
media_scan.c     elenco dei file di una cartella letto da un processo figlio
media_icons.c    icone in piu' per le tre viste (generate, fuori da gen/)
view_files.c     Esplora: elenco, scorciatoie, azioni, proprieta', rinomina
view_files_text.c   lettore di testi di Esplora
view_gallery.c   Galleria: griglia di miniature
view_gallery_viewer.c   visualizzatore a tutto schermo (usato da Galleria ed Esplora)
bs_img.c         decodifica JPEG/PNG/BMP/GIF, riduzione, RGB565 con dithering, cache delle miniature
bs_work.c        filo di lavoro unico per cio' che e' lento (cartelle, immagini, testi)
third_party/     stb_image.h e TJpgDec (incorporati, compilati dentro bs_img.c)
shell.h          interfacce interne (vista, registro, servizi)

bui.h            API del toolkit
bui.c            tela RGB565: riempimenti, sfumature con dithering, rettangoli arrotondati,
                 maschere A8, blit, ombre, testo, icone
bui_core.c       scelta del backend, ciclo eventi (poll), timer, animazioni, descrittori, filtri d'ingresso
bui_be.h         interfaccia (privata) fra il nucleo e i backend
bui_x.c          backend x11: finestre X11, MIT-SHM, popup, traduzione degli eventi X
bui_comp.c       compositore software dei backend drm e mem: finestre come livelli di una superficie
bui_drm.c        backend drm: ioctl DRM grezzi (niente libdrm), dumb buffer, DIRTYFB a fasce; dispositivi evdev
bui_evdev.c      traduzione evdev: calibrazione/rotazione/filtro del tocco, mappa US, accentate, ripetizione
bui_mem.c        backend mem: compositore in memoria guidato da uno script, PNG (prove senza X)
bui_w.c          widget, pannelli, scorrimento cinetico, liste, dialogo di conferma, toast
bui_theme.c      i temi incorporati, livello e curve delle animazioni
bui_x.h          accesso a Xlib per chi parla col gestore di finestre (solo wm.c, idle.c)
gen/             font e icone generati (maschere A8 in tabelle C)
tools/bake.c     generatore lato PC (stb_truetype): DejaVu Sans + Material Icons Round
test/            Dockerfile, finto brain-ctl, scene xdotool (x11), scene mem/*.sh, evdev_test.c,
                 finto Xorg (fake-xorg)
```

### Disegno

- Ogni finestra ha una **tela RGB565 che è l'immagine XShm stessa** (nessuna
  copia intermedia). Lo stato dello schermo vive nella tela: un `Expose`
  rimanda soltanto i pixel, non ridisegna.
- `bui_win_damage(win, rect)` annota un rettangolo sporco (fino a 8 per
  finestra, fusi quando l'unione non spreca area). Al giro successivo del
  ciclo, per ogni rettangolo la finestra riceve `paint(win, canvas, clip)` e
  deve ridisegnare *tutto ciò che interseca* `clip`, sfondo compreso; poi parte
  un `XShmPutImage` di quel solo rettangolo.
- Fra due invii della stessa finestra passano almeno ~30 ms e non si disegna
  finché il server non ha confermato l'invio precedente (`ShmCompletion`):
  durante trascinamenti e inerzia si va al passo che il server regge, senza
  code. Il primo ridisegno dopo una pausa (il "premuto" di un tocco) parte
  subito.
- A riposo il processo dorme in `poll()`: lo svegliano solo lo scatto del
  minuto e la lettura della batteria ogni 30 s.
- Visual non RGB565 (PC a 24 bit) e display senza SHM sono gestiti con una
  conversione all'invio: comodo per sviluppare, mai usato sul Brain (a 24 bit
  i popup non catturano il fondo: compaiono come rettangoli pieni).
- `BUI_TRACE=1` stampa su stderr ogni rettangolo inviato: è il modo per
  verificare che una modifica non ridisegni più del necessario.

### Tocco e tastiera

Il pannello (`bui_panel`) distingue tocco e trascinamento:

- `BUI_PRESS`: il widget sotto il dito diventa "premuto" e viene ridisegnato
  subito (solo lui);
- movimento oltre `BUI_DRAG_SLOP` (12 px, per il rumore del resistivo): se il
  pannello o la lista scorrono e il moto è verticale diventa scorrimento
  (il "premuto" si annulla); su un cursore diventa regolazione; altrimenti il
  tocco resta valido finché il dito è sul widget (± 12 px);
- al rilascio durante uno scorrimento parte l'inerzia (interi, attrito
  esponenziale); un tocco durante l'inerzia la ferma e non attiva nulla.

Tastiera fisica: frecce = fuoco (navigazione spaziale, anello d'accento),
Invio/Spazio = attiva, Maiusc+Tab = precedente, Esc/Backspace = indietro (se
la vista non li usa), sinistra/destra su un cursore = regola. Tab, Pag↑/Pag↓,
Ins e Canc sul Brain sono i tasti del dizionario: fuori dai testi e dai popup
fanno le azioni della shell (vedi *Tasti del dizionario*); con "Tasto
normale" tornano Tab = successivo, Pag↑/Pag↓ = scorre, Canc = elimina.

| tasto del Brain | codice | effetto |
|---|---|---|
| **Home** | `KEY_HOMEPAGE` (X: `XF86HomePage`) | alla griglia da ovunque: chiude la vista (con il suo `leave`: la nota si salva), pannello rapido, calendario, dialoghi e menu di spegnimento, toglie la ricerca. Schermo spento o attenuato: lo riaccende soltanto. Durante un'app X (backend drm): la chiude e torna alla shell, come Ctrl+Alt+Esc |
| Go Back | `KEY_ESC` | indietro (Esc); nel Terminale e' Esc per il programma |
| accensione | `KEY_POWER` | vedi *Energia* |
| Page↑ | `KEY_LEFTCTRL` | Ctrl |
| Onsei (音声) | `KEY_LEFTALT` | Alt: accentate (vedi *Ingresso*), luminosita' e volume coi tasti del dizionario |
| Kokugo, Eiwa, Brain English, My Dictionary, Rireki | `KEY_PAGEUP`, `KEY_PAGEDOWN`, `KEY_TAB`, `KEY_INSERT`, `KEY_DELETE` | vedi *Tasti del dizionario* |

Fino al device tree precedente anche Home mandava `KEY_ESC`.
Nella griglia una lettera porta al riquadro che inizia così; nella
calcolatrice cifre e operatori scrivono; nell'orologio `1 2 3` cambiano
scheda e Spazio avvia/ferma.

### Popup

Impostazioni rapide e dialoghi sono finestre *override-redirect* con grab di
puntatore e tastiera. All'apertura catturano lo schermo sotto di sé
(`XShmGetImage`): così angoli arrotondati, ombra e fondo scurito (con
dithering) si fondono con qualunque cosa ci sia sotto, senza compositing. Un
dialogo aperto dal pannello rapido gli resta sopra: "Annulla" riporta al
pannello. Il toast non è modale e può restare mentre sotto lo schermo cambia:
è sagomato con l'estensione Shape invece di catturare il fondo.

### Collaborazione con JWM (EWMH)

- barra: `_NET_WM_WINDOW_TYPE_DOCK`, `_NET_WM_STRUT(_PARTIAL)` di 40 px in alto;
  home: `_NET_WM_WINDOW_TYPE_DESKTOP`; entrambe `SKIP_TASKBAR`/`STICKY`.
- linguette: `_NET_CLIENT_LIST` + `_NET_WM_NAME` (o `WM_NAME`), aggiornate dai
  `PropertyNotify`; escluse le finestre dock/desktop/menu/splash e quelle con
  `SKIP_TASKBAR`. Tocco = `_NET_ACTIVE_WINDOW`, × = `_NET_CLOSE_WINDOW`. Se non
  entrano tutte compare `+N`; quando sono strette la × resta solo sull'attiva.
- Home: `_NET_SHOWING_DESKTOP = 1` alla radice (solo se non è già 1: JWM lo
  tratta come un interruttore e non lo azzera quando si apre una finestra);
  150 ms dopo, ciò che è ancora visibile viene ridotto a icona con
  `XIconifyWindow`. Senza gestore di finestre le nostre finestre vengono
  semplicemente rialzate.
- Fuoco: quando nessuna finestra esterna è attiva i tasti devono arrivare
  alla home, ma JWM lascia il fuoco sulla radice quando chiude o riduce a
  icona la finestra attiva: 200 ms dopo ogni cambio di `_NET_ACTIVE_WINDOW`
  la shell controlla e, se serve, lo riprende.
- La home chiede tutto lo schermo ma calcola l'area utile dalla posizione
  reale: funziona sia se resta a y=0 sotto la barra sia se il gestore la
  sposta nell'area di lavoro. Se lo schermo cambia dimensione (RandR) barra e
  home vengono ritirate, ridimensionate e rimappate.

## Aggiungere una vista

Una vista è una `bs_view` (in `shell.h`): una struttura di callback, tutte
facoltative. La shell disegna intestazione e pulsante "indietro" e assegna
alla vista il rettangolo sottostante.

| callback | quando | cosa fare |
|---|---|---|
| `enter(v)` | la vista sta per apparire | inizializzare lo stato (`v->win` è già valido) |
| `layout(v, area)` | dopo `enter` e a ogni cambio di dimensione | calcolare la geometria per `area` (coordinate della finestra) |
| `draw(v, c, clip)` | serve ridisegnare | disegnare ciò che interseca `clip`; lo sfondo `BUI_C_BG` è già steso e il clip della tela è già impostato |
| `pointer(v, ev)` | tocco (`BUI_PRESS/MOVE/RELEASE/WHEEL/CANCEL`) | di solito `return bui_panel_pointer(&panel, ev)` |
| `key(v, k)` | tasto | ritornare 1 se usato; Esc/Backspace non usati tornano alla griglia |
| `tick(v, now_ms)` | scadenza chiesta con `bs_view_tick_in/every` | aggiornare e chiedere il ridisegno del minimo |
| `notify(v, what)` | `BS_EV_BATTERY`, `_BRIGHTNESS`, `_VOLUME`, `_MINUTE`, `_WINDOWS`, `_PREFS`, `_THEME` (seguito da `layout()`), `_SCREEN` | aggiornare ciò che dipende dal sistema |
| `text_input(v)` | a ogni tasto del dizionario | ritornare 1 se in quel momento la vista scrive testo (editor, campo, terminale): i tasti restano PagSu/PagGiù/Tab/Ins/Canc (vedi *Tasti del dizionario*) |
| `leave(v)` | la vista viene nascosta | `bui_panel_clear()`, liberare; i tick sono già fermati dalla shell |

Servizi per le viste: `bs_view_close()`, `bs_view_damage(rect)`,
`bs_view_tick_in(ms)` (una volta), `bs_view_tick_every(ms)` (periodico;
`BUI_FRAME_MS` per un'animazione), `bs_view_set_title()`,
`bs_view_is_active(v)`, `bs_open(id)`; e da bui: `bui_toast()`,
`bui_dialog_open()`, `bui_timer_add()` (sopravvive alla chiusura della vista:
così il timer dell'Orologio suona anche da un'altra pagina).

### Esempio minimo

`view_ciao.c`: un contatore con un pulsante e un'etichetta.

```c
#include <stdio.h>
#include "shell.h"

static bui_panel panel;
static bui_widget *label;
static char text[32];
static int count;

static void on_plus(bui_widget *w)
{
	(void)w;
	snprintf(text, sizeof(text), "Tocchi: %d", ++count);
	bui_widget_damage(label);	/* ridisegna solo l'etichetta */
}

static void enter(bs_view *v)
{
	bui_panel_init(&panel, v->win);
	snprintf(text, sizeof(text), "Tocchi: %d", count);
}

static void layout(bs_view *v, bui_rect a)
{
	(void)v;
	bui_panel_clear(&panel);
	bui_panel_set_viewport(&panel, a, a.h);	/* contenuto alto quanto l'area: non scorre */
	/* i rettangoli dei widget sono relativi all'angolo dell'area */
	label = bui_add_label(&panel, bui_r(24, 24, a.w - 48, 32), text, BUI_LS_TITLE, BUI_AL_LEFT);
	bui_add_button(&panel, bui_r(24, 72, 200, BUI_TOUCH), "Aggiungi", BUI_IC(add, 24),
		       BUI_BS_FILLED, on_plus);
}

static void leave(bs_view *v) { (void)v; bui_panel_clear(&panel); }
static void draw(bs_view *v, bui_canvas *c, bui_rect clip) { (void)v; bui_panel_draw(&panel, c, clip); }
static int pointer(bs_view *v, const bui_pointer *e) { (void)v; return bui_panel_pointer(&panel, e); }
static int key(bs_view *v, const bui_key *k) { (void)v; return bui_panel_key(&panel, k); }

bs_view bs_view_ciao = {
	.enter = enter, .leave = leave, .layout = layout,
	.draw = draw, .pointer = pointer, .key = key,
};
```

Per registrarla: `extern bs_view bs_view_ciao;` in `shell.h`, il file in
`SRC` nel `Makefile`, e una riga in `apps.c`:

```c
{ "ciao", "Ciao", BUI_IC(apps, 40), &bs_view_ciao, NULL, 0 },
```

Non restano riquadri segnaposto: `files`, `gallery`, `music`, `video` e
`notes` sono viste vere, e il riquadro esterno "File" (`xfe`) è stato tolto
ora che c'è Esplora. Un riquadro può ancora essere riservato con
`&bs_view_stub` e `BS_APP_STUB`.

Esempi più completi: `view_stub.c` (solo `draw`), `view_demo.c` (pannello
scorrevole, lista, widget personalizzato), `view_clock.c` (tick, ridisegni
parziali, stato che sopravvive alla chiusura), `view_calc.c` (tastiera).

### Regole per chi scrive una vista

- **Niente float** nel disegno e nei cicli; interi e virgola fissa.
- **Mai bloccare**: niente `system()`, niente letture lente. Per un processo
  esterno `bs_ctl()` / `bs_launch()`; per un descrittore `bui_fd_add()`.
- **Ridisegnare il minimo**: `bui_widget_damage(w)` o `bs_view_damage(rect)`
  sulla parte che cambia; controllare con `BUI_TRACE=1`.
- `draw()` può essere chiamata con qualunque `clip`: deve produrre gli stessi
  pixel ogni volta (niente stato che avanza dentro `draw`).
- I testi passati ai widget **non vengono copiati**: devono restare validi
  (buffer statici della vista).
- Bersagli per il dito ≥ `BUI_TOUCH` (56 px); spaziature a multipli di 8.
- **Colori solo da `BUI_C_*` / `bui_th`, letti in `draw()` o `layout()`**: mai
  in inizializzatori statici, mai esadecimali fissi (vedi *Temi*).
- Al massimo `BUI_PANEL_MAX` (64) widget per pannello; per elenchi lunghi
  usare `bui_list` (virtuale: disegna solo le righe visibili).
- Un `on_click` può ricostruire il pannello (`bui_panel_clear` + nuovi
  widget): dopo la chiamata il toolkit non tocca più il widget.

## Temi

Un tema (`bui_theme`, in `bui.h`; la tabella è in `bui_theme.c`) è una
struttura con: i 17 colori dell'interfaccia, i raggi di riquadri, schede e
pulsanti, lo stile dei riquadri della home, le tinte delle icone delle app,
l'eventuale ombra sotto i riquadri e i parametri dello sfondo procedurale.
Sei temi incorporati:

| id | nome | carattere |
|---|---|---|
| `teal` | Teal | quello originale: scuro blu-grigio, accento verde acqua |
| `brace` | Brace | scuro caldo, accento ambra, angoli più morbidi, grana leggera |
| `nero` | Nero | nero pieno ad alto contrasto, accento ciano, riquadri a contorno, angoli stretti |
| `chiaro` | Chiaro | chiaro, accento blu, icone a più colori, ombre sotto i riquadri |
| `nord` | Nord | freddo e desaturato, icone a più colori |
| `aurora` | Aurora | sfumatura viola-blu vivace, riquadri di vetro, icone a più colori |

Si scelgono in **Impostazioni → Aspetto** (una striscia di anteprime): il
cambio è immediato e viene salvato. Nello stesso riquadro: il livello delle
animazioni e lo stile dei riquadri (del tema / pieni / vetro / contorno).

Come funziona:

- `BUI_C_BG`, `BUI_C_ACCENT`, ... non sono più costanti: sono campi di
  `bui_th`, il tema corrente, letti al momento del disegno. Il codice delle
  viste non cambia, con due sole regole: **niente `BUI_C_*` negli
  inizializzatori statici** e niente colori conservati oltre un cambio di
  tema. Dopo `bui_theme_set()` la shell manda `BS_EV_THEME` e richiama
  `layout()` della vista attiva: i colori copiati nei widget (`w->color`) in
  `layout()` si rinnovano da soli.
- `bui_th.r_tile / r_card / r_btn` sono i raggi; `bui_th.light` dice se il
  tema è chiaro (per chi disegna sopra immagini).
- **Sfondo**: sfumatura diagonale `wall0 → wall1` più fino a tre aloni
  (`blob[]`: centro e raggio in percentuale, luce aggiunta) e grana
  (`grain`: rumore di ±N livelli prima del dithering ordinato). È calcolato
  una volta su una griglia di nodi ogni 32 px e interpolato con sole somme:
  NNN M di istruzioni per 854×480, come prima; la grana ne aggiunge NNN. Non
  c'è cache su disco: rileggere 820 kB dalla microSD costerebbe più che
  ricalcolarli.
- **Sfondo dell'utente**: se esiste `~/.config/brainshell/wallpaper.rgb565`
  (pixel RGB565 grezzi, little-endian, esattamente larghezza×altezza dello
  schermo nell'orientamento in uso, quindi 819 840 byte) viene usato al posto
  di quello procedurale, avvicinato di un terzo al colore di fondo del tema
  perché il testo resti leggibile. Per crearlo da un'immagine:
  `convert foto.jpg -resize 854x480^ -gravity center -extent 854x480 -flip -define bmp:subtype=RGB565 bmp3:w.bmp && tail -c 819840 w.bmp > wallpaper.rgb565`.
- **Stili dei riquadri** (`bui_th.tile_style`): `BUI_TILE_FILLED` (pieni),
  `BUI_TILE_OUTLINE` (solo il bordo), `BUI_TILE_GLASS` (vetro). Il vetro non
  miscela nulla mentre si scorre: all'avvio la home prepara una seconda
  immagine grande quanto lo schermo — lo stesso sfondo, sfocato (se è una
  foto) e portato verso il colore dei controlli — e ogni riquadro ne copia
  il pezzo che ha sotto (`bui_blit_rr`, una `memcpy` per riga più gli angoli).
  Costa 820 kB di RAM e un secondo passaggio di calcolo all'avvio, solo
  quando lo stile è attivo.
- **Icone tinte**: se il tema ha `tint[]`, ogni app prende una delle sei
  tinte (per posizione nel registro); altrimenti tutte l'accento.
- **Ombre**: `bui_shadow()` usa una maschera A8 (angolo + profilo dei lati)
  calcolata una volta per ogni coppia raggio/ampiezza; disegnarla costa solo
  la miscela dell'anello esterno. La usano dialoghi, pannelli, il riquadro
  trascinato e, nei temi con `tile_shadow`, i riquadri della home.

Per aggiungere un tema: una voce nella tabella `themes[]` di `bui_theme.c`.

## Animazioni

Regola del progetto: il pannello riceve solo le **righe** cambiate (17 ms di
bus per uno schermo intero, 0,5 ms per 24 righe), quindi un'animazione è
ammessa solo se tocca poche righe, oppure se è breve e non ridisegna due
volte gli stessi pixel. Tutte sono funzioni del tempo trascorso
(`bui_tween()` + `bui_ease_out()`, solo interi): se un fotogramma tarda, il
successivo è già più avanti e la durata non cambia. Tutte si interrompono al
primo tocco o tasto. A riposo non resta nessun timer.

**Impostazioni → Aspetto → Animazioni**: *Complete* (`full`), *Ridotte*
(`reduced`: solo quelle piccole, segnate ● nella tabella), *Nessuna*
(`none`: tutto istantaneo come nella versione 0.1, barre di scorrimento
sempre visibili).

| animazione | durata | area per fotogramma | note |
|---|---|---|---|
| apertura di una vista | 180 ms | solo le righe nuove: in tutto **un** ridisegno completo, in 4–6 fette (la prima, ~45%, parte al tocco) | fascia che si allarga dalla riga del riquadro; nessuna istantanea, nessun pixel disegnato due volte |
| ritorno alla griglia | 150 ms | come sopra | la fascia si richiude verso il riquadro |
| pannello rapido / calendario | 170 ms giù, 120 ms su | il popup fin dove arriva la scheda: al massimo 424×310 px | fondo catturato + ombra da maschera + scheda |
| dialogo | 140 ms | scheda + ombra (~484×270 px) | dissolvenza sul fondo già scurito (che resta un solo fotogramma pieno, come prima) |
| ● pressione di pulsanti e riquadri | immediata; rilascio 150 ms | il widget (un riquadro: 148×152) | il riquadro premuto si stringe di 3 px e schiarisce; al rilascio sfuma |
| ● cursori, interruttori, selettori a segmenti | 130 ms | il widget | il pomello scivola verso il punto toccato; durante il trascinamento segue il dito senza ritardo |
| ● toast | 170 ms su, 120 ms giù | 48 righe | si sposta la finestra, il contenuto è disegnato una volta |
| ● barra di scorrimento | compare subito, svanisce in 3 passi dopo 0,7 s | il solo cursore della barra (4 px di larghezza) | |
| elastico ai bordi di liste e pannelli | ritorno 220 ms | come uno scorrimento (l'area che scorre) | oltre il limite il contenuto segue il dito a un terzo della velocità |
| batteria in carica | 1 s, 12 passi | 28×16 px | un battito a ogni lettura della batteria (30 s), solo in carica |
| indicatore di pagina della griglia | — | ridisegnato con la griglia | segue lo scorrimento, nessun fotogramma suo |

I due punti dell'orologio non lampeggiano: sarebbe un risveglio al secondo
per sempre.

Per chi scrive una vista: `bui_anim_level()` dice il livello;
`bui_anim_add(step, user)` chiama `step` a ogni fotogramma (~33 ms) finché
ritorna ≠ 0; dentro `step` si calcola `t = bui_tween(start, durata, now)`
(0..1024) e si chiede il ridisegno del minimo. Le animazioni dei widget del
toolkit sono già fatte e non richiedono nulla.

## La home

- **Striscia delle informazioni** (72 px sopra la griglia): ora grande, giorno
  e data, tre indicatori (batteria, disco usato, RAM usata) e, se la musica
  suona, "In riproduzione". Si aggiorna allo scatto del minuto e alle letture
  della batteria — nessun risveglio in più — e solo se la griglia è in vista
  e lo schermo acceso; ogni parte ridisegna solo il proprio rettangolo.
- **In riproduzione**: `bs_home_set_now_playing(titolo, BS_NP_PLAYING |
  BS_NP_PAUSED)` (il testo viene copiato; `NULL` o `BS_NP_NONE` lo toglie).
  La chiama la vista Musica a ogni cambio di stato. Un tocco apre `music`.
  In orizzontale sta a destra nella striscia, in verticale su una riga sua.
- **Modo modifica**: tenere premuto un riquadro (0,45 s) lo solleva; si
  trascina per riordinare, il segno − / + in alto a destra nasconde o mostra,
  "Fine" (o Esc, Invio, Home) conferma. Ordine e riquadri nascosti sono
  salvati (`order=`, `hidden=`). Le app nuove che il file non conosce vanno
  in coda.
- **Ricerca**: scrivendo lettere o cifre sulla tastiera la griglia mostra solo
  le app con una parola che inizia così; Invio apre la prima, Backspace
  cancella, Esc annulla.
- **Indicatore di pagina** a destra quando i riquadri non entrano tutti.

Nella barra: un tocco sull'**orologio** apre il calendario del mese (frecce o
Pag↑/Pag↓ per cambiare mese, tocco sul titolo per tornare a oggi) con gli
ultimi tre avvisi (toast) mostrati; il resto della zona di stato apre le
impostazioni rapide come prima.

## Preferenze

`~/.config/brainshell/config`, testo `chiave=valore`; le chiavi sconosciute
si ignorano. Viene riscritto 0,7 s dopo l'ultima modifica, in modo atomico
(file temporaneo, `fsync`, `rename`), e all'uscita.

| chiave | valori | default |
|---|---|---|
| `theme` | `teal` `brace` `nero` `chiaro` `nord` `aurora` | `teal` |
| `animations` | `full` `reduced` `none` | `full` |
| `tiles` | `theme` `filled` `glass` `outline` | `theme` |
| `battery_percent` | `0` `1` | `1` |
| `dim` | secondi di inattività prima di attenuare lo schermo, 0 = mai | `120` |
| `off` | secondi prima di spegnerlo, 0 = mai | `0` |
| `sleep_after` | secondi a schermo spento (o di inattività, se `off` è 0) prima di sospendere, solo a batteria; 0 = mai | `300` |
| `power_saver` | `0` `1`: risparmio energetico (vedi *Energia*) | `0` |
| `order` | id delle app, separati da virgole, nell'ordine della griglia | ordine del registro |
| `hidden` | id delle app nascoste | nessuna |
| `key_kokugo` `key_eiwa` `key_english` `key_mydict` `key_history` | azione del tasto del dizionario: `standard` `none` `switcher` `quick` `screenshot` `saver` `music` `note` `app:ID` | `switcher` `quick` `music` `note` `screenshot` |

Accanto, facoltativo: `wallpaper.rgb565` (vedi *Temi*).

## Schermo e inattività

`idle.c` usa il contatore `IDLETIME` dell'estensione SYNC (che sta in
libXext: nessuna dipendenza nuova), cioè l'inattività di tutto il server X,
anche dentro le app esterne. Tre allarmi: inattività oltre `dim`, oltre
`off`, e di nuovo sotto la soglia più bassa. Nessun timer e nessuna
interrogazione periodica.

- **Attenuato**: `brain-ctl brightness N` con N = 30% del livello scelto
  dall'utente (che la shell continua a ricordare); qualunque attività lo
  ripristina e il tocco vale normalmente.
- **Spento**: `brain-ctl screen off`; se l'helper non conosce il verbo
  (uscita ≠ 0) la shell ripiega su `brain-ctl brightness 0` e non ci riprova.
  Una finestra InputOnly copre lo schermo: il **primo tocco o tasto riaccende
  soltanto** e viene scartato, anche se cade su un'app esterna. Al risveglio:
  `brain-ctl screen on` e la luminosità di prima. A schermo spento si ferma
  anche lo scatto del minuto.
- Se la shell esce con lo schermo attenuato o spento, prima lo ripristina.
- Senza SYNC (server X minimali) si ripiega su un timer che vede solo
  l'attività sulle finestre della shell.
- **Batteria scarica**: un toast quando, a batteria, il livello scende a 15%
  e a 5%.

Le soglie si scelgono in Impostazioni → Schermo (mai / 30 s / 1 / 2 / 5 min).

## Esplora e Galleria

Due viste integrate (`files` e `gallery` nel registro). Entrambe usano
`BS_VIEW_NO_HEADER`: disegnano da sé l'intestazione perché il visualizzatore
di immagini occupa tutta l'area sotto la barra di stato.

### Il filo di lavoro (`bs_work`)

Tutto ciò che può aspettare un disco gira in **un solo filo di lavoro** con
una coda: lettura delle cartelle (`readdir` + `stat` + ordinamento), ricerca
ricorsiva delle immagini, decodifica, lettura e a capo dei testi,
eliminazione ricorsiva, pulizia della cache. `bs_work_submit(run, done, arg,
front)` accoda; `run` gira nel filo di lavoro e non tocca né bui né lo stato
delle viste; `done` gira **sempre nel filo principale** (svegliato da una
pipe registrata con `bui_fd_add`), esattamente una volta, anche per un lavoro
annullato (serve a liberare `arg`). `bs_work_cancel(id)` toglie un lavoro
dalla coda o alza il flag `*cancel` di quello in corso: i decodificatori lo
controllano a ogni lettura dal file e a ogni blocco prodotto, quindi una
decodifica superata si ferma in pochi millisecondi. Ogni vista tiene un
contatore di generazione: un risultato che arriva dopo che l'utente è andato
altrove viene buttato. Il filo nasce al primo uso e poi dorme su una
condizione (zero CPU a riposo); ha tutti i segnali bloccati, quindi
`SIGCHLD`/`SIGTERM` continuano ad arrivare al filo principale. `pthread` fa
parte di libc: le dipendenze restano libX11, libXext, libc.

### Immagini (`bs_img`)

- **JPEG baseline** (quasi tutte le foto): TJpgDec, che decodifica a blocchi
  con 32 kB di memoria e sa uscire già ridotto a 1/2, 1/4, 1/8 (a 1/8 usa il
  solo coefficiente DC, senza IDCT). Si sceglie la riduzione più forte che
  lascia l'immagine ≥ della misura richiesta; le righe passano una striscia
  di MCU alla volta nel filtro di riduzione: una foto da 12 MP non sta mai in
  memoria per intero. Limite: 40 MP (`BS_IMG_MAX_PIXELS`).
- **PNG, BMP, GIF (primo fotogramma), JPEG progressivi**: stb_image
  (`STBI_NO_HDR`, `STBI_NO_LINEAR`, solo quei quattro formati). Decodifica
  tutta l'immagine in memoria, quindi il limite è 4 MP (2 MP per le GIF):
  oltre, "Immagine troppo grande per questo dispositivo". La trasparenza è
  fusa su un fondo scuro.
- **Riduzione**: media d'area esatta in interi (ogni pixel d'origine pesa per
  la parte che cade nel pixel d'uscita), riga per riga, con reciproci a
  virgola fissa al posto delle divisioni; mai ingrandimento. Uscita RGB565
  con dithering ordinato (`bui_pack_dither`). Rispetto a
  `convert -filter box -resize` il PSNR è 35–38 dB (la differenza è il
  passaggio a 16 bit).
- **Orientamento EXIF** rispettato (le foto scattate in verticale).
- File troncati: si mostra la parte arrivata. File rovinati: errore, mai un
  blocco (provato con ASan/UBSan su file corrotti a caso, `test/imgtest.sh`).
- Nessun float a runtime: il binario ARM di prova non contiene chiamate
  `__aeabi_f*/d*` e produce gli stessi pixel del PC sotto `qemu-arm -cpu arm926`.

### Galleria

Griglia delle immagini di `~/Immagini` (ricorsiva fino a 6 livelli, cartelle
nascoste escluse, al massimo 5000), le più recenti in testa; 5 colonne in
orizzontale, 3 in verticale. È una `bui_list` le cui righe contengono le
miniature: scorrimento cinetico e disegno delle sole righe visibili.

- **Miniature** 160×160 ritagliate al centro, create dal filo di lavoro e
  salvate in `~/.cache/brainshell/thumbs/<chiave>.t565` (`"BTH1"`, larghezza
  e altezza a 16 bit, poi RGB565 grezzo: 51 kB l'una). La chiave è l'FNV-1a a
  64 bit di percorso + mtime + dimensione: una foto modificata ha un'altra
  chiave. Alla riapertura la griglia si riempie dalla cache; dopo ogni
  ricerca i file di cache rimasti orfani vengono cancellati.
- **Memoria limitata**: al massimo 48 miniature in RAM (~2,4 MB); quando
  servono altri posti esce la più lontana da ciò che si vede. In coda ci sono
  al massimo 4 richieste, e quelle uscite dallo schermo vengono annullate:
  durante uno scorrimento veloce non si accumula lavoro inutile.
- **Visualizzatore**: l'immagine è decodificata già alla misura dell'area
  (854×440 o 480×814); restano in memoria la corrente e le due vicine
  (~750 kB l'una), quella nel verso di marcia viene preparata in anticipo.
  Finché la decodifica non arriva c'è un indicatore rotante (ridisegna solo
  il suo quadrato di 64 px, 8 volte al secondo, e si ferma all'arrivo).
  Un colpetto mostra/nasconde la barra (nome, "3 di 128", dimensioni,
  indietro, elimina, frecce); strisciata orizzontale o frecce = precedente /
  successiva; Inizio/Fine; Canc = elimina (con conferma); Esc = indietro.
- Tastiera nella griglia: frecce, Pag↑/Pag↓, Inizio/Fine, Invio apre.

### Esplora

Parte da `$HOME`. In orizzontale le scorciatoie (Home, Documenti, Immagini,
Musica, Video, Note e i supporti montati sotto `/media`, `/mnt`,
`/run/media`, letti da `/proc/mounts`) sono una colonna a sinistra; in
verticale una striscia di pulsanti in alto. Le cartelle standard che non
esistono vengono create al primo tocco.

- Righe da 64 px: icona per tipo, nome, dimensione e data; cartelle prima,
  poi ordine alfabetico senza distinzione fra maiuscole e minuscole.
  Intestazione con nome e percorso (`~/...`), occhio (file nascosti, spenti
  all'avvio; tasto `.`) e freccia in su (cartella superiore; Backspace).
  Tornando su resta selezionata la cartella da cui si esce.
- La cartella è letta dal filo di lavoro (al massimo 4000 voci, poi un
  avviso). Finché la risposta non arriva resta l'elenco precedente; dopo
  200 ms compare "Caricamento…".
- **Apertura per tipo**: immagini → visualizzatore della Galleria (sfoglia le
  immagini della cartella); `txt md log conf sh c h py json xml csv ini cfg`
  → lettore di testi; file senza estensione → lettore solo se sembrano testo
  (niente NUL, pochi caratteri di controllo nei primi 4 kB); video e audio →
  `brainplay FILE` con `bs_launch` (variabile `BRAIN_PLAYER`); il resto →
  "Tipo di file non supportato".
- **Lettore di testi**: sola lettura, primo MB, UTF-8 (i byte non validi
  sono letti come Latin-1), tabulazioni espanse, a capo fra le parole
  calcolato dal filo di lavoro; una riga di testo per riga di `bui_list`.
  Frecce, Pag↑/Pag↓, Spazio, Inizio/Fine.
- **Azioni** (pulsante ⋮ della riga, pressione lunga di 550 ms, oppure → da
  tastiera): Apri, Rinomina (campo di testo scritto dalla tastiera fisica,
  Invio conferma), Proprietà (nome, tipo, dimensione, data, permessi,
  posizione), Elimina (conferma; le cartelle vengono eliminate col
  contenuto, nel filo di lavoro). Canc elimina la riga col fuoco.
- Tastiera: frecce (← porta alle scorciatoie), Invio apre, una lettera o una
  cifra porta alla voce successiva che inizia così, Inizio/Fine, Esc chiude.

### Prove

`test/mkassets.sh` genera con ImageMagick una home finta (foto da 12 MP,
JPEG ruotato via EXIF, progressivo, grigio, PNG trasparente, BMP, GIF,
file troncati/finti/enormi, testi, un file da 1,5 MB, audio e video finti).
Le scene `files` e `gallery` la percorrono in entrambi gli orientamenti
(`test/run.sh 854x480 files`, `test/run.sh -d 480x854 gallery`) e fanno
parte di `test/all.sh`; `test/imgtest.sh` prova `bs_img.c` da solo
(confronto con ImageMagick, rifiuti attesi, file corrotti con ASan/UBSan).

## Note, Musica e Video

Tre viste che lavorano sulle cartelle dell'utente: `~/Note`, `~/Musica`,
`~/Video`. Condividono `media.h`.

### Elenchi che non fermano l'interfaccia (`media_scan.c`)

Su una scheda SD lenta `opendir`/`stat` (e la lettura dei tag) possono
bloccare per secondi. Li esegue un **processo figlio** (`fork` senza `exec`)
che scrive una riga per file su una pipe; il padre la legge, non bloccante,
dal `poll()` del toolkit (`bui_fd_add`). Niente thread: `NEEDED` resta
libX11/libXext/libc. Una `ms_lib` descrive la libreria (cartella sotto
`$HOME`, estensioni, ricorsione fino a 8 livelli, `probe()` eseguita nel
figlio per ricavare due testi, ordinamento); `ms_scan()` avvia la lettura a
ogni apertura della vista. La prima volta le voci compaiono man mano
("Ricerca dei brani…"); alle riaperture resta visibile l'elenco precedente e
viene sostituito tutto insieme a lettura finita. Al massimo 4000 voci; i
file nascosti e quelli con tab o a-capo nel nome sono ignorati.

### Note (`view_notes.c`, `view_notes_edit.c`)

- Una nota è un file UTF-8 `~/Note/*.txt`; titolo = prima riga non vuota,
  anteprima = il seguito. Elenco dal più recente, con data ("oggi 14:05",
  "18 nov", "24 dic 2025"). "Nuova nota" (o il tasto `N`) apre l'editor; il
  file `nota-AAAAMMGG-HHMMSS.txt` nasce al primo salvataggio (una nota nuova
  lasciata vuota non lascia file).
- **Editor**: buffer unico UTF-8 più la tabella degli inizi delle righe
  visibili (a capo automatico fra le parole; parole più larghe della riga
  spezzate fra due caratteri). Frecce, Home/Fine (Ctrl: inizio/fine testo),
  Pag↑/Pag↓, Invio, Backspace, Canc, Tab (4 spazi); su/giù conservano la
  colonna; un tocco posiziona il cursore, un trascinamento scorre con
  inerzia; la riga del cursore resta sempre in vista. All'apertura: tab →
  4 spazi, CR tolti, byte non UTF-8 letti come Latin-1.
- **Ridisegno minimo**: una modifica ricalcola solo il capoverso toccato e
  invia la riga cambiata *da dove inizia la modifica* (≈800×26 px per un
  carattere; 2×22 px per uno spostamento del cursore). Solo se cambia il
  numero di righe si ridisegna da lì in giù. Il cursore non lampeggia: a
  editor aperto e fermo la shell non si sveglia mai. Il titolo
  nell'intestazione si aggiorna al salvataggio, non a ogni tasto.
- **Salvataggio**: automatico 3 s dopo l'ultimo tasto, tornando all'elenco
  (freccia o Esc) e quando la vista si chiude (Home, altra app, uscita della
  shell). Scrittura su `~/Note/.NOME.tmp`, `fdatasync`, `rename`: sul disco
  c'è sempre una versione intera. È l'unica operazione sincrona (pochi kB;
  avviene a tastiera ferma). Se non riesce: avviso, e uscendo dall'editor un
  dialogo "Resta / Esci" invece di perdere il testo in silenzio.
- **Eliminazione**: cestino nell'intestazione dell'editor, con conferma.
- La vista usa `BS_VIEW_NO_HEADER` e si disegna l'intestazione (stessa
  geometria di quella della shell): la freccia dell'editor deve tornare
  all'elenco.
- Limite: 256 kB per nota (oltre, l'editor rifiuta di aprirla per non
  troncarla). Niente selezione né appunti.

### Musica (`view_music.c`, `view_music_player.c`, `view_music_tags.c`)

- Libreria: `~/Musica` ricorsiva (mp3 flac ogg opus wav m4a aac), ordinata
  per cartella e nome con i numeri in ordine naturale. Titolo/artista dai
  tag ID3v2 (2.2–2.4, primi 8 kB) o ID3v1 dei soli mp3, letti nel figlio
  della scansione; per gli altri formati vale il nome del file finché il
  lettore non comunica `title`/`artist`.
- Riproduzione: `brainplay --slave --no-window FILE` (`$BRAIN_PLAYER`), un
  processo per brano. Comandi sul suo stdin (`pause`, `play`, `seek N`,
  `volume N`, `quit`); il suo stdout (`state`, `pos`, `dur`, `title`,
  `artist`, `volume`) è una pipe non bloccante nel `poll()` del toolkit. La
  vista **non ha un timer**: si ridisegna quando il lettore scrive (cursore
  e tempo una volta al secondo mentre suona; zero risvegli in pausa).
- Pannello: titolo, artista · cartella, cursore di posizione (il comando
  `seek` parte solo al rilascio; durante il trascinamento cambia solo il
  tempo mostrato), precedente (entro 3 s dall'inizio va al brano prima,
  altrimenti torna a capo) / pausa / successivo, volume del lettore
  (ricordato e ripassato a ogni nuovo processo). In orizzontale il pannello
  sta a destra dell'elenco, in verticale sotto.
- Tastiera: frecce e Invio come ovunque (sinistra/destra sul cursore =
  ±5 %), Spazio pausa/riprendi, `N`/`P` successivo/precedente.
- **In sottofondo**: uscendo dalla vista la musica continua e a fine brano
  parte il successivo (lo stato vive in `view_music_player.c`, non nella
  vista); rientrando si ritrova il brano corrente. A fine elenco si ferma.
- Processi: `state ended`, cambio brano e uscita mandano `quit` + SIGTERM;
  alla chiusura della shell ci pensa un `atexit`; se la shell muore di
  colpo il lettore riceve SIGTERM (`PR_SET_PDEATHSIG`) e vede chiudersi lo
  stdin. I figli sono raccolti dal SIGCHLD di `sys.c`: niente zombie. Se il
  lettore non parte o rifiuta il file: "Riproduzione non riuscita", senza
  passare a raffica ai brani successivi.

### Video (`view_video.c`)

Elenco di `~/Video` ricorsivo (mp4 mkv avi webm mov mpg mpeg ts flv) con
nome, data · cartella, dimensione. Un tocco (o Invio) mette in pausa la
musica e avvia `brainplay FILE` a tutto schermo con `bs_launch()`, come le
altre app esterne (linguetta nella barra, "Impossibile avviare…" se manca).

### Piccole estensioni del toolkit

- `bui_x.c`, `map_key()`: i tasti che producono `€` (keysym `EuroSign`) o un
  keysym Unicode (`0x01000000 + codice`) ora arrivano col loro codice
  Unicode in `bui_key.key`; prima venivano scartati perché `XLookupString`
  rende solo Latin-1. L'editor converte il codice in UTF-8. Nessun metodo
  di input (niente tasti morti): la mappa italiana ha le accentate dirette.
- Il resto c'era già: `bui_fd_add()` per i descrittori, `bui_scroll_*` per
  l'inerzia, `bui_anim_add()`.

### Prove

```sh
test/all-media.sh                      # notes, music, video, media: ASan+UBSan, 854x480 e 480x854
test/run.sh 854x480 notes              # una scena sola (binario normale)
test/run.sh -e BUI_TRACE=1 854x480 media-perf
test/fuzz.sh [passi [seme]]            # editor a caso, senza X: vedi test/edit_fuzz.c
```

`test/brainplay` è un finto lettore (bash) che parla il protocollo slave,
finge la durata scritta nel file (`dur=N`) e annota avvii e comandi in
`$BRAIN_CTL_STATE/player.log`; `media_files` (in `test/lib.sh`) crea note,
brani (uno con un vero tag ID3v2) e video di esempio. Le scene scrivono
nell'editor con `xdotool type`/`key` (accentate ed euro compresi) e
controllano i byte dei file, i comandi arrivati al lettore, i processi
rimasti. `test/edit_fuzz.c` confronta, dopo ognuno di centinaia di migliaia
di tasti/tocchi/cambi di larghezza a caso, il testo con un modello e le
righe ricalcolate per differenza con un ricalcolo completo.

## Terminale (`view_term.c`, `vt.c`)

Fino alla 0.3 il riquadro Terminale avviava X + `st` (circa 11 s sul
Brain). Ora e' una vista integrata: si apre subito, disegna nel compositore
come le altre viste e la sessione resta viva quando si torna alla griglia.

### Processo

- PTY con `posix_openpt` / `grantpt` / `unlockpt`; il figlio fa `setsid`,
  prende il terminale di controllo (`TIOCSCTTY`), chiude tutti gli altri
  descrittori, rimette i segnali di default ed esegue la shell di login:
  `$SHELL` se e' un percorso eseguibile, altrimenti `/bin/bash` (o
  `/bin/sh`), con `argv[0]` col trattino (`-bash`: legge `~/.bash_profile`).
- `TERM=xterm-256color` se la terminfo c'e' (`ncurses-base` di Debian la
  installa in `/usr/share/terminfo/x/`), altrimenti `xterm`;
  `COLORTERM=truecolor`; `LANG=C.UTF-8` se manca.
- Dimensione con `TIOCSWINSZ` a ogni layout (rotazione compresa): il kernel
  manda `SIGWINCH` al programma in primo piano.
- Il lato master e' non bloccante e sta nel `poll()` del toolkit
  (`bui_fd_add`); a ogni risveglio si leggono al massimo 64 kB, poi si torna
  al ciclo (l'interfaccia resta reattiva anche sotto `yes`).
- Il figlio e' sorvegliato con `bs_child_watch` (SIGCHLD di `sys.c`, niente
  zombie). A processo finito: `[processo terminato]` nel terminale, al posto
  dei tasti della barra un pulsante **Riavvia**; anche Invio riavvia. Esc o
  Backspace a quel punto chiudono la vista come altrove.
- **Sessione**: uscendo dalla vista (Home, freccia, un'altra app) il PTY
  continua a essere letto e l'emulatore aggiornato, senza disegnare;
  rientrando si ridisegna tutto. Una sessione sola.

### Emulatore (`vt.c`)

Stato puro (nessun bui, nessun PTY): griglia di celle `{codice, fg, bg,
attributi}` da 12 byte, righe come array di puntatori (uno scorrimento
ruota puntatori), storico ad anello di 1000 righe. Copre cio' che serve a
bash/readline, less, top, htop, nano, vim, apt:

- UTF-8 (sequenze non valide → U+FFFD), caratteri larghi su due celle
  (CJK, emoji: tabella di larghezze come glibc), combinanti ignorati;
- movimenti (CUU/CUD/CUF/CUB/CNL/CPL/CHA/HPA/VPA/CUP...), salvataggio del
  cursore (DECSC/DECRC, CSI s/u), modo origine, a capo automatico differito
  (DECAWM), tabulazioni (HTS/TBC/CHT/CBT);
- SGR: grassetto, attenuato, corsivo (annotato, non disegnato),
  sottolineato, lampeggiante (ignorato), inverso, nascosto, barrato; 16
  colori, 256 colori, truecolor `38;2;R;G;B` e `38:2::R:G:B` convertito in
  RGB565;
- cancellazioni ED/EL/ECH col colore di fondo corrente (`bce`, come vuole la
  terminfo di xterm), ICH/DCH, IL/DL, SU/SD, REP, modo inserimento (IRM);
- regioni di scorrimento (DECSTBM), IND/RI/NEL;
- schermo alternativo 47/1047/1049 (con salvataggio del cursore), DECCKM
  (frecce `ESC O A`), DECKPAM, DECSCNM (schermo inverso), DECTCEM;
- set grafico DEC (`ESC ( 0`, SO/SI: i riquadri di ncurses), DECALN;
- risposte: DA1 (`ESC[?62;22c`), DA2 (un VT220, cosi' vim non chiede
  altro), DSR 5/6 (posizione del cursore);
- OSC 0/1/2: il titolo va nell'intestazione; altri OSC, DCS, APC, PM
  ignorati fino a ST/BEL; incolla tra parentesi (2004), mouse
  (1000/1002/1006...), forma del cursore e operazioni sulla finestra
  annotati o ignorati.

Ogni modifica annota le colonne cambiate della riga. Gli scorrimenti
consecutivi della stessa regione si sommano in uno solo, spostando con le
righe anche i segni di "cambiata": chi disegna sposta i pixel una volta per
fotogramma e ridisegna solo le righe nuove (`sc_top`, `sc_bot`, `sc_n`).

### Disegno

- **Font**: DejaVu Sans Mono 13 px, normale e grassetto (`BUI_FONT_MONO` e
  `BUI_FONT_MONO_B`, 524 glifi l'uno: ASCII, Latin-1, Latin Extended-A,
  greco, punteggiatura, frecce, simboli matematici e geometrici; +85 kB),
  in **celle da 8×16 px**: **106×24** in orizzontale (sotto barra e
  intestazione), **60×44** in verticale. A 13 px l'avanzamento di DejaVu Mono
  e' 7,8 px: le celle da 8 restano allineate senza spazi. Le maiuscole
  accentate (13 px sopra la linea di base) scendono di un pixel per entrare
  nella cella.
- **Linee dei riquadri** (U+2500–257F: sottili, spesse, doppie, archi,
  diagonali), **blocchi** (U+2580–259F, ombreggiature comprese), **braille**
  (U+2800–28FF, i grafici di htop/btop) e le linee di scansione del set DEC
  sono disegnati a misura di cella: si uniscono senza buchi fra le righe.
  Un carattere senza glifo e' un rettangolo; un carattere largo un
  rettangolo su due celle.
- **Colori** dal tema: fondo `BUI_C_BG`, testo `BUI_C_TEXT`, cursore a
  blocco nel colore d'accento; i 16 colori ANSI hanno una tavolozza per i
  temi scuri e una per quelli chiari (Chiaro); 256 colori e truecolor come
  xterm. Grassetto sui colori 0–7 = la versione chiara (come xterm).
- **Solo le celle cambiate**: l'uscita del programma si accumula e si
  disegna al massimo ~30 volte al secondo (`RENDER_MS` 33). Le celle
  cambiate vengono disegnate **direttamente nella tela** e inviate con
  `bui_win_present()` (nuovo nel toolkit: invia un rettangolo senza
  ridisegnarlo); uno scorrimento sposta le righe della tela con `memmove`
  e ne invia la fascia, poi si disegnano solo le righe nuove. Se la tela
  non e' "nostra" (transizione in corso, un rettangolo della finestra in
  attesa di ridisegno) si ripiega sul percorso normale `bs_view_damage` →
  `draw()`, che ridisegna dalle celle. Il cursore non lampeggia (nessun
  risveglio a terminale fermo).
- Numeri (backend mem, 854×480): una riga scritta = una fascia da 16 righe
  di pannello; 120 righe di uscita in un colpo = un fotogramma con uno
  spostamento della regione e le righe nuove.

### Storico

1000 righe (le righe che escono dalla cima dello schermo principale; lo
schermo alternativo non ne aggiunge). Trascinamento col dito con inerzia,
rotellina, **Maiusc+PagSu/PagGiù** una pagina; mentre si guarda lo storico
la vista resta ferma sulle stesse righe anche se il programma scrive, e un
indicatore in basso a destra dice quante righe mancano ("storico −42"). **Un
tasto qualsiasi torna in fondo** (e arriva al programma). Nello schermo
alternativo (less, man, vim) il trascinamento manda invece frecce su/giù al
programma.

### Tastiera e barra dei tasti

| tasto | al programma |
|---|---|
| lettere, cifre, simboli | il carattere (UTF-8); le accentate con Onsei come altrove (à è é ì ò ù ç € °) |
| Page↑ (Ctrl) + lettera | il codice di controllo (Ctrl+C, Ctrl+D, Ctrl+Z, Ctrl+[ ...): `bui_evdev.c` ora li consegna (prima Ctrl+lettera non produceva nulla) |
| Onsei + lettera senza accentata | `ESC` + lettera (Meta) |
| frecce | `ESC [ A` o, in modo applicazione (DECCKM), `ESC O A`; con modificatori `ESC [ 1 ; m A` |
| Invio, Backspace, Tab, Maiusc+Tab | `CR`, `DEL` (0x7F, come la terminfo), `TAB`, `ESC [ Z` |
| **Go Back** (Esc) | `ESC` al programma: nel terminale **non** chiude la vista |
| Kokugo/Eiwa/Brain English/My Dictionary/Rireki | PagSu, PagGiù, Tab, Ins, Canc (`ESC [5~`, `ESC [6~`, `TAB`, `ESC [2~`, `ESC [3~`) |
| Maiusc + Kokugo/Eiwa | storico (non vanno al programma) |
| **Home** | alla griglia (la sessione resta) |

Per uscire dalla vista: **Home** o la **freccia** dell'intestazione (Esc va
al programma). La **barra dei tasti** a tocco (nell'intestazione in
orizzontale, su una riga sua in verticale) ha `Esc Tab Ctrl ↑ ↓ ← → | ~ / -`:
**Ctrl resta premuto** (evidenziato) e vale per il tasto successivo, dalla
barra o dalla tastiera. Niente selezione ne' appunti.

Una **pressione lunga** (0,8 s) sul testo chiede "Aprire in X?" e avvia `st`
in una sessione X come prima della 0.4 (il ripiego; `BRAINSHELL_TERMINAL=x11`
fa aprire direttamente st dal riquadro).

### Energia

Il terminale **non impedisce** attenuazione, spegnimento dello schermo e
sospensione automatica, **tranne** quando un programma ha scritto negli
ultimi 30 s: un comando in primo piano (il gruppo in primo piano del PTY,
`tcgetpgrp`, non e' la shell) oppure un'uscita che non e' l'eco di un tasto
premuto da meno di un secondo. Cosi' `apt upgrade`, `make` o un `ping` che
scrive tengono sveglio il Brain finche' lavorano, mentre la shell ferma al
prompt (o un `sleep`, o un editor in attesa) lo lascia dormire.
`bs_power_can_autosleep()` lo chiede a `bs_term_keep_awake()`; quando non si
puo' sospendere si ricontrolla ogni 30 s come per la musica. In sospensione
anche i programmi del terminale si fermano (e riprendono al risveglio).

### Prove

- `test/vt_test.c` (`make tests`): 162 controlli. Flussi di byte registrati
  (`test/vt/*.raw`, rifatti con `test/vt/record.sh` in un contenitore con
  tmux, vim-tiny, nano, less, htop) di bash con prompt colorato, `ls
  --color`, readline che va a capo e inserisce, `top`, `htop`, la schermata
  iniziale di vim, vim che scorre (Ctrl-E/Ctrl-Y) e inserisce una riga,
  nano, less, la riga di avanzamento di apt (regione di scorrimento),
  frammenti alla vttest (DECALN, margini, IRM, ICH/DCH/ECH, IL/DL, set
  grafico, caratteri larghi tagliati, schermo alternativo, truecolor,
  barrato): testo di ogni riga e cursore uguali a quelli di tmux
  (l'emulatore di riferimento, `test/vt/*.txt`). Poi gli stessi flussi
  spezzati a caso in pezzi da 1–300 byte: dopo ogni pezzo una tela finta
  aggiornata solo dalle celle segnate (e dagli scorrimenti in attesa) deve
  coincidere con la griglia. Piu' sequenze a mano: colori e attributi,
  `bce`, regioni, IL/DL, ICH/DCH/ECH, DECOM, schermo alternativo, risposte
  DA/DSR, titolo con BEL e con ST, UTF-8 non valido, caratteri larghi,
  DEC grafico, SO/SI, REP, ridimensionamento con lo storico, e i byte dei
  tasti (frecce normali/DECCKM, Ctrl, Alt, Canc, Maiusc+PagSu...).
- Scena mem `term` (un vero `/bin/sh` nel contenitore,
  `BRAINSHELL_TERM_SHELL`): comandi scritti dalla tastiera evdev che creano
  file, colori, accentate in UTF-8 sullo schermo, 120 righe e storico
  trascinato, Ctrl+C dal tasto Ctrl e dal Ctrl della barra, Esc che resta
  nel terminale, tasti del dizionario che restano tasti, sospensione
  trattenuta da un programma che scrive e poi liberata
  (`BRAINSHELL_TERM_AWAKE_MS`), Home e ritorno con la sessione viva,
  freccia, `exit` → "[processo terminato]" → Invio → shell nuova.
  `BRAINSHELL_TERM_DUMP` scrive il testo dello schermo per i controlli.
- Scena mem `termdraw`: i flussi registrati (htop, vim, apt, vttest, less)
  e 300 righe di seguito mandati al terminale con `cat`; lo schermo
  disegnato per differenza (celle nella tela, righe spostate con memmove)
  deve essere identico, pixel per pixel, a quello ridisegnato da zero dopo
  Home e rientro.
- Entrambe in orizzontale e in verticale, con ASan+UBSan (`test/mem.sh -d`)
  e col binario ARM sotto `qemu-arm -cpu arm926` (`test/mem.sh -q`).

### Da verificare sul Brain

Leggibilita' delle celle 8×16 sul pannello da 5" (alternativa: un font da
12 px in celle 7×14 = 122×27), velocita' con un fiume di uscita (`yes`,
`find /`) e durante lo scorrimento dello storico (ridisegno di tutto il
testo, ~25 fotogrammi al secondo stimati), htop e vim veri, `apt` con la
barra in fondo, il tasto Page↑ come Ctrl, Onsei come Alt/Meta, la
sospensione automatica durante un `apt upgrade`.

## Tasti del dizionario (`keys.c`, `view_keys.c`)

Il device tree del PW-SH5 manda i tasti del dizionario come tasti normali.
La shell li riconosce e, **fuori dai testi**, fa con loro un'azione
scelta in **Impostazioni → Tasti** (una pagina per tasto: tasto normale,
nessuna azione, app recenti, impostazioni rapide, schermata, risparmio
energetico sì/no, Musica riproduci/pausa, Note, apri un'app a scelta; e la
pagina **Guida ai tasti**).

| tasto | codice | predefinito | con Onsei (Alt) |
|---|---|---|---|
| **Kokugo** (国語) | `KEY_PAGEUP` | **App recenti**: ogni pressione passa alla successiva; Invio, un tocco, oppure lasciarlo dopo averlo tenuto (≥ 0,45 s) apre la scelta; Esc annulla | luminosità −10 |
| **Eiwa · Waei** (英和・和英) | `KEY_PAGEDOWN` | **Impostazioni rapide** (di nuovo: le chiude) | luminosità +10 |
| **Brain English** | `KEY_TAB` | **Musica**: pausa/ripresa del brano caricato; senza brano apre Musica | audio disattivato / di nuovo al volume di prima |
| **My Dictionary** (マイ辞書) | `KEY_INSERT` | **Note**; tenuto 0,5 s: **nota nuova** | volume −10 |
| **Rireki** (履歴) | `KEY_DELETE` | **Schermata** (`brain-ctl screenshot`, con l'avviso "Schermata salvata") | volume +10 |
| Page↑ (ページ↑) | `KEY_LEFTCTRL` | Ctrl | — |
| Onsei (音声) | `KEY_LEFTALT` | Alt (accentate) | — |
| Home | `KEY_HOMEPAGE` | alla griglia (vedi *Tocco e tastiera*) | — |
| Go Back (戻る) | `KEY_ESC` | Esc / indietro | — |

Regole:

- **Testi**: nel Terminale, nell'editor delle Note e nel campo di rinomina
  di Esplora (la vista lo dice con il nuovo callback `text_input`) i cinque
  tasti restano PagSu, PagGiù, Tab, Ins e Canc. Anche **nei popup**
  (dialoghi, impostazioni rapide, calendario, menu di spegnimento): Tab
  sposta il fuoco, PagSu/PagGiù cambiano mese; solo il tasto delle
  impostazioni rapide le richiude.
- **Maiusc o Ctrl** + tasto: sempre il tasto normale (Maiusc+Tab, Ctrl+PagSu).
- **Onsei** + tasto: luminosità e volume **ovunque**, anche nel Terminale
  (con un avviso "Luminosità 60%"). Tenuto premuto si ripete.
- **Pressione lunga**: evdev manda anche il rilascio di questi cinque tasti
  (`BUI_K_RELEASED`, solo all'aggancio della shell, mai alle finestre). La
  ripetizione resta (PagGiù o Canc tenuti nei testi) ma fuori dai testi
  conta solo la prima pressione. Sotto X (`brainshell-x11`) il rilascio non
  arriva: My Dictionary apre sempre Note e il selettore si chiude con Invio.
- **App recenti**: le app aperte, dalla più recente (al massimo 8; le
  esterne comprese). Dalla griglia la prima pressione sceglie la più
  recente, da una vista quella prima (come Alt+Tab). Sotto ogni app:
  "aperta", "sessione attiva" (Terminale), "in riproduzione" (Musica).
  È un popup modale grande quanto lo schermo col fondo scurito.
- Le scelte sono salvate come `key_kokugo`, `key_eiwa`, `key_english`,
  `key_mydict`, `key_history` (vedi *Preferenze*); un valore sconosciuto
  vale il predefinito. "Scelte iniziali" nella pagina Tasti le ripristina.

Prove: `test/evdev_test.c` (pressione, ripetizione e rilascio dei cinque
tasti, con Onsei, Ins, Ctrl+lettera → codici di controllo, Ctrl+Spazio che
non arriva) e la scena mem `dictkeys` (selettore con pressioni ripetute,
Invio, tenuto e rilasciato; impostazioni rapide aperte e chiuse; schermata;
Musica; Note e nota nuova col tasto tenuto, con Tab/Canc/PagSu che restano
tasti nell'editor; Onsei per luminosità, volume e muto; Maiusc+Tab e
Ctrl+PagSu normali; in orizzontale Impostazioni → Tasti → Rireki → "Apri
un'app" → Calcolatrice, salvato nel file, poi Rireki che apre la
Calcolatrice).

Da verificare sul Brain: che il device tree mandi davvero quei codici (e il
rilascio), la pressione lunga di My Dictionary e di Kokugo con le dita, e
che Onsei+tasto non dia fastidio scrivendo le accentate.

## Il toolkit bui in breve

**Colori** (`BUI_C_*`): `BG`, `BAR`, `SURFACE`, `SURFACE2`, `SURFACE3`, `LINE`,
`TEXT`, `TEXT2`, `TEXT3`, un solo accento (`ACCENT`, `ACCENT_P`, `ACCENT_BG`,
`ON_ACCENT`) più `DANGER` e `WARN` per i significati: sono campi del tema
corrente `bui_th` (vedi *Temi*), con `bui_theme_count/get/find/set/current`.
`BUI_RGB(r,g,b)` / `BUI_HEX(0xRRGGBB)` per altri colori.

**Primitive**: `bui_fill`, `bui_fill_a` (alfa), `bui_grad_v` (sfumatura con
dithering ordinato 8×8), `bui_rrect` / `bui_rrect_a` / `bui_rrect_line`
(angoli con antialiasing, maschere d'angolo in cache), `bui_mask` (A8
colorata), `bui_blit` (RGB565), `bui_blit_a` (miscelata), `bui_blit_rr`
(dentro un rettangolo arrotondato: il "vetro"), `bui_shadow` (da maschera A8
in cache), `bui_mix`, `bui_pack_dither`; clip con `bui_clip_push/pop`.

**Testo** (UTF-8): `bui_text`, `bui_text_width`, `bui_text_box` (una riga
allineata, troncata con "…"), `bui_text_wrap` (a capo automatico; con tela
`NULL` misura). Font disponibili:

| macro | font | glifi |
|---|---|---|
| `BUI_FONT_SMALL` / `_SMALL_B` | DejaVu Sans / Bold 15 px | ASCII, Latin-1 (À–ÿ), `° ± × ÷ µ · « » – — ‘ ’ “ ” • … € ← ↑ → ↓ − ✓` |
| `BUI_FONT_BODY` / `_BODY_B` | 18 px | come sopra |
| `BUI_FONT_TITLE` | Bold 22 px | come sopra |
| `BUI_FONT_KEY` | Sans 26 px | `0-9 . , : + - = % ( ) / * A C E e ± × ÷ − √` |
| `BUI_FONT_NUM` | Sans 44 px | `0-9 . , : + - = % ( ) E e × ÷ − …` |
| `BUI_FONT_HUGE` | Sans 88 px | `0-9 : . , -` |
| `BUI_FONT_MONO` / `_MONO_B` | DejaVu Sans Mono / Bold 13 px (terminale, celle 8×16) | ASCII, Latin-1, Latin Extended-A, greco, punteggiatura, frecce, simboli matematici e geometrici comuni (524 glifi) |

Le dimensioni non sono casuali: a 15, 18, 22, 26, 44 e 88 px l'altezza delle
maiuscole e della x di DejaVu cade quasi su pixel interi, quindi il testo
resta nitido anche senza hinting. Coppie di kerning ASCII incluse per i font
di testo.

**Icone**: `BUI_IC(nome, px)` → `bui_icon_draw` / `bui_icon_in`. A 24 px:
`home back close vol_up vol_down vol_off bolt brightness rotate screenshot
power restart play pause replay flag add remove check info usb memory storage
battery backspace chevron expand warning timer hourglass settings clock calc
terminal folder files gallery music video note apps more trash edit up eye
eye_off image doc file audio sdcard prev open`; a 40 px le icone delle
app (`settings clock calc terminal folder files gallery music video note apps
timer`); `close` anche a 18, `bolt` a 16. Per aggiungerne: una riga nella
tabella `icons[]` di `tools/bake.c` (nome Material Icons) e `make bake`.

**Widget** (`bui_add_*` su un pannello): etichetta, pulsante (stili `TONAL`,
`FILLED`, `DANGER`, `GHOST`, `KEY`, `KEY_OP`, `SEG`, `ROUND`; icona a sinistra
o sopra con `BUI_F_ICON_TOP`), riquadro, cursore (`on_change(w, final)`),
interruttore, riga etichetta/valore, scheda, lista virtuale con scorrimento
cinetico (`bui_list`, `bui_list_row` per la riga standard), widget
personalizzato (`draw`). `bui_header_build` costruisce l'intestazione di
pagina; `bui_dialog_open` il dialogo modale (fino a 3 pulsanti);
`bui_toast` il messaggio breve. Stato: `bui_widget_set_value/_text/_flag`,
flag `HIDDEN DISABLED SELECTED NOFOCUS DIVIDER DIM`.

**Ciclo**: `bui_timer_add/del`, `bui_anim_add/del` (un passo per fotogramma
finché ritorna ≠ 0), `bui_fd_add/del`, `bui_now()`.

**Nuovi nella 0.4**: `bui_win_present(win, r)` invia un rettangolo della tela
senza ridisegnarlo (pixel scritti fuori da `paint`, come le righe del
terminale spostate con `memmove`; x11: come un'esposizione, drm/mem: solo
ricomposizione); `bui_set_key_hook(f)`: un aggancio chiamato per ogni tasto
dopo i filtri e prima della finestra (lo usa `keys.c`); `BUI_K_INSERT`;
`BUI_K_RELEASED` (rilascio dei tasti del dizionario, solo all'aggancio);
Ctrl+carattere consegnato come codice di controllo (0x01–0x1F, 0x7F) con
`BUI_MOD_CTRL`, sia da evdev sia da X (le viste che scrivono ignorano già i
caratteri sotto 0x20); fino a 6 filtri d'ingresso.

`brainshell --open demo` mostra tutto il campionario.

## Contratto col sistema

| cosa | come |
|---|---|
| batteria | `/sys/class/power_supply/brain-battery/{capacity,status,voltage_now}`, `/sys/class/power_supply/brain-usb/online`; letti all'avvio e poi ogni 30 s |
| memoria | `/proc/meminfo` (`MemTotal`, `MemAvailable`) all'apertura delle Impostazioni |
| kernel | `uname()` (con `BRAIN_SYSROOT`: `$BRAIN_SYSROOT/proc/sys/kernel/osrelease`) |
| spazio | `statvfs($HOME)` |
| tutto ciò che è privilegiato | `brain-ctl` (default `/usr/local/bin/brain-ctl`, variabile `BRAIN_CTL`) |

`brain-ctl suspend [S]` (backend drm): sospende il sistema e ritorna solo
al risveglio (S = secondi per la sveglia RTC); vedi *Energia*.
`brain-ctl brightness` / `volume` stampano 0–100; con un argomento
impostano; `rotate`, `screenshot` (stampa il percorso del PNG), `poweroff`,
`reboot`; **`screen off` / `screen on`** (nuovo, facoltativo: spegne e
riaccende pannello e retroilluminazione, uscita 0 se riuscito; se manca la
shell usa `brightness 0` e poi il livello di prima — vedi *Schermo e
inattività*. Nota: l'helper attuale in `os/brain-ctl` non ha ancora il verbo
e porta `brightness 0` a 1, quindi oggi "spento" sul Brain significa
retroilluminazione al minimo; basta aggiungere a `brain-ctl` un ramo
`screen)` che scriva `bl_power` del backlight). Ogni chiamata è un processo figlio con stdout su una pipe dentro
`poll()`: l'interfaccia non aspetta mai (provato con un `brain-ctl` che
impiega 2 s). Durante il trascinamento di un cursore parte al massimo un
processo ogni 140 ms e mai due insieme; l'ultimo valore viene sempre
inviato; una lettura partita prima di una scrittura viene scartata. Il
cursore della luminosità parte da 5 per non spegnere lo schermo. La
schermata viene chiesta 400 ms dopo la chiusura del pannello.

Programmi esterni: `fork` + `setsid` + `execvp`, raccolti con `SIGCHLD`;
se il programma manca compare "Impossibile avviare ...".

File mancanti o `brain-ctl` assente: segnaposto ("—", batteria vuota, cursori
disattivati), mai un errore.

Variabili d'ambiente:

| variabile | effetto |
|---|---|
| `BRAIN_CTL` | percorso dell'helper |
| `BRAIN_SYSROOT` | prefisso per `/sys` e `/proc` (prove) |
| `BRAIN_PLAYER` | percorso di `brainplay` (Musica e Video; default: `brainplay` nel `PATH`) |
| `BUI_TRACE` | stampa ogni rettangolo inviato al server |
| `BUI_DEBUG` | stampa gli errori X (normalmente ignorati) |
| `BUI_NO_SHM` | forza `XPutImage` |
| `BRAINSHELL_TIMING` | stampa i ms da `main()` al primo fotogramma completo |
| `BRAINSHELL_EXTRA_TILES=N` | aggiunge N riquadri finti (per provare lo scorrimento della griglia) |
| `BRAINSHELL_CHILD_OUTPUT` | non chiude stdout/stderr dei programmi lanciati |
| `BRAINSHELL_IDLE_MS=A,S[,Z]` | soglie di inattività in ms (attenua, spegni, sospendi) al posto delle preferenze: solo per le prove |
| `BRAINSHELL_BATTERY_MS` | intervallo di lettura della batteria (default 30000): solo per le prove |
| `BRAINSHELL_NO_SYNC` | non usa l'estensione SYNC (prova il ripiego di `idle.c`) |
| `BRAINSHELL_BACKEND` | `drm`, `x11`, `mem` (vedi *Senza X*) |
| `BRAINSHELL_XORG` | comando che avvia X per un'app esterna (default `sudo -n /usr/local/sbin/brain-xapp`) |
| `BRAINSHELL_READY_FILE` | file creato al primo fotogramma (lo aspetta `brain-preinit`) |
| `BRAINSHELL_DRM_DEVICE` | al posto di `/dev/dri/card0` |
| `BRAINSHELL_EVDEV` | dispositivi di ingresso separati da virgole, al posto della ricerca per nome |
| `BRAINSHELL_TOUCH_CAL`, `BRAINSHELL_ROTATION` | al posto di `99-calibrator.conf` e `/etc/brain-rotation` |
| `BRAINSHELL_ST_CELL` | cella di st in px (default `8x16`) per la griglia a tutto schermo |
| `BRAINSHELL_TERMINAL=x11` | il riquadro Terminale apre st in una sessione X (come fino alla 0.3) invece della vista integrata |
| `BRAINSHELL_TERM_SHELL` | programma della vista Terminale al posto di `$SHELL` (prove: `/bin/sh`) |
| `BRAINSHELL_TERM_DUMP` | file in cui il Terminale scrive il testo dello schermo a ogni fotogramma (prove) |
| `BRAINSHELL_TERM_AWAKE_MS` | al posto dei 30 s di uscita che trattengono la sospensione (prove) |
| `BRAINSHELL_SCRIPT`, `BRAINSHELL_MEM_SIZE`, `BRAINSHELL_SHOTS`, `BUI_MEM_STATE` | backend mem (vedi `bui_mem.c`) |

Le preferenze sono descritte in *Preferenze*.

## Prove

```sh
test/run.sh 854x480 final          # tutte le schermate in orizzontale -> shots/landscape-*.png
test/run.sh 480x854 final          # in verticale -> shots/portrait-*.png
test/run.sh -d 854x480 keys        # -d: binario con ASan + UBSan
test/run.sh -e BUI_TRACE=1 -e BRAINSHELL_TIMING=1 854x480 perf
test/all.sh                        # tutte le scene con ASan, riassunto ok/FALLITA
test/qemu.sh                       # binario ARM sotto qemu-user: stessi pixel del nativo?
```

`test/session.sh` (dentro il contenitore) avvia `Xvfb :99 -screen 0 WxHx16`
(stesso visual RGB565 del Brain), JWM con `test/jwmrc`, un `BRAIN_SYSROOT`
finto, il finto `test/brain-ctl` (che annota ogni chiamata con l'ora) e due
finti `st`/`xfe` (xterm e xclock); poi esegue la scena, uno script di
comandi `tap`, `press`/`release`, `drag`, `key`, `shot` (`test/lib.sh`).

| scena | cosa prova |
|---|---|
| `final`, `final-grid` | ogni pagina, i popup, finestre esterne, fuoco da tastiera; salva le schermate definitive |
| `land1` `land2` `land3` `port1` | viste, impostazioni rapide, dialoghi, linguette (attiva/chiudi), Home |
| `scroll` | inerzia di griglia e liste |
| `keys` | tutta la navigazione da tastiera |
| `states` | batteria in carica/bassa/critica/piena, file mancanti, `brain-ctl` assente, programma inesistente, niente zombie |
| `misc` | tocco che ferma l'inerzia, `brain-ctl` lento, molte finestre |
| `nowm` (`-e NO_WM=1`) | senza gestore di finestre |
| `resize` | cambio di dimensione dello schermo a caldo |
| `perf` | avvio, memoria, CPU a riposo, rettangoli inviati |
| `qemu` (da `test/qemu.sh`) | binario ARM sotto `qemu-arm -cpu arm926`, confronto dei pixel col nativo |
| `demo` (`--open demo`) | campionario del toolkit |
| `files` | Esplora: scorciatoie, file nascosti, apertura per tipo, lettore di testi, azioni, rinomina, eliminazione, pressione lunga, lancio del lettore, niente zombie |
| `notes` `music` `video` | Note, Musica e Video con controlli automatici (`CHECK ok` / `CHECK FALLITO`), vedi *Note, Musica e Video*; `test/all-media.sh` le esegue con ASan |
| `media` | schermate definitive delle tre viste (`shots/*-34..45-*.png`) |
| `media-perf` (`-e BUI_TRACE=1`) | rettangoli inviati mentre si scrive e mentre suona, CPU a riposo |
| `rice` | transizioni e pannelli catturati a metà, calendario, dialogo, ricerca, modo modifica, toast, ogni tema (home, riquadro premuto, Impostazioni, pannello rapido), cambio di tema a caldo |
| `rice2` (`-e BRAINSHELL_EXTRA_TILES=14`) | "In riproduzione" nella home, i tre stili dei riquadri, sfondo dell'utente, elastico della griglia |
| `idle` (`-e BRAINSHELL_IDLE_MS=2000,4000`) | attenuazione, spegnimento, primo tocco che sveglia soltanto; con `-e BRAIN_CTL_NO_SCREEN=1` il ripiego sulla luminosità |
| `test/cg.sh` (immagine con valgrind) | istruzioni (callgrind) per avvio, tocco su un riquadro, ritorno |
| `gallery` | Galleria: miniature e cache, scorrimento, tastiera, visualizzatore, strisciata, eliminazione, immagini rovinate |

Note: per le schermate si usa `xwd`, non `import` di ImageMagick (non cattura
le finestre con *save-under* e disturba i grab). Le coordinate nelle scene
sono quelle dei due formati provati.

## Prestazioni

Misurate sul PC di sviluppo (aarch64, Xvfb + JWM) — vedi la scena `perf`:

| | misurato | atteso sul Brain |
|---|---|---|
| binario armel senza simboli | 541 kB (0.1: 335 kB). Temi, animazioni, home, inattività: +43 kB; Esplora, Galleria e decodificatori ~130 kB; Note, Musica, Video ~35 kB; 0.4: 749 kB col Terminale (vedi *Senza X* → *Misure*) | — |
| da `main()` al primo fotogramma completo | 12–19 ms; 10,4 M di istruzioni (callgrind; 0.1: 10,5 M), tre quarti per lo sfondo. Tema con vetro (Aurora): un secondo passaggio dello sfondo, ~+7 M | 150–250 ms: ~60 ms di CPU nostra, il resto caricamento delle librerie e lavoro del server X |
| CPU a riposo | 1 tick di CPU in 65 s; 7 risvegli: minuto (barra + ora della home) e batteria. Nessun timer delle animazioni o dell'inattività | lo stesso |
| RSS | 4,1 MB come nella 0.1 (1,1 anonima, 0,9 tele condivise, 2,1 codice e librerie mappati); +0,8 MB col vetro | simile |
| tocco su un riquadro (premuto) | un solo invio di 148×152 px | ~5 ms di bus (152 righe) |
| apertura di una vista (Calcolatrice) | 4,61 M di istruzioni contro 4,50 M della 0.1 (+2,5%); 5 fette di 204+120, 92, 36, 4 righe: 440 righe in tutto, come un solo fotogramma | 17 ms di bus in tutto, spalmati su ~180 ms |
| ritorno alla griglia | 2,32 M di istruzioni (0.1: 2,14 M; in più la striscia e la lettura degli indicatori) | come sopra |
| rilascio di un pulsante / interruttore | 4 invii del solo widget (es. 395×60) in 150 ms | ~1,3 ms di bus ciascuno |
| barra di scorrimento che svanisce | 3 invii di 4×177 px | ~4 ms di bus ciascuno (177 righe) |
| scatto del minuto | barra 62×40 px + ora della home 131×72 px (solo se la griglia è in vista) | ~1,5 + ~1,5 ms di bus |
| cronometro | 58×53 px dieci volte al secondo, le cifre grandi (262×78) una volta al secondo | ~2 ms di bus per decimo |
| scorrimento a tutto schermo | 854×440 px per fotogramma | ~35–40 ms a fotogramma (17 ms di bus): ~25 fps |

I numeri "attesi" sono stime, non misure: sul dispositivo vanno verificati
con `BRAINSHELL_TIMING=1` e `BUI_TRACE=1`.

## Limiti noti

- Mai eseguito sull'hardware vero: tempi, resa dei colori sul pannello e
  comportamento del touch resistivo sono da verificare lì.
- Backend DRM: vedi *Senza X* → *Da verificare sul Brain*. Senza X le app
  esterne sono una alla volta, a tutto schermo, senza linguette nella barra.
- La batteria si aggiorna ogni 30 s (come da contratto): collegare
  l'alimentazione si vede con quel ritardo.
- Le linguette che non entrano nella barra si riassumono in `+N` ma non c'è
  un elenco per raggiungerle.
- Il toast ha il bordo non sfumato (è sagomato con Shape) e i popup modali
  mostrano sotto di sé lo schermo com'era all'apertura.
- Con più di 64 widget in un pannello quelli in eccesso vengono ignorati (con
  un avviso su stderr): la griglia mostra quindi al massimo ~58 riquadri.
- Animazioni e tempi sono verificati solo sotto Xvfb (fotogrammi catturati a
  metà, rettangoli inviati, istruzioni): sul Brain vanno guardati dal vero. Se
  la transizione delle viste non convince, "Ridotte" la toglie.
- Nel modo modifica i riquadri non scivolano al nuovo posto (saltano); i
  riquadri finti di prova non si riordinano.
- La ricerca guarda solo l'inizio delle parole e solo lettere ASCII e cifre.
- Terminale: una sola sessione; niente selezione, appunti, incolla, mouse
  (le richieste dei programmi sono ignorate), corsivo e lampeggio; i
  caratteri combinanti sono ignorati e quelli senza glifo (CJK, emoji) sono
  rettangoli; cambiando dimensione le righe non si riavvolgono (si
  troncano); Ctrl+Spazio (NUL) solo col Ctrl della barra.
- Le miniature della Galleria già in cache tengono, sotto le parti
  trasparenti, il colore del tema con cui sono state create.
- Lo spegnimento vero dello schermo aspetta il verbo `screen` in `brain-ctl`
  (vedi *Contratto col sistema*); il default è "attenua dopo 2 minuti, non
  spegnere mai".
- Sfondo dell'utente: solo RGB565 grezzo della misura esatta dello schermo
  (nessuna decodifica o scalatura all'avvio, per scelta).
- Galleria: i JPEG progressivi e i PNG oltre 4 MP non si aprono (andrebbero
  decodificati per intero in memoria); niente zoom né rotazione manuale; le
  GIF mostrano il primo fotogramma. I tempi sul Brain sono stimati (una foto
  da 12 MP: ~0,5–1 s per la miniatura, 2–3 s a tutto schermo), non misurati.
- Esplora: niente copia/sposta/nuova cartella; la rinomina richiede la
  tastiera fisica; ordinamento solo per nome; i nomi con caratteri fuori da
  Latin-1 si vedono con "?".
- Solo scorrimento verticale.
- Tastiera: caratteri Latin-1 (`XLookupString`) più `€` e keysym Unicode, nessun metodo di input.

## Senza X: backend DRM, ingresso evdev, X su richiesta

Fino alla 0.2 la catena era kernel → Xorg (12 MB, ~3,5 s per partire) →
JWM (13 MB) → brainshell (4 MB), e ogni fotogramma della shell veniva
copiato due volte dentro X (immagine XShm → ShadowFB → dumb buffer). Ora
`brainshell` è l'unico processo grafico: apre `/dev/dri/card0`, disegna,
legge `/dev/input/event*`. X serve solo alle poche app esterne e parte per
loro.

### Disegno

- **Una superficie, finestre come livelli** (`bui_comp.c`). Home (desktop),
  barra (dock) e popup restano `bui_win` con la loro tela RGB565 in RAM
  normale, esattamente come con X: le viste e la shell non cambiano. Il
  compositore tiene l'ordine (desktop < normali < dock < popup), chiede alle
  finestre i rettangoli sporchi (`paint`, come prima) e copia **una volta**
  i soli pixel cambiati nel dumb buffer, partendo dalla finestra più alta
  che copre tutto il rettangolo (i livelli coperti non si copiano).
- **Dumb buffer CMA write-combined: solo scritture.** Non si legge mai dallo
  schermo: il fondo dei popup (dialoghi scuriti, pannello rapido) si
  ricompone dalle tele delle finestre sotto.
- **DIRTYFB a fasce di righe.** Il driver `brain` unisce tutte le clip di
  una chiamata in un solo rettangolo e invia righe intere: i rettangoli sono
  raccolti in fasce, le fasce più vicine di 12 righe si fondono, le altre
  partono con chiamate separate (barra in alto e toast in basso non mandano
  tutto ciò che sta in mezzo). Il primo fotogramma, già composto, va con
  `SETCRTC` (che invia l'intero schermo): niente lampi né schermo nero.
- Stesso passo di prima: un fotogramma ogni ~30 ms durante animazioni e
  trascinamenti, il primo dopo una pausa subito; a riposo nessun risveglio.
- Il modo DRM dice l'orientamento (854×480, oppure 480×854 col pannello
  ruotato dal device tree). Solo ioctl grezzi di `<drm/drm.h>`, niente libdrm.

Cosa faceva X e chi lo fa ora:

| con X11 | senza X |
|---|---|
| finestre dock/desktop, `_NET_WM_STRUT` | livelli del compositore; la home sa già che la barra è in alto |
| popup *override-redirect* + `XShmGetImage` del fondo | livello in cima; fondo ricomposto dalle tele (+ scurito con dithering) |
| grab di puntatore e tastiera dei popup modali | il popup modale più alto riceve tocchi (anche fuori di sé) e tasti; come `XGrabPointer`, il resto del tocco in corso passa a lui |
| fuoco della tastiera sulla home (`focus_guard`) | i tasti vanno al popup modale più alto, altrimenti alla home |
| toast sagomato con Shape | angoli trasparenti nel compositore (stessa maschera `bui_corner_inset`) |
| elenco `_NET_CLIENT_LIST`, linguette | nessuna finestra esterna insieme alla shell: l'app esterna ha tutto lo schermo |
| inattività: `IDLETIME` di SYNC, finestra InputOnly a schermo spento | ora dell'ultimo evento evdev, un solo timer; il primo tocco a schermo spento riaccende ed è scartato fino al rilascio (`idle_ev.c`) |
| attenuare/spegnere | invariato: `brain-ctl brightness N`, `brain-ctl screen off/on` |

### Ingresso (evdev)

- Dispositivi trovati per nome in `/sys/class/input`: tocco `mxs-lradc-ts`,
  tastiere `brain-kbd-gpio` e `brain-kbd-i2c`; se non ce n'è nessuno,
  qualunque dispositivo che sembri un tocco (ABS_X/Y + BTN_TOUCH o
  BTN_LEFT) o una tastiera. Tempi su `CLOCK_MONOTONIC` (`EVIOCSCLOCKID`).
  **`EVIOCGRAB`** su tutti: i tasti non arrivano anche alla console del
  kernel (il getty su tty1).
- **Tocco**: calibrazione dalla riga `Option "Calibration" "181 3986 3986
  255"` di `/etc/X11/xorg.conf.d/99-calibrator.conf` (minx maxx miny maxy,
  y invertita), rotazione da `/etc/brain-rotation` con le stesse matrici di
  `os/brain-rotate`, tutto in virgola fissa 16.16. Filtro per il resistivo:
  mediana degli ultimi 3 campioni (uno spike non sposta il punto), zona
  morta di 2 px, di una raffica si invia solo l'ultimo movimento, il
  rilascio avviene nell'ultimo punto inviato. `SYN_DROPPED` gestito.
- **Tastiera**: mappa US con Shift e Bloc Maiusc; con Ctrl i tasti di
  carattere non producono testo ma il codice di controllo (come
  `XLookupString`: Ctrl+C = 0x03, Ctrl+[ = ESC; Ctrl+Spazio, cioè NUL, non
  arriva), che le viste di testo ignorano e il Terminale manda al programma;
  la ripetizione del kernel è ignorata e la shell ripete da sé (500 ms, poi
  25/s). Dei cinque tasti del dizionario (PagSu, PagGiù, Tab, Ins, Canc)
  arriva anche il rilascio, `BUI_K_RELEASED`, che vede solo l'aggancio dei
  tasti della shell (`keys.c`).
- **Accentate senza tasti morti, AltGr o composizione**: Alt + tasto (sul
  Brain Alt è il tasto *Onsei*; le cifre si ottengono col tasto *Simboli*).
  Con Shift la maiuscola. Il carattere arriva senza il modificatore Alt,
  quindi l'editor delle note lo scrive.

| tasti | carattere | | tasti | carattere |
|---|---|---|---|---|
| Alt+a | à (À) | | Alt+c | ç (Ç) |
| Alt+e | è (È) | | Alt+4 | € |
| Alt+r | é (É) | | Alt+0 | ° |
| Alt+i | ì (Ì) | | Alt+- | — |
| Alt+o | ò (Ò) | | Alt+, | « |
| Alt+u | ù (Ù) | | Alt+. | » |

### App X esterne su richiesta (`xapp.c`)

Un riquadro con un programma esterno, un video dalla vista Video o da
Esplora, il terminale st di ripiego (pressione lunga nel Terminale → "Apri in
X", oppure `BRAINSHELL_TERMINAL=x11`): `bs_launch()` passa a
`bs_xapp_launch()`. Una sola app alla
volta, a tutto schermo:

1. `STARTING`: sale il toast "Apertura di …"; dopo 250 ms
2. la shell **lascia il master DRM** (`DROP_MASTER`) e **libera
   l'ingresso** (via `EVIOCGRAB`; i descrittori restano aperti): l'ultimo
   fotogramma, col toast, resta sul pannello finché X non mette il suo;
3. `RUNNING`: esegue `$BRAINSHELL_XORG ARGV…`, di default `sudo -n
   /usr/local/sbin/brain-xapp`, cioè `xinit brain-xapp-client ARGV -- Xorg
   :0 vt2 -config brain-run.conf -nolisten tcp -noreset -novtswitch -s 0
   -dpms -nocursor -br` (la stessa configurazione di `brain-ui`, generata da
   `brain-xconf`; nessun gestore di finestre: brainplay si apre a tutto
   schermo da sé, a `st` la shell aggiunge `-g COLONNExRIGHE` per riempire
   lo schermo);
4. quando quel processo finisce (l'app è uscita e xinit ha chiuso X):
   `RETURNING`, `SET_MASTER` (permesso senza root allo stesso descrittore
   che era master; se X non l'ha ancora lasciato si riprova ogni 100 ms),
   `SETCRTC` del proprio framebuffer, cattura dei dispositivi, modificatori
   riletti con `EVIOCGKEY`, tutto lo schermo ricomposto dalle tele (nessun
   ridisegno delle viste). `IDLE`.

**Come si torna alla shell**: uscendo dall'app (`exit` nel terminale, × o
fine del video). In più, dato che i dispositivi restano aperti senza cattura
(`brain-xconf` mette `Option "GrabDevice" "false"`, che è anche il default
del driver evdev di X), la shell vede ancora i tasti: il tasto **Home**
(`KEY_HOMEPAGE`), **Ctrl+Alt+Esc** (sul Brain: *PagSu*/Ctrl + *Onsei* +
*Go Back*) o una pressione lunga del tasto di accensione mandano SIGTERM al
comando (sudo lo inoltra a xinit, che chiude app e X).
Mentre l'app gira l'inattività continua a contare (gli eventi arrivano ai
filtri con `suspended = 1`): lo schermo si attenua e si spegne anche dentro
il terminale, e il primo tocco lo riaccende (ma arriva anche all'app).
Se il comando esce con 127, o con un errore prima di "ready" entro 5 s:
toast "Impossibile avviare …". Se la shell termina mentre X è su, prima
chiude X (altrimenti la shell riavviata non avrebbe il master).

### Video

`brainplay` senza finestra X non decodifica il video (`--no-window` = solo
audio: `win_open()` è obbligatoria per l'uscita PXP, `vo_pxp.c` usa
posizione e visibilità della finestra). Quindi **un video è un'app X
esterna** come il Terminale (scelta fatta: nessuna modifica a `player/`).
La musica non cambia: `brainplay --slave --no-window` gira senza X.

La variante migliore (la shell disegna la barra dei comandi, brainplay
scrive il video nelle righe riservate) richiede una piccola modifica al
lettore, che descrivo e non faccio:

- `brainplay --slave --overlay Y,H FILE`: niente `win_open()`;
  `opt.disp_w/disp_h` presi da `PXP_IOC_SCREEN` (larghezza del pannello, H
  righe); `vo_pxp` in modo schermo con `out_y = Y`, `out_height = H` e
  "finestra sempre visibile e a tutta larghezza" (oggi lo decide
  `VisibilityNotify`); nessuna `ui_new()`, i comandi solo dal protocollo
  slave (che c'è già: `pause`, `seek`, `volume`, `state`, `pos`);
- un comando slave `overlay on|off` per sospendere le scritture quando la
  shell mostra un popup sopra il video, e `redraw` per ripetere l'ultimo
  fotogramma;
- lato shell: una vista "lettore" che riserva le righe e un "buco" nel
  compositore (`bui_comp`: non copiare né inviare quelle righe mentre il
  video è attivo, altrimenti il dumb buffer, che è proprio il buffer in cui
  scrive il PXP, verrebbe sovrascritto).

### Avvio senza X (`os/early/`)

| file | |
|---|---|
| `brain-preinit` (modificato) | avvia `brain-shell-session` (se c'è, c'è `/usr/local/bin/brainshell` e non c'è `/etc/brain-ui-x11`), altrimenti `brain-ui` come prima; aspetta (max 8 s) `/tmp/xdg-user/brainshell-ready` **o** il socket di X, poi `exec /sbin/init` |
| `brain-shell-session` (nuovo) | root: permessi di card0/pxp/fb0 (video), snd (audio), `/dev/input/event*` (input: senza udev sono 0600 root), console non oscurabile; poi in ciclo `brainshell` come `user` (`setpriv`) con `HOME`, `PATH`, `XDG_RUNTIME_DIR=/tmp/xdg-user`, `BRAIN_CTL`, `BRAINSHELL_BACKEND=drm`, `BRAINSHELL_READY_FILE`; la riavvia se esce; **ripiega su X** (`/run/brain-ui-x11` + `exec brain-ui`) se esce con 75 (DRM non disponibile) o cade 3 volte di fila entro 10 s; prepara `brain-run.conf` in sottofondo 3 s dopo |
| `brain-xconf` (nuovo) | scrive `/etc/X11/brain-run.conf` esattamente come `brain-ui` |
| `brain-xapp` (nuovo) | root via sudo: X per un'app (vedi sopra); rifiuta l'argomento `--` (passerebbe opzioni al server X che gira come root) |
| `brain-xapp-client` (nuovo) | client di xinit: `xhost` per l'utente, "ready" sullo stdout, poi l'app come utente |
| `brain-xapp.sudoers` (nuovo) | `user ALL=(root) NOPASSWD: /usr/local/sbin/brain-xapp` in `/etc/sudoers.d/brain-xapp` |

`brain-ui`, `brain-session`, `~/.xinitrc` non cambiano: la sessione X resta
identica (sotto X `brainshell` esegue `brainshell-x11` che sta accanto).

### Due binari invece di dlopen

`brainshell` (drm + mem, NEEDED solo `libc.so.6`) e `brainshell-x11` (+ x11,
NEEDED `libX11.so.6 libXext.so.6 libc.so.6`) dagli stessi sorgenti, col solo
`-DBUI_HAVE_X11` e tre file in più (`bui_x.c`, `wm.c`, `idle.c`). Con dlopen
tutte le ~90 funzioni Xlib/Xext/SYNC/Shape usate da `bui_x.c`, `wm.c` e
`idle.c` andrebbero chiamate attraverso puntatori caricati a mano (e le
macro di Xlib riscritte): molto codice in più da tenere allineato, per
risparmiare 600 kB di disco. Così il binario del Brain non carica né
inizializza Xlib, e chi lo lancia sotto X (`~/.xinitrc` invariato) finisce
comunque nella variante giusta.

### Installazione e prova sul Brain

```sh
# dal Mac
scp os/brainshell os/brainshell-x11 os/early/brain-preinit os/early/brain-shell-session \
    os/early/brain-xconf os/early/brain-xapp os/early/brain-xapp-client \
    os/early/brain-xapp.sudoers brain:/tmp/
ssh brain
cd /tmp
sudo install -m 755 brainshell brainshell-x11 /usr/local/bin/
sudo install -m 755 brain-preinit brain-shell-session brain-xconf brain-xapp brain-xapp-client /usr/local/sbin/
sudo install -m 440 brain-xapp.sudoers /etc/sudoers.d/brain-xapp && sudo visudo -c
sudo /usr/local/sbin/brain-xconf          # brain-run.conf anche prima del primo avvio
sudo reboot
```

Dopo il riavvio: `cat /tmp/brain-ui.log` (orari di `brain-shell-session`,
"primo fotogramma dopo N ms", eventuali errori di brainshell),
`/tmp/brain-xapp.log` per le sessioni X. Prove a mano: tocco ovunque
(calibrazione, anche nei due verticali), scorrimento, tastiera (accentate
con Onsei), Terminale integrato (vedi *Terminale* → *Da verificare sul
Brain*), il ripiego X (pressione lunga nel Terminale → Apri in X → `exit` →
di nuovo la shell; ancora → Ctrl+Alt+Esc), un video,
attenuazione/spegnimento (anche dentro un'app X), schermata dal pannello
rapido, i tasti del dizionario.

Per provare senza riavviare (da SSH, con la sessione X attuale ferma):
`sudo touch /run/brain-nodesktop; sudo pkill xinit` poi
`sudo /usr/local/sbin/brain-shell-session &`.

**Tornare a X**: `sudo touch /etc/brain-ui-x11` e riavviare (si toglie con
`sudo rm /etc/brain-ui-x11`). Se brainshell non riesce ad aprire il DRM o
cade di continuo il ripiego su X è automatico (`/run/brain-ui-x11` dice
perché). Senza nessuna interfaccia: `/etc/brain-noearlyui` come prima.

### Prove senza X e senza pannello

- **Backend `mem`** (`bui_mem.c`): lo stesso compositore del DRM, la
  superficie è un buffer in memoria, l'ingresso uno script (`tap`, `press`,
  `move`, `release`, `drag`, `key` — passa dalla mappa evdev vera —, `type`
  con le accentate, `hold` per la ripetizione, `wait`, `shot` in PNG,
  `expect` sullo stato della shell, `pixel`, `rss`, `stats` fotogrammi e
  righe inviate, `sh`). `test/mem.sh [-d|-q] [GEOMETRIA [SCENE]]` esegue le
  scene `test/mem/*.sh`; schermate in `shots/mem/GEOMETRIA/`:

| scena | cosa prova |
|---|---|
| `home` | home, riquadro premuto, una vista che scorre, ritorno; righe inviate, zero fotogrammi a riposo, RSS |
| `views` | ogni vista aperta e chiusa (freccia, Esc, Backspace) |
| `quick` | pannello rapido, cursore → `brain-ctl brightness`, dialogo sopra il pannello (Annulla torna al pannello), calendario |
| `scroll` | griglia con 14 riquadri in più: inerzia, tocco che la ferma, Impostazioni |
| `dialog` | dialoghi modali, tocco fuori = annulla, Tab/Invio conferma → `brain-ctl rotate`, schermata + toast |
| `keys` | ricerca nella griglia, calcolatrice da tastiera, frecce con ripetizione, nota scritta con accentate e salvata in UTF-8 |
| `xapp` | finto Xorg (`test/fake-xorg`): X parte sempre a schermo già ceduto, tocchi ignorati durante l'app, Ctrl+Alt+Esc → SIGTERM → ritorno, app che esce da sola, comando che fallisce (toast), doppio avvio rifiutato, video da Video, geometria di st |
| `idle` | attenuazione, spegnimento, primo tocco che sveglia soltanto, spegnimento e risveglio durante un'app X |
| `term` | Terminale con un vero `/bin/sh`: comandi, colori, UTF-8, storico, Ctrl+C (tasto e barra), Esc, tasti del dizionario che restano tasti, sospensione trattenuta, Home e ritorno, `exit` e Riavvia (vedi *Terminale*) |
| `termdraw` | disegno per differenza del Terminale identico al ridisegno completo con htop, vim, apt, vttest, less e 300 righe |
| `dictkeys` | tasti del dizionario: app recenti, impostazioni rapide, schermata, Musica, Note e nota nuova, Onsei per luminosità e volume, Impostazioni → Tasti (vedi *Tasti del dizionario*) |

`xapp`, `idle`, `autosleep` e `homekey` (e le scene x11) usano `BRAINSHELL_TERMINAL=x11`:
il riquadro Terminale vi fa da app X esterna, come prima della 0.4.

- **`test/evdev_test.c`** (`make tests`): flussi di `struct input_event`
  come li manda il kernel: angoli e centro nelle tre rotazioni, valori fuori
  calibrazione, rumore entro la zona morta, spike a metà trascinamento,
  raffiche, `SYN_DROPPED`, BTN_LEFT; tutta la mappa ASCII e le accentate,
  Shift/Bloc Maiusc/Ctrl/Alt, Ctrl+lettera → codici di controllo,
  ripetizione (500 ms, 25/s, niente raffiche); i tasti del dizionario
  (pressione, ripetizione, rilascio, con Onsei), Ins.
- **`test/vt_test.c`** (`make tests`): l'emulatore del terminale, vedi
  *Terminale* → *Prove*.
- `test/mem.sh -q`: il binario armel sotto `qemu-arm -cpu arm926`.
- `test/cg-mem.sh` (immagine con valgrind): istruzioni da `main()` al primo
  fotogramma, mem contro x11.
- Le scene x11 (`test/all.sh`, `test/all-media.sh`) girano con
  `brainshell-x11`.

### Misure (PC di sviluppo, aarch64)

| | mem/drm | x11 |
|---|---|---|
| binario armel senza simboli | 749 440 byte (testo 667 kB; 0.3: 610 480 / 522 kB: Terminale ~36 kB di codice + 85 kB di font mono, tasti ~11 kB), NEEDED `libc.so.6` | 749 712 byte (testo 688 kB), NEEDED `libX11.so.6 libXext.so.6 libc.so.6` |
| `main()` → primo fotogramma (callgrind, 854×480) | 10,09 M istruzioni | 10,41 M (+ il lavoro del server X) |
| idem 480×854 | 10,39 M | 10,70 M |
| RSS a riposo dopo home/Impostazioni (backend mem) | 4,6 MB (2,8 anonima: tele 0,8 MB + superficie 0,8 MB, che sul Brain è il dumb buffer) | 4,1 MB + Xorg 12 MB + JWM 13 MB sul Brain |
| a riposo | 0 fotogrammi in 1,5 s | come prima |

I file hanno quasi la stessa dimensione perché il linker allinea i segmenti
a 64 kB; conta il testo.

### Da verificare sul Brain

Nulla di questo backend è stato eseguito sull'hardware: tempi reali di
avvio e di ingresso/uscita da X, comportamento di `SET_MASTER` come utente
dopo l'uscita di X, che fbcon non disegni sopra la shell, che il driver evdev
di X non catturi i dispositivi (altrimenti Ctrl+Alt+Esc e l'inattività
durante l'app non funzionano: resta l'uscita dall'app), il filtro del
resistivo, la cella di `st` (`BRAINSHELL_ST_CELL`), `sudo -n` senza
password per `brain-xapp`.

## Energia (`power.c`, `idle_ev.c`, `batt_est.c`)

Solo col backend drm (sotto X il tasto di accensione non arriva alla shell e
`idle.c` non sospende). Stima, avvisi e risparmio energetico valgono anche
con x11.

### Tasto di accensione

Il kernel lo espone come dispositivo evdev "Brain Power Button" (KEY_POWER,
anche sorgente di risveglio): la shell lo apre e lo cattura con gli altri
(logind ha comunque `HandlePowerKey=ignore`). Pressione e rilascio arrivano
come `BUI_K_POWER` / `BUI_K_POWER_UP` a un filtro d'ingresso di `power.c`,
prima di qualunque finestra, mai ripetuti.

| pressione | effetto |
|---|---|
| con lo schermo spento o attenuato | lo riaccende soltanto (pressione e rilascio scartati) |
| breve (< 1 s), a batteria | sospensione |
| breve, col cavo USB (`brain-usb/online` = 1) | solo schermo spento: la ricarica è una macchina a stati del kernel che deve continuare |
| fra 1 e 1,5 s | niente |
| lunga (≥ 1,5 s, mentre è ancora premuto) | menu a tutto schermo **Spegni / Riavvia / Sospendi / Annulla** (fuoco su Annulla; un tocco sul fondo o Esc annulla). Durante un'app X: chiude l'app e torna alla shell |

Lo stesso menu si apre da Impostazioni rapide → Spegni. "Sospendi" col cavo
spegne solo lo schermo (con un avviso).

### Sospensione e risveglio

`brain-ctl suspend [N]` gira in un processo figlio e ritorna solo quando il
sistema si è risvegliato (tasto, cavo USB collegato, sveglia RTC). Se il
timer dell'Orologio è in corso, N = i secondi che mancano: il Brain si
sveglia in tempo per farlo suonare. Mentre il comando è in corso nessun
ingresso arriva a nessuno. Al ritorno:

- gli eventi evdev con un tempo precedente al risveglio (il tasto che ha
  svegliato, tocchi accodati) vengono scartati (`bui_input_ignore_before`);
- le animazioni vengono portate a termine (`bui_anim_finish_all`);
- inattività azzerata, schermo acceso, batteria riletta, ora e data
  aggiornate (`BS_EV_MINUTE`), tutte le finestre ridisegnate e reinviate al
  pannello (`bui_redraw_all`);
- se `brain-ctl` fallisce subito: toast "Sospensione non riuscita".

`bui_now()` usa `CLOCK_BOOTTIME` (anche i tempi degli eventi evdev, con
`EVIOCSCLOCKID`): conta il tempo passato in sospensione, quindi timer e
scadenze superati durante il sonno (il timer dell'Orologio, il minuto
della barra) scattano subito al risveglio.

### Sospensione automatica

Impostazioni → Schermo → **Sospendi dopo** (Mai / 1 / 5 / 15 / 30 min,
default 5; `sleep_after=` in secondi). Si conta da quando lo schermo si è
spento, oppure dall'ultimo tocco se lo spegnimento è "Mai". Solo a batteria
e mai con la musica che suona, un'app X aperta, il menu aperto o un
programma del Terminale che ha scritto negli ultimi 30 s (vedi *Terminale* →
*Energia*): finché non si può si ricontrolla ogni 30 s (es. il cavo staccato
a schermo già spento).

### Batteria

- Striscia della home: la colonna dei valori contiene "100%"; in carica o a
  carica completa un fulmine fra la barra e il valore.
- **Autonomia stimata** dalla pendenza della carica (minimi quadrati sugli
  ultimi 60 minuti, un campione al minuto, `batt_est.c`): compare solo con
  almeno 20 minuti di dati a batteria, nel pannello rapido ("87% · 3 h 20
  min") e in Impostazioni → Batteria ("Calcolo in corso…" prima). Il cavo
  azzera i dati; il tempo in sospensione conta (bui_now include il sonno).
- 15%: toast. 5%: dialogo a tutto schermo "Batteria quasi scarica" →
  "Collega il cavo…" (riaccende lo schermo). **3% o capacità 0 a batteria:
  spegnimento ordinato** con `brain-ctl poweroff` (preferenze salvate prima;
  il kernel spegne comunque a 3,3 V). Una sospensione lì farebbe perdere
  tutto quando la batteria finisce nel sonno.

### Risparmio energetico

Interruttore in fondo alle Impostazioni rapide (e in Impostazioni →
Schermo), `power_saver=`: animazioni al massimo "Ridotte", luminosità al
massimo 40% (il cursore torna a 40 al rilascio), schermo attenuato dopo
15 s e spento dopo 30 s, sospensione dopo 1 min. Spento, tornano le
preferenze di prima.

### Prove

`test/mem.sh` comprende le scene `power` (pressione breve e lunga col finto
`brain-ctl suspend` che dorme 2 s, tasto accodato prima del risveglio,
Sospendi e Spegni dal menu, cavo USB, sveglia del timer), `autosleep`
(musica, app X, cavo, ricontrollo dopo il distacco), `battery` (100%, in
carica, 15/5/3%, risparmio energetico, preferenze salvate) e `homekey` (Home
da una vista con nota salvata, da un dialogo sopra il pannello rapido, dalla
ricerca, a schermo spento, durante un'app X). Il backend mem
ha `hold Power MS` e `stalekey SPEC`. `make tests` compila anche
`test/batt_test.c` (stima dell'autonomia).

### Da verificare sul Brain

Nome e codici del dispositivo "Brain Power Button", che la cattura non
impedisca il risveglio, che `brain-ctl suspend` ritorni davvero al risveglio
e che il pannello vada ridisegnato del tutto (lo si fa sempre),
`CLOCK_BOOTTIME` per `EVIOCSCLOCKID` (kernel 6.1: sì), la sveglia RTC, la
pendenza reale della carica (capacità a scalini del gauge del Brain), cosa
succede a un'app X durante una sospensione (X non sa del risveglio).

## Licenze dei materiali incorporati

- `third_party/stb_image.h` (v2.30): pubblico dominio / MIT.
- `third_party/tjpgd.c`, `tjpgd.h`, `tjpgdcnf.h` (TJpgDec R0.03, ChaN 2021):
  licenza libera di ChaN (nessuna restrizione, va conservato l'avviso di
  copyright in testa ai sorgenti). `tjpgdcnf.h` è modificato: `JD_SZBUF`
  4096, `JD_FASTDECODE` 2.

- DejaVu Sans (glifi in `gen/bui_fonts.c`): licenza Bitstream Vera / DejaVu.
- Material Icons Round (`tools/material-icons/`, glifi in `gen/bui_icons.c`):
  Apache-2.0, vedi `tools/material-icons/LICENSE`.
- `tools/stb_truetype.h`: pubblico dominio / MIT (usato solo dal generatore).
