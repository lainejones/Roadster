/* amigui.c - see amigui.h. */
#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <string.h>
#include "amigui.h"

#define M 8

/* ---- About --------------------------------------------------------------- */

void amg_about(struct Window *win, const char *name, const char *version, const char *text)
{
    struct EasyStruct es;
    char title[64];
    strncpy(title, "About ", sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
    strncat(title, name, sizeof(title) - strlen(title) - 1);
    memset(&es, 0, sizeof(es));
    es.es_StructSize = sizeof(es);
    es.es_Title = (UBYTE *)title;
    es.es_TextFormat = (UBYTE *)(text ? "%s %s\n\n%s" : "%s %s");
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequest(win, &es, NULL, (ULONG)name, (ULONG)version, (ULONG)text);
}

/* ---- settings files -------------------------------------------------------- */

/* Make the drawers in path (up to its last '/') as needed. */
static void make_dirs(char *path)
{
    char *p;
    for (p = path; *p; p++)
        if (*p == '/') {
            BPTR l;
            *p = 0;
            if ((l = Lock((STRPTR)path, ACCESS_READ)) != 0)
                UnLock(l);
            else if ((l = CreateDir((STRPTR)path)) != 0)
                UnLock(l);
            *p = '/';
        }
}

static int write_one(const char *dir, const char *name, const void *data, ULONG len)
{
    char path[256];
    BPTR fh;
    int ok;
    strncpy(path, dir, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    strncat(path, name, sizeof(path) - strlen(path) - 1);
    make_dirs(path);
    if (!(fh = Open((STRPTR)path, MODE_NEWFILE)))
        return 0;
    ok = Write(fh, (APTR)data, len) == (LONG)len;
    Close(fh);
    return ok;
}

int amg_env_write(const char *name, const void *data, ULONG len, int keep)
{
    int ok = write_one("ENV:", name, data, len);
    if (keep)
        ok = write_one("ENVARC:", name, data, len) && ok;
    return ok;
}

LONG amg_env_read(const char *name, char *buf, LONG size)
{
    static const char *dirs[] = {"ENV:", "ENVARC:"};
    char path[256];
    int i;
    for (i = 0; i < 2; i++) {
        BPTR fh;
        LONG n;
        strncpy(path, dirs[i], sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        strncat(path, name, sizeof(path) - strlen(path) - 1);
        if (!(fh = Open((STRPTR)path, MODE_OLDFILE)))
            continue;
        n = Read(fh, buf, size - 1);
        Close(fh);
        if (n < 0)
            n = 0;
        buf[n] = 0;
        return n;
    }
    return -1;
}

/* ---- a settings page -------------------------------------------------------- */

int amg_tlen(AmgPage *pg, const char *s)
{
    return TextLength(&pg->rp, (STRPTR)s, strlen(s));
}

int amg_bh(AmgPage *pg)
{
    return pg->font->tf_YSize + 6;
}

int amg_min_width(struct TextAttr *ta)
{
    struct TextFont *f = OpenFont(ta);
    struct RastPort rp;
    int bw;
    if (!f)
        return 420;
    InitRastPort(&rp);
    SetFont(&rp, f);
    bw = TextLength(&rp, (STRPTR)"Defaults", 8) + 24;
    CloseFont(f);
    return 2 * M + 4 * bw + 3 * 8;
}

int amg_page_open(AmgPage *pg, struct Screen *scr, APTR vi, struct TextAttr *ta,
                  struct Window *parent, const char *title, int w, int h)
{
    int left;
    memset(pg, 0, sizeof(*pg));
    pg->scr = scr;
    pg->vi = vi;
    pg->ta = ta;
    if (!(pg->font = OpenFont(ta)))
        return 0;
    InitRastPort(&pg->rp);
    SetFont(&pg->rp, pg->font);
    if (w > scr->Width - 20)
        w = scr->Width - 20;
    if (h > scr->Height - scr->BarHeight - 1 - scr->WBorTop - scr->Font->ta_YSize - 4)
        h = scr->Height - scr->BarHeight - 1 - scr->WBorTop - scr->Font->ta_YSize - 4;
    left = parent ? parent->LeftEdge + 20 : (scr->Width - w) / 2;
    if (left + w + 20 > scr->Width)
        left = scr->Width - w - 20;
    if (left < 0)
        left = 0;
    pg->win = OpenWindowTags(NULL,
        WA_Left, left, WA_Top, scr->BarHeight + 1,
        WA_InnerWidth, w, WA_InnerHeight, h,
        WA_Title, (ULONG)title, WA_PubScreen, (ULONG)scr,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_SimpleRefresh, TRUE,
        WA_IDCMP, BUTTONIDCMP | CHECKBOXIDCMP | STRINGIDCMP | CYCLEIDCMP | INTEGERIDCMP |
                  SLIDERIDCMP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
        TAG_END);
    if (!pg->win) {
        CloseFont(pg->font);
        pg->font = NULL;
        return 0;
    }
    return 1;
}

struct Gadget *amg_buttons(AmgPage *pg, struct Gadget *gad, int y, int w, int defaults)
{
    static const char *txt[] = {"Save", "Use", "Defaults", "Cancel"};
    static const int ids[] = {AMG_ID_SAVE, AMG_ID_USE, AMG_ID_DEFAULTS, AMG_ID_CANCEL};
    struct NewGadget ng;
    int bw = amg_tlen(pg, "Defaults") + 24, x = pg->win->BorderLeft + M;
    int n = defaults ? 4 : 3, i, k = 0;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = pg->vi;
    ng.ng_TextAttr = pg->ta;
    ng.ng_TopEdge = pg->win->BorderTop + y;
    ng.ng_Width = bw;
    ng.ng_Height = amg_bh(pg);
    ng.ng_Flags = PLACETEXT_IN;
    for (i = 0; i < 4; i++) {
        if (ids[i] == AMG_ID_DEFAULTS && !defaults)
            continue;
        ng.ng_LeftEdge = x + k++ * (w - 2 * M - bw) / (n - 1);
        ng.ng_GadgetText = (UBYTE *)txt[i];
        ng.ng_GadgetID = ids[i];
        gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    }
    return gad;
}

void amg_page_show(AmgPage *pg)
{
    AddGList(pg->win, pg->glist, ~0, -1, NULL);
    RefreshGList(pg->glist, pg->win, NULL, -1);
    GT_RefreshWindow(pg->win, NULL);
}

int amg_page_run(AmgPage *pg, void (*on_gadget)(AmgPage *pg, int id, UWORD code, void *ctx),
                 void *ctx)
{
    for (;;) {
        struct IntuiMessage *msg;
        WaitPort(pg->win->UserPort);
        while ((msg = GT_GetIMsg(pg->win->UserPort)) != NULL) {
            ULONG cl = msg->Class;
            UWORD code = msg->Code;
            struct Gadget *gad = (struct Gadget *)msg->IAddress;
            GT_ReplyIMsg(msg);
            switch (cl) {
            case IDCMP_CLOSEWINDOW:
                return AMG_CANCEL;
            case IDCMP_VANILLAKEY:
                if (code == 27)
                    return AMG_CANCEL;
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(pg->win);
                GT_EndRefresh(pg->win, TRUE);
                break;
            case IDCMP_GADGETUP:
                switch (gad->GadgetID) {
                case AMG_ID_SAVE:   return AMG_SAVE;
                case AMG_ID_USE:    return AMG_USE;
                case AMG_ID_CANCEL: return AMG_CANCEL;
                default:
                    if (on_gadget)
                        on_gadget(pg, gad->GadgetID, code, ctx);
                }
                break;
            }
        }
    }
}

void amg_page_close(AmgPage *pg)
{
    if (pg->win)
        CloseWindow(pg->win);
    if (pg->glist)
        FreeGadgets(pg->glist);
    if (pg->font)
        CloseFont(pg->font);
    memset(pg, 0, sizeof(*pg));
}

int amg_checked(struct Gadget *g)
{
    return (g->Flags & GFLG_SELECTED) != 0;
}

const char *amg_string(struct Gadget *g)
{
    return (const char *)((struct StringInfo *)g->SpecialInfo)->Buffer;
}
