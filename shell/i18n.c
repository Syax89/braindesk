// SPDX-License-Identifier: MIT
/*
 * i18n.c - lingue dell'interfaccia: italiano (testo del codice), inglese,
 * giapponese.
 *
 * Il codice resta scritto in italiano: ogni testo che il toolkit disegna o
 * misura passa da bs_tr() (aggancio bui_set_translator), che cerca la frase
 * italiana intera nella tabella qui sotto e restituisce la traduzione. Le
 * frasi composte con snprintf passano il formato da T() prima di comporre.
 * Una frase che manca dalla tabella resta in italiano. Cambiare lingua
 * riavvia l'interfaccia (main.c): tutto si ricostruisce con i testi nuovi.
 *
 * I glifi giapponesi sono nei font della shell (tools/bake.c li prende da
 * IPAex Gothic): solo i caratteri usati in questo file.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "shell.h"

/* { italiano, inglese, giapponese } */
static const char *const tr[][3] = {
	/* app */
	{ "Impostazioni", "Settings", "設定" },
	{ "Orologio", "Clock", "時計" },
	{ "Calcolatrice", "Calculator", "電卓" },
	{ "Terminale", "Terminal", "ターミナル" },
	{ "Esplora", "Files", "ファイル" },
	{ "Galleria", "Gallery", "ギャラリー" },
	{ "Musica", "Music", "音楽" },
	{ "Video", "Video", "ビデオ" },
	{ "Note", "Notes", "メモ" },
	{ "Campionario", "Showcase", "見本" },
	{ "Tasti", "Keys", "キー" },
	{ "Data e ora", "Date and time", "日付と時刻" },
	{ "Giochi", "Games", "ゲーム" },
	{ "Dizionario", "Dictionary", "辞書" },

	/* barra, impostazioni rapide, avvisi */
	{ "Rotazione dello schermo: riavvio in corso…", "Rotating the screen: restarting…", "画面を回転しています: 再起動中…" },
	{ "Ruotare lo schermo?", "Rotate the screen?", "画面を回転しますか?" },
	{ "Per cambiare orientamento il dispositivo deve riavviarsi. Salva il lavoro aperto prima di continuare.",
	  "To change orientation the device must restart. Save your open work before continuing.",
	  "向きを変えるには再起動が必要です。続ける前に作業を保存してください。" },
	{ "Annulla", "Cancel", "キャンセル" },
	{ "Ruota e riavvia", "Rotate and restart", "回転して再起動" },
	{ "Schermata non riuscita", "Screenshot failed", "スクリーンショットに失敗しました" },
	{ "Schermata salvata: %s", "Screenshot saved: %s", "スクリーンショットを保存: %s" },
	{ "Ruota", "Rotate", "回転" },
	{ "Schermata", "Screenshot", "スクショ" },
	{ "Spegni", "Power off", "電源オフ" },
	{ "Risparmio energetico", "Power saver", "省電力" },
	{ "ULTIMI AVVISI", "RECENT NOTIFICATIONS", "最近の通知" },
	{ "In riproduzione", "Playing", "再生中" },
	{ "in riproduzione", "playing", "再生中" },
	{ "In pausa", "Paused", "一時停止中" },
	{ "Cerca: %s", "Search: %s", "検索: %s" },
	{ "Invio apre · Esc annulla", "Enter opens · Esc cancels", "Enterで開く · Escでキャンセル" },
	{ "Nessuna app · Esc annulla", "No apps · Esc cancels", "アプリなし · Escでキャンセル" },
	{ "Trascina per riordinare · tocca il segno per nascondere o mostrare",
	  "Drag to reorder · tap the mark to hide or show", "ドラッグで並べ替え · 印をタップで表示/非表示" },
	{ "Trascina · il segno nasconde o mostra", "Drag · the mark hides or shows", "ドラッグ · 印で表示/非表示" },
	{ "Modifica della home", "Edit home", "ホームを編集" },
	{ "Fine", "Done", "完了" },
	{ "Apertura di %s…", "Opening %s…", "%sを開いています…" },
	{ "Apertura di %.60s…", "Opening %.60s…", "%.60sを開いています…" },
	{ "Disco", "Disk", "容量" },

	/* temi */
	{ "Nero", "Black", "ブラック" },
	{ "Chiaro", "Light", "ライト" },
	{ "Brace", "Ember", "Ember" },

	/* tasti del dizionario */
	{ "Tasto normale", "Normal key", "通常のキー" },
	{ "Nessuna azione", "No action", "動作なし" },
	{ "App recenti", "Recent apps", "最近のアプリ" },
	{ "Impostazioni rapide", "Quick settings", "クイック設定" },
	{ "Risparmio energetico sì/no", "Power saver on/off", "省電力 オン/オフ" },
	{ "Musica: riproduci/pausa", "Music: play/pause", "音楽: 再生/一時停止" },
	{ "Note (tenuto: nuova nota)", "Notes (hold: new note)", "メモ (長押し: 新規メモ)" },
	{ "Apri un'app", "Open an app", "アプリを開く" },
	{ "Apri %s", "Open %s", "%sを開く" },
	{ "Tasto normale (%s)", "Normal key (%s)", "通常のキー (%s)" },
	{ "Kokugo: la successiva · Invio: apri · Esc: annulla", "Kokugo: next · Enter: open · Esc: cancel",
	  "国語: 次へ · Enter: 開く · Esc: キャンセル" },
	{ "Nessuna app recente", "No recent apps", "最近のアプリはありません" },
	{ "sessione attiva", "session active", "セッション実行中" },
	{ "Luminosità non disponibile", "Brightness unavailable", "明るさを変更できません" },
	{ "Luminosità", "Brightness", "明るさ" },
	{ "Volume non disponibile", "Volume unavailable", "音量を変更できません" },
	{ "Volume", "Volume", "音量" },
	{ "Audio disattivato", "Sound off", "消音" },
	{ "Risparmio energetico attivo", "Power saver on", "省電力オン" },
	{ "Risparmio energetico disattivato", "Power saver off", "省電力オフ" },
	{ "Musica in pausa", "Music paused", "音楽を一時停止" },
	{ "Musica: riproduzione", "Music: playing", "音楽: 再生中" },
	{ "App non trovata", "App not found", "アプリが見つかりません" },
	{ "Tasti riportati alle scelte iniziali", "Keys reset to defaults", "キーを初期設定に戻しました" },
	{ "TASTI DEL DIZIONARIO", "DICTIONARY KEYS", "辞書キー" },
	{ "Guida ai tasti", "Key guide", "キーガイド" },
	{ "Scelte iniziali", "Defaults", "初期設定" },
	{ "%s: apri", "%s: open", "%s: 開く" },
	{ "App recenti: ogni pressione passa alla successiva; Invio, un tocco o il rilascio dopo averlo tenuto aprono quella scelta, Esc annulla.",
	  "Recent apps: each press moves to the next one; Enter, a tap or releasing after holding opens it, Esc cancels.",
	  "最近のアプリ: 押すたびに次へ。Enter、タップ、長押し後に離すと開きます。Escでキャンセル。" },
	{ "Impostazioni rapide (di nuovo per chiuderle).", "Quick settings (again to close them).",
	  "クイック設定 (もう一度押すと閉じます)。" },
	{ "Musica: pausa e ripresa del brano caricato; senza brano apre Musica.",
	  "Music: pause and resume the loaded track; with no track it opens Music.",
	  "音楽: 曲の一時停止と再開。曲がなければ音楽を開きます。" },
	{ "Note; tenuto premuto mezzo secondo apre una nota nuova.", "Notes; held for half a second it opens a new note.",
	  "メモ。0.5秒長押しで新しいメモ。" },
	{ "Schermata (salvata da brain-ctl, con un avviso).", "Screenshot (saved by brain-ctl, with a notice).",
	  "スクリーンショット (brain-ctlが保存し、通知します)。" },
	{ "Luminosità − / +.", "Brightness − / +.", "明るさ − / +。" },
	{ "Volume − / +.", "Volume − / +.", "音量 − / +。" },
	{ "Audio disattivato / riattivato.", "Sound off / on.", "消音 / 消音解除。" },
	{ "Mentre si scrive", "While typing", "入力中" },
	{ "Nel Terminale, nell'editor delle note e nei campi di testo i cinque tasti restano PagSu, PagGiù, Tab, Ins e Canc; anche nei dialoghi e nei pannelli. Con Maiusc o Ctrl restano sempre tasti normali.",
	  "In the Terminal, the note editor and text fields the five keys stay PgUp, PgDn, Tab, Ins and Del; also in dialogs and panels. With Shift or Ctrl they are always normal keys.",
	  "ターミナル、メモ編集、入力欄では5つのキーはPgUp、PgDn、Tab、Ins、Delのままです。ダイアログやパネルでも同じです。ShiftかCtrlと一緒なら常に通常のキーです。" },
	{ "Alla griglia da ovunque (anche dal Terminale; la sessione resta aperta).",
	  "Back to the grid from anywhere (also from the Terminal; the session stays open).",
	  "どこからでもホームへ (ターミナルからも。セッションは残ります)。" },
	{ "Indietro; nel Terminale è Esc per il programma.", "Back; in the Terminal it is Esc for the program.",
	  "戻る。ターミナルではプログラムへのEscです。" },
	{ "Ctrl (Ctrl+C, Ctrl+D... nel Terminale).", "Ctrl (Ctrl+C, Ctrl+D... in the Terminal).",
	  "Ctrl (ターミナルでCtrl+C、Ctrl+Dなど)。" },
	{ "Onsei + lettera", "Onsei + letter", "音声 + 文字" },
	{ "Accentate: a à, e è, r é, i ì, o ò, u ù, 4 €.", "Accents: a à, e è, r é, i ì, o ò, u ù, 4 €.",
	  "アクセント文字: a à、e è、r é、i ì、o ò、u ù、4 €。" },

	/* accensione e batteria */
	{ "Sospensione non riuscita", "Suspend failed", "スリープに失敗しました" },
	{ "In carica: si spegne solo lo schermo", "Charging: only the screen turns off", "充電中: 画面だけ消えます" },
	{ "Spegnimento in corso…", "Shutting down…", "電源を切っています…" },
	{ "Riavvio in corso…", "Restarting…", "再起動しています…" },
	{ "Spegnimento", "Power", "電源" },
	{ "Riavvia", "Restart", "再起動" },
	{ "Sospendi", "Sleep", "スリープ" },
	{ "Nessuna batteria", "No battery", "バッテリーなし" },
	{ "Batteria esaurita: spegnimento…", "Battery empty: shutting down…", "バッテリー切れ: 電源を切ります…" },
	{ "Batteria quasi scarica", "Battery low", "バッテリー残量わずか" },
	{ "Collega il cavo: carica al %d%%. Al 3%% il dispositivo si spegne da solo.",
	  "Plug in the cable: %d%% left. At 3%% the device turns itself off.",
	  "ケーブルを接続してください: 残り%d%%。3%%で自動的に電源が切れます。" },
	{ "Batteria al %d%%: collega l'alimentazione", "Battery at %d%%: plug in the charger",
	  "バッテリー残り%d%%: 充電してください" },
	{ "In carica", "Charging", "充電中" },
	{ "A batteria", "On battery", "バッテリー駆動" },
	{ "Carica completa", "Fully charged", "充電完了" },
	{ "Non in carica", "Not charging", "充電していません" },
	{ "Impossibile avviare %s", "Cannot start %s", "%sを起動できません" },

	/* calcolatrice, orologio */
	{ "Errore", "Error", "エラー" },
	{ "I risultati compariranno qui", "Results will appear here", "ここに結果が表示されます" },
	{ "Tempo scaduto", "Time's up", "時間です" },
	{ "Giro %d", "Lap %d", "ラップ %d" },
	{ "Timer terminato", "Timer finished", "タイマー終了" },
	{ "Cronometro", "Stopwatch", "ストップウォッチ" },
	{ "Timer", "Timer", "タイマー" },
	{ "Nessun giro registrato", "No laps recorded", "ラップはありません" },
	{ "Pausa", "Pause", "一時停止" },
	{ "Riprendi", "Resume", "再開" },
	{ "Avvia", "Start", "開始" },
	{ "Giro", "Lap", "ラップ" },
	{ "Azzera", "Reset", "リセット" },

	/* file */
	{ "Cartella", "Folder", "フォルダ" },
	{ "Immagine", "Image", "画像" },
	{ "Audio", "Audio", "オーディオ" },
	{ "File", "File", "ファイル" },
	{ "Home", "Home", "ホーム" },
	{ "Proprietà", "Properties", "プロパティ" },
	{ "Rinomina", "Rename", "名前を変更" },
	{ "Documenti", "Documents", "書類" },
	{ "Immagini", "Pictures", "ピクチャ" },
	{ "Caricamento…", "Loading…", "読み込み中…" },
	{ "La cartella non esiste più", "The folder no longer exists", "フォルダがありません" },
	{ "Cartella non accessibile", "Folder not accessible", "フォルダにアクセスできません" },
	{ "Cartella vuota", "Empty folder", "空のフォルダ" },
	{ "Cartella molto grande: mostrati i primi 4000 elementi", "Very large folder: showing the first 4000 items",
	  "大きなフォルダ: 最初の4000項目を表示" },
	{ "Tipo di file non supportato", "File type not supported", "未対応のファイル形式です" },
	{ "Impossibile leggere il file", "Cannot read the file", "ファイルを読めません" },
	{ "Memoria insufficiente", "Not enough memory", "メモリが足りません" },
	{ "il lettore multimediale", "the media player", "メディアプレーヤー" },
	{ "Percorso troppo lungo", "Path too long", "パスが長すぎます" },
	{ "Impossibile eliminare tutto il contenuto", "Could not delete all the contents", "一部を削除できませんでした" },
	{ "Elemento eliminato", "Item deleted", "削除しました" },
	{ "Elimina", "Delete", "削除" },
	{ "Eliminare la cartella?", "Delete the folder?", "フォルダを削除しますか?" },
	{ "«%.120s» e tutto il suo contenuto verranno eliminati definitivamente.",
	  "«%.120s» and all its contents will be deleted permanently.", "「%.120s」とその中身はすべて完全に削除されます。" },
	{ "Eliminare il file?", "Delete the file?", "ファイルを削除しますか?" },
	{ "«%.120s» verrà eliminato definitivamente.", "«%.120s» will be deleted permanently.",
	  "「%.120s」は完全に削除されます。" },
	{ "Collegamento a ", "Link to ", "リンク先: " },
	{ "Nome non valido", "Invalid name", "無効な名前です" },
	{ "Esiste già un elemento con questo nome", "An item with this name already exists", "同じ名前の項目があります" },
	{ "Impossibile rinominare", "Cannot rename", "名前を変更できません" },
	{ "File nascosti visibili", "Hidden files shown", "隠しファイルを表示" },
	{ "File nascosti non mostrati", "Hidden files not shown", "隠しファイルを非表示" },
	{ "Apri", "Open", "開く" },
	{ "Nome", "Name", "名前" },
	{ "Tipo", "Type", "種類" },
	{ "Dimensione", "Size", "サイズ" },
	{ "Modificato", "Modified", "変更日" },
	{ "Permessi", "Permissions", "アクセス権" },
	{ "Posizione", "Location", "場所" },
	{ "NUOVO NOME", "NEW NAME", "新しい名前" },
	{ "Scrivi con la tastiera, poi premi Invio.", "Type with the keyboard, then press Enter.",
	  "キーボードで入力してEnterを押してください。" },
	{ "Il file è vuoto", "The file is empty", "ファイルは空です" },
	{ "mostrato solo l'inizio", "only the beginning is shown", "先頭のみ表示" },
	{ "sola lettura", "read only", "読み取り専用" },

	/* galleria */
	{ "1 immagine", "1 image", "画像 1枚" },
	{ "%d immagini%s", "%d images%s", "画像 %d枚%s" },
	{ " (le prime)", " (the first ones)", " (先頭のみ)" },
	{ "Ricerca delle immagini…", "Looking for images…", "画像を探しています…" },
	{ "Nessuna immagine", "No images", "画像はありません" },
	{ "Le foto copiate nella cartella Immagini e le schermate compaiono qui.",
	  "Photos copied to the Pictures folder and screenshots appear here.",
	  "ピクチャフォルダにコピーした写真とスクリーンショットがここに表示されます。" },
	{ "%d di %d", "%d of %d", "%d / %d" },
	{ "Impossibile eliminare l'immagine", "Cannot delete the image", "画像を削除できません" },
	{ "Immagine eliminata", "Image deleted", "画像を削除しました" },
	{ "Eliminare l'immagine?", "Delete the image?", "画像を削除しますか?" },
	{ "«%s» verrà eliminata definitivamente.", "«%s» will be deleted permanently.", "「%s」は完全に削除されます。" },

	/* musica */
	{ "Nessun brano", "No track", "曲がありません" },
	{ "Tocca un brano per ascoltarlo", "Tap a track to play it", "曲をタップして再生" },
	{ "Riproduzione non riuscita", "Playback failed", "再生に失敗しました" },
	{ "Nessun brano nella cartella Musica", "No tracks in the Music folder", "音楽フォルダに曲がありません" },
	{ "Ricerca dei brani…", "Looking for tracks…", "曲を探しています…" },

	/* note */
	{ "Nota", "Note", "メモ" },
	{ "Nuova nota", "New note", "新規メモ" },
	{ "Salvata", "Saved", "保存済み" },
	{ "Modificata", "Modified", "変更あり" },
	{ "Nota non salvata", "Note not saved", "メモは保存されていません" },
	{ "Non è stato possibile salvare la nota. Uscire comunque? Le ultime modifiche andranno perse.",
	  "The note could not be saved. Leave anyway? The latest changes will be lost.",
	  "メモを保存できませんでした。このまま閉じますか? 最近の変更は失われます。" },
	{ "Resta", "Stay", "残る" },
	{ "Esci", "Leave", "閉じる" },
	{ "Impossibile aprire la nota", "Cannot open the note", "メモを開けません" },
	{ "Nota troppo grande per l'editor", "Note too large for the editor", "メモが大きすぎて編集できません" },
	{ "Impossibile eliminare la nota", "Cannot delete the note", "メモを削除できません" },
	{ "Nota eliminata", "Note deleted", "メモを削除しました" },
	{ "Eliminare la nota?", "Delete the note?", "メモを削除しますか?" },
	{ "La nota verrà eliminata definitivamente.", "The note will be deleted permanently.", "メモは完全に削除されます。" },
	{ "Senza titolo", "Untitled", "無題" },
	{ "Nessun altro testo", "No other text", "本文なし" },
	{ "Nota vuota", "Empty note", "空のメモ" },
	{ "Lettura delle note…", "Reading notes…", "メモを読み込み中…" },
	{ "Nessuna nota: tocca «Nuova nota» per iniziare", "No notes: tap «New note» to start",
	  "メモはありません: 「新規メモ」をタップ" },
	{ "Impossibile salvare la nota", "Cannot save the note", "メモを保存できません" },
	{ "La nota ha raggiunto la lunghezza massima", "The note has reached the maximum length", "メモが最大の長さに達しました" },
	{ "Scrivi qui…", "Write here…", "ここに入力…" },

	/* impostazioni */
	{ "Mai", "Never", "しない" },
	{ "Calcolo in corso…", "Calculating…", "計算中…" },
	{ "Collegata", "Connected", "接続" },
	{ "Non collegata", "Not connected", "未接続" },
	{ "SCHERMO", "SCREEN", "画面" },
	{ "Orientamento", "Orientation", "向き" },
	{ "Mostra percentuale batteria", "Show battery percentage", "バッテリー残量を%表示" },
	{ "Attenua lo schermo dopo", "Dim the screen after", "画面を暗くするまで" },
	{ "Spegni lo schermo dopo", "Turn the screen off after", "画面を消すまで" },
	{ "Sospendi dopo (a batteria)", "Sleep after (on battery)", "スリープまで (バッテリー時)" },
	{ "AUDIO", "SOUND", "サウンド" },
	{ "DISPOSITIVO", "DEVICE", "デバイス" },
	{ "Tasti del dizionario", "Dictionary keys", "辞書キー" },
	{ "ASPETTO", "APPEARANCE", "外観" },
	{ "Tema", "Theme", "テーマ" },
	{ "Animazioni", "Animations", "アニメーション" },
	{ "Complete", "Full", "すべて" },
	{ "Ridotte", "Reduced", "少なめ" },
	{ "Nessuna", "None", "なし" },
	{ "Riquadri della home", "Home tiles", "ホームのタイル" },
	{ "Pieni", "Filled", "塗り" },
	{ "Vetro", "Glass", "ガラス" },
	{ "Contorno", "Outline", "線" },
	{ "Lingua", "Language", "言語" },
	{ "BATTERIA", "BATTERY", "バッテリー" },
	{ "Livello", "Level", "残量" },
	{ "Stato", "Status", "状態" },
	{ "Tensione", "Voltage", "電圧" },
	{ "Alimentazione USB", "USB power", "USB給電" },
	{ "Autonomia stimata", "Estimated time left", "推定残り時間" },
	{ "SISTEMA", "SYSTEM", "システム" },
	{ "Memoria libera", "Free memory", "空きメモリ" },
	{ "Spazio libero", "Free space", "空き容量" },
	{ "Kernel", "Kernel", "カーネル" },
	{ "In arrivo", "Coming soon", "準備中" },
	{ "Questa app sarà disponibile in un prossimo aggiornamento.", "This app will be available in a future update.",
	  "このアプリは今後のアップデートで使えるようになります。" },
	{ "Cambio della lingua…", "Changing language…", "言語を変更しています…" },

	/* terminale */
	{ "storico −%d", "history −%d", "履歴 −%d" },
	{ "Apertura del terminale X…", "Opening the X terminal…", "Xターミナルを開いています…" },
	{ "Aprire in X?", "Open in X?", "Xで開きますか?" },
	{ "Il terminale st in una sessione X a tutto schermo (lento ad aprirsi). Questa sessione resta aperta.",
	  "The st terminal in a full screen X session (slow to open). This session stays open.",
	  "全画面Xセッションのstターミナル (起動は遅め)。このセッションは残ります。" },
	{ "Apri in X", "Open in X", "Xで開く" },

	/* data e ora */
	{ "Italia (Roma)", "Italy (Rome)", "イタリア (ローマ)" },
	{ "Regno Unito (Londra)", "United Kingdom (London)", "イギリス (ロンドン)" },
	{ "Portogallo (Lisbona)", "Portugal (Lisbon)", "ポルトガル (リスボン)" },
	{ "Spagna (Madrid)", "Spain (Madrid)", "スペイン (マドリード)" },
	{ "Francia (Parigi)", "France (Paris)", "フランス (パリ)" },
	{ "Germania (Berlino)", "Germany (Berlin)", "ドイツ (ベルリン)" },
	{ "Svizzera (Zurigo)", "Switzerland (Zurich)", "スイス (チューリッヒ)" },
	{ "Grecia (Atene)", "Greece (Athens)", "ギリシャ (アテネ)" },
	{ "Romania (Bucarest)", "Romania (Bucharest)", "ルーマニア (ブカレスト)" },
	{ "Turchia (Istanbul)", "Turkey (Istanbul)", "トルコ (イスタンブール)" },
	{ "Russia (Mosca)", "Russia (Moscow)", "ロシア (モスクワ)" },
	{ "Emirati (Dubai)", "UAE (Dubai)", "UAE (ドバイ)" },
	{ "India (Calcutta)", "India (Kolkata)", "インド (コルカタ)" },
	{ "Thailandia (Bangkok)", "Thailand (Bangkok)", "タイ (バンコク)" },
	{ "Cina (Shanghai)", "China (Shanghai)", "中国 (上海)" },
	{ "Giappone (Tokyo)", "Japan (Tokyo)", "日本 (東京)" },
	{ "Corea (Seul)", "Korea (Seoul)", "韓国 (ソウル)" },
	{ "Australia (Sydney)", "Australia (Sydney)", "オーストラリア (シドニー)" },
	{ "Nuova Zelanda (Auckland)", "New Zealand (Auckland)", "ニュージーランド (オークランド)" },
	{ "Brasile (San Paolo)", "Brazil (São Paulo)", "ブラジル (サンパウロ)" },
	{ "Argentina (Buenos Aires)", "Argentina (Buenos Aires)", "アルゼンチン (ブエノスアイレス)" },
	{ "USA (New York)", "USA (New York)", "アメリカ (ニューヨーク)" },
	{ "USA (Chicago)", "USA (Chicago)", "アメリカ (シカゴ)" },
	{ "USA (Denver)", "USA (Denver)", "アメリカ (デンバー)" },
	{ "USA (Los Angeles)", "USA (Los Angeles)", "アメリカ (ロサンゼルス)" },
	{ "Messico (Città del Messico)", "Mexico (Mexico City)", "メキシコ (メキシコシティ)" },
	{ "Impossibile cambiare l'ora", "Cannot change the time", "時刻を変更できません" },
	{ "Data e ora impostate", "Date and time set", "日付と時刻を設定しました" },
	{ "Impossibile cambiare il fuso orario", "Cannot change the time zone", "タイムゾーンを変更できません" },
	{ "Fuso orario cambiato", "Time zone changed", "タイムゾーンを変更しました" },
	{ "Ora", "Time", "時刻" },
	{ "Data", "Date", "日付" },
	{ "Fuso orario", "Time zone", "タイムゾーン" },
	{ "Imposta l'ora", "Set the time", "時刻を設定" },
	{ "Imposta la data", "Set the date", "日付を設定" },
	{ "ORE", "HOURS", "時" },
	{ "MINUTI", "MINUTES", "分" },
	{ "GIORNO", "DAY", "日" },
	{ "MESE", "MONTH", "月" },
	{ "ANNO", "YEAR", "年" },
	{ "Imposta", "Set", "設定" },

	/* dizionario */
	{ "Scrivi una parola", "Type a word", "単語を入力" },
	{ "Scrivi in romaji (inu, taberu…)", "Type in romaji (inu, taberu…)", "ローマ字で入力 (inu, taberu…)" },
	{ "Dizionario non installato: mancano i file in /usr/local/share/brain/dict",
	  "Dictionary not installed: the files in /usr/local/share/brain/dict are missing",
	  "辞書がありません: /usr/local/share/brain/dict にファイルがありません" },
	{ "Scrivi con la tastiera: le voci compaiono mentre digiti. Invio apre la prima, Esc torna indietro.",
	  "Type on the keyboard: entries appear as you type. Enter opens the first one, Esc goes back.",
	  "キーボードで入力すると候補が表示されます。Enterで最初の項目、Escで戻ります。" },
	{ "Nessuna voce trovata", "No entries found", "見つかりません" },

	/* giochi */
	{ "Trova le coppie", "Find the pairs", "ペアを探そう" },
	{ "Trova le coppie di carte", "Find the pairs of cards", "同じカードのペアを探す" },
	{ "Completato in %d mosse!", "Done in %d moves!", "%d手でクリア!" },
	{ "Mosse: %d", "Moves: %d", "手数: %d" },
	{ "Metti in ordine i numeri", "Put the numbers in order", "数字を順番に並べよう" },
	{ "Riordina le tessere da 1 a 15", "Sort the tiles from 1 to 15", "1から15まで並べ替える" },
	{ "Risolto in %d mosse!", "Solved in %d moves!", "%d手で完成!" },
	{ "Hai vinto!", "You won!", "あなたの勝ち!" },
	{ "Ha vinto il Brain", "The Brain won", "Brainの勝ち" },
	{ "Pareggio", "Draw", "引き分け" },
	{ "Tu giochi con X, il Brain con O", "You play X, the Brain plays O", "あなたはX、BrainはO" },
	{ "Tre in fila contro il Brain", "Three in a row against the Brain", "Brainと三目並べ" },
	{ "Frecce o trascina per girare · tocca per iniziare", "Arrows or drag to turn · tap to start",
	  "矢印キーかドラッグで方向転換 · タップで開始" },
	{ "Mangia e cresci, senza toccarti", "Eat and grow, without biting yourself", "食べて伸びよう、自分にぶつからないで" },
	{ "Punti: %d", "Score: %d", "得点: %d" },
	{ "Fine: %d punti (record %d) · tocca per ripartire", "Game over: %d points (best %d) · tap to restart",
	  "終了: %d点 (最高 %d) · タップで再開" },
	{ "Partita interrotta · tocca per ricominciare", "Game stopped · tap to start again", "中断しました · タップで再開" },
	{ "Ricomincia", "Restart", "やり直す" },
	{ "Quindici", "Fifteen", "15パズル" },
	{ "Tris", "Tic-tac-toe", "三目並べ" },
	{ "Serpente", "Snake", "スネーク" },
	{ "Memory", "Memory", "神経衰弱" },

	/* comuni */
	{ "OK", "OK", "OK" },
	{ "Chiudi", "Close", "閉じる" },
	{ "Indietro", "Back", "戻る" },
	{ "Sì", "Yes", "はい" },
	{ "No", "No", "いいえ" },
};
#define NTR ((int)(sizeof(tr) / sizeof(tr[0])))

int bs_lang;

static uint16_t slot[1024];	/* indice + 1 in tr[], 0 = vuoto */

static uint32_t hash(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

static const char *lookup(const char *s)
{
	uint32_t i = hash(s) & 1023;

	while (slot[i]) {
		if (!strcmp(tr[slot[i] - 1][0], s))
			return tr[slot[i] - 1][bs_lang];
		i = (i + 1) & 1023;
	}
	return s;
}

const char *bs_tr(const char *s)
{
	if (!bs_lang || !s || !*s)
		return s;
	return lookup(s);
}

static const char *ui_tr(const char *s)
{
	return bs_tr(s);
}

void bs_i18n_init(int lang)
{
	int k;

	bs_lang = lang >= 0 && lang < BS_NLANGS ? lang : 0;
	memset(slot, 0, sizeof(slot));
	for (k = 0; k < NTR; k++) {
		uint32_t i = hash(tr[k][0]) & 1023;

		while (slot[i])
			i = (i + 1) & 1023;
		slot[i] = (uint16_t)(k + 1);
	}
	bui_set_translator(bs_lang ? ui_tr : NULL);
}

const char *bs_lang_code(int lang)
{
	static const char *const codes[BS_NLANGS] = { "it", "en", "ja" };

	return codes[lang >= 0 && lang < BS_NLANGS ? lang : 0];
}

int bs_lang_find(const char *code)
{
	int i;

	for (i = 0; i < BS_NLANGS; i++)
		if (!strcmp(code, bs_lang_code(i)))
			return i;
	return 0;
}

/* date: "Venerdì 20 novembre", "Friday 20 November", "金曜日 11月20日" */
void bs_fmt_date(char *buf, int len, int style)
{
	static const char *const days[3][7] = {
		{ "domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato" },
		{ "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
		{ "日曜日", "月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日" },
	};
	static const char *const months[2][12] = {
		{ "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto",
		  "settembre", "ottobre", "novembre", "dicembre" },
		{ "January", "February", "March", "April", "May", "June", "July", "August", "September",
		  "October", "November", "December" },
	};
	time_t t = time(NULL);
	struct tm tm;
	const char *d;

	localtime_r(&t, &tm);
	d = days[bs_lang][tm.tm_wday];
	if (bs_lang == BS_LANG_JA) {
		if (style == BS_DATE_SHORT)
			snprintf(buf, (size_t)len, "%d月%d日 (%.3s)", tm.tm_mon + 1, tm.tm_mday, d);
		else if (style)
			snprintf(buf, (size_t)len, "%s %d年%d月%d日", d, tm.tm_year + 1900, tm.tm_mon + 1,
				 tm.tm_mday);
		else
			snprintf(buf, (size_t)len, "%s %d月%d日", d, tm.tm_mon + 1, tm.tm_mday);
		return;
	}
	if (style == BS_DATE_SHORT)
		snprintf(buf, (size_t)len, "%.3s %d %s", d, tm.tm_mday, months[bs_lang][tm.tm_mon]);
	else if (style)
		snprintf(buf, (size_t)len, "%s %d %s %d", d, tm.tm_mday, months[bs_lang][tm.tm_mon], tm.tm_year + 1900);
	else
		snprintf(buf, (size_t)len, "%s %d %s", d, tm.tm_mday, months[bs_lang][tm.tm_mon]);
	if (buf[0] >= 'a' && buf[0] <= 'z')
		buf[0] -= 32;
}

/* nome del mese: "Novembre", "November", "11月" */
const char *bs_month_name(int m, int abbrev)
{
	static const char *const it[12] = { "Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno",
					    "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre" };
	static const char *const en[12] = { "January", "February", "March", "April", "May", "June",
					    "July", "August", "September", "October", "November", "December" };
	static const char *const ja[12] = { "1月", "2月", "3月", "4月", "5月", "6月",
					    "7月", "8月", "9月", "10月", "11月", "12月" };
	static char ab[8];

	if (m < 0 || m > 11)
		return "";
	if (bs_lang == BS_LANG_JA)
		return ja[m];
	if (!abbrev)
		return bs_lang == BS_LANG_EN ? en[m] : it[m];
	snprintf(ab, sizeof(ab), "%.3s", bs_lang == BS_LANG_EN ? en[m] : it[m]);
	ab[0] += ab[0] >= 'A' && ab[0] <= 'Z' ? 'a' - 'A' : 0;
	return ab;
}

/* iniziali dei giorni da lunedi' (calendario) */
const char *bs_weekday_initial(int i)
{
	static const char *const ini[3][7] = {
		{ "L", "M", "M", "G", "V", "S", "D" },
		{ "M", "T", "W", "T", "F", "S", "S" },
		{ "月", "火", "水", "木", "金", "土", "日" },
	};

	return ini[bs_lang][i % 7];
}
