/* amigui.h - the parts every GUI program of ours shares: the About box, the
 * settings page frame (Save / Use / [Defaults] / Cancel) and saving settings
 * the way the system's prefs editors do (Save = ENV: + ENVARC:, Use = ENV:).
 *
 * Copy amigui.c and amigui.h into a project's sources; GadTools and
 * Intuition V37 are enough (Kickstart 2.04). Pages are laid out from the
 * font the caller passes, so with Topaz 8 they fit a 640x200 screen.
 *
 * Origin: WHDUpdate's guiprefs.c (C:\projects\WHDUpdate), 2026-10-30. */
#ifndef AMIGUI_H
#define AMIGUI_H

#include <exec/types.h>
#include <intuition/intuition.h>
#include <graphics/text.h>
#include <graphics/rastport.h>

/* ---- About --------------------------------------------------------------- */

/* "About <name>" requester: "<name> <version>\n<text>" with an OK button.
 * text may hold \n; NULL for none. */
void amg_about(struct Window *win, const char *name, const char *version, const char *text);

/* ---- settings files -------------------------------------------------------- */

/* Write data to ENV:<name> and, when keep, also ENVARC:<name> (makes the
 * drawers in name's path as needed). 1 = written. */
int amg_env_write(const char *name, const void *data, ULONG len, int keep);

/* Read ENV:<name>, else ENVARC:<name>, into buf (NUL-terminated, at most
 * size-1 bytes). Returns the length, or -1 if neither is there. */
LONG amg_env_read(const char *name, char *buf, LONG size);

/* ---- a settings page -------------------------------------------------------- */

#define AMG_MAXGAD 64

enum { AMG_CANCEL = 0, AMG_USE = 1, AMG_SAVE = 2 };      /* what amg_page_run returns */
enum { AMG_ID_SAVE = 9001, AMG_ID_USE, AMG_ID_DEFAULTS, AMG_ID_CANCEL };

typedef struct {
    struct Screen *scr;
    APTR vi;
    struct TextAttr *ta;
    struct TextFont *font;
    struct RastPort rp;             /* for measuring text in the page's font */
    struct Window *win;
    struct Gadget *glist;           /* CreateContext() result */
    struct Gadget *g[AMG_MAXGAD];   /* the caller's gadgets, by its own index */
} AmgPage;

/* Open a page window (inner size w x h, kept on the screen) near parent.
 * The font is opened so the caller can measure with amg_tlen() first. */
int  amg_page_open(AmgPage *pg, struct Screen *scr, APTR vi, struct TextAttr *ta,
                   struct Window *parent, const char *title, int w, int h);
int  amg_tlen(AmgPage *pg, const char *s);
int  amg_bh(AmgPage *pg);                         /* a button's height: font + 6 */

/* The button row at inner y across inner width w: Save, Use, [Defaults],
 * Cancel - the order of the system's prefs editors. */
struct Gadget *amg_buttons(AmgPage *pg, struct Gadget *gad, int y, int w, int defaults);
/* The narrowest a page can be and still hold the button row. */
int  amg_min_width(struct TextAttr *ta);

/* Add the gadgets (pg->glist) and draw them. */
void amg_page_show(AmgPage *pg);

/* Run until Save / Use / Cancel (Esc and the close gadget = Cancel).
 * on_gadget gets every other GADGETUP (Defaults too, as AMG_ID_DEFAULTS). */
int  amg_page_run(AmgPage *pg, void (*on_gadget)(AmgPage *pg, int id, UWORD code, void *ctx),
                  void *ctx);
void amg_page_close(AmgPage *pg);

/* The state of a CHECKBOX_KIND gadget, and a STRING_KIND gadget's text. */
int  amg_checked(struct Gadget *g);
const char *amg_string(struct Gadget *g);

#endif
