/* SPDX-License-Identifier: MIT */
/*
 * Touch / keyboard user interface: the control bar under the picture and,
 * for files without pictures, the audio view.
 */
#ifndef BRAINPLAY_UI_H
#define BRAINPLAY_UI_H

#include "brainplay.h"
#include "vo.h"
#include "win.h"

struct ui;

struct ui *ui_new(struct win *win, struct player *p, const struct options *opt);
void ui_free(struct ui *ui);
/* the video output to keep informed about the video area (may be NULL) */
void ui_set_vo(struct ui *ui, struct vo *vo);
/* false when the user asked to quit */
bool ui_event(struct ui *ui, XEvent *ev);
/* redraws what changed; returns when it wants to be called again */
double ui_tick(struct ui *ui, double now);

#endif
