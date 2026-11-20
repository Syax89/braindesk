// SPDX-License-Identifier: MIT
/*
 * bs_work.h - un filo di lavoro per cio' che non deve fermare l'interfaccia:
 * letture di cartelle su SD lente, decodifica di immagini, caricamento di
 * testi. Un solo filo, una coda; i risultati tornano nel ciclo di bui
 * attraverso una pipe (bui_fd_add), quindi le callback "done" girano sempre
 * nel filo principale.
 */
#ifndef BS_WORK_H
#define BS_WORK_H

/*
 * Accoda un lavoro. run() gira nel filo di lavoro: non deve toccare ne' bui
 * ne' lo stato delle viste, solo `arg`; deve controllare *cancel ogni tanto e
 * uscire presto se diventa non zero. done() gira nel filo principale, sempre
 * ed esattamente una volta (anche per un lavoro annullato, con cancelled = 1:
 * serve a liberare `arg`). Con front != 0 il lavoro passa davanti alla coda.
 * Ritorna un id > 0, oppure 0 se non e' stato possibile (done non chiamata).
 */
int bs_work_submit(void (*run)(void *arg, const volatile int *cancel),
		   void (*done)(void *arg, int cancelled), void *arg, int front);
/* annulla (id <= 0 o sconosciuto: nessun effetto) */
void bs_work_cancel(int id);

#endif
