/*
 * Roadster - a native Roadshow on/off switch + status panel (Commodity)
 *
 * Replaces Roadie (AmiBlitz3) which gurus on the A530's 68EC030 whenever the
 * data cache is enabled (TRAP #11 / spontaneous reset - see the daynaport
 * project notes, 2026-08-22). Plain C + GadTools, no FPU, no Blitz runtime,
 * runs on any 68000+ with OS 2.0+.
 *
 *  +--------------------------------------------------------------+
 *  | Status: ONLINE   IP 192.168.50.36                            |
 *  | Iface:  scsidayna                                            |
 *  | [ Offline ] [ Online ] [ Status ] [ Ping ] [ Trace ] [Clear] |
 *  | Target: [ 8.8.8.8__________________ ]                        |
 *  | +----------------------------------------------------------+ |
 *  | | command output (scrolling listview)                      | |
 *  | +----------------------------------------------------------+ |
 *  | ready                                                        |
 *  +--------------------------------------------------------------+
 *
 * Design notes:
 *  - Offline = ConfigureNetInterface <iface> OFFLINE. Deliberately NOT
 *    NetShutdown: a full stack teardown blocks on every program holding a
 *    bsdsocket socket (smb2fs, netharness, ...), and Roadshow then DEFERS the
 *    teardown, which fires asynchronously later - the class of crash that
 *    took the A500 down repeatedly. Interface-offline stops traffic without
 *    ripping the stack out from under anything.
 *  - Online = AddNetInterface (in case the stack was fully shut down by
 *    other means) followed by ConfigureNetInterface ONLINE UP.
 *  - Commands run ASYNCHRONOUSLY: we write T:Roadster.script ending in an
 *    "Echo >T:Roadster.done" sentinel, SystemTags() it with SYS_Asynch, and
 *    a timer tick polls for the sentinel. The GUI never freezes, even during
 *    a slow traceroute.
 *  - The interface name is auto-detected from DEVS:NetInterfaces/ (first
 *    entry), overridable with an IFACE=<name> CLI argument or icon tooltype.
 *  - ENV:RoadshowOnline is maintained like Roadie did (OS 3.2 title-bar use).
 *
 * Build with build.sh under WSL (amiga-gcc). Strip at LINK time with -s;
 * the standalone strip corrupts hunk relocs on this toolchain.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <libraries/commodities.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/commodities.h>
#include <proto/icon.h>

#include <string.h>

static const char verstag[] __attribute__((used)) =
    "$VER: Roadster 1.0 (22.08.2026)";

unsigned long __stack = 20000;

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *CxBase        = NULL;
struct Library       *IconBase      = NULL;

/* ------------------------------------------------------------------ */

#define OUTFILE   "T:Roadster.out"
#define DONEFILE  "T:Roadster.done"
#define SCRIPTFILE "T:Roadster.script"

#define MAXLINES  250              /* listview line pool                */
#define LINELEN   78
#define HOSTMAX   60
#define IFACEMAX  32

#define POLLSECS  1                /* sentinel poll while busy          */
#define IDLESECS  15               /* auto status refresh when idle     */

#define EVT_HOTKEY 1L

enum {                             /* gadget IDs                        */
    GAD_OFFLINE = 1, GAD_ONLINE, GAD_STATUS, GAD_PING, GAD_TRACE,
    GAD_CLEAR, GAD_TARGET, GAD_LIST, GAD_MSG
};

enum {                             /* what the running script is doing  */
    ACT_NONE = 0, ACT_ONLINE, ACT_OFFLINE, ACT_REFRESH, ACT_SHOW,
    ACT_PING, ACT_TRACE
};

struct Line {
    struct Node node;
    char        text[LINELEN + 2];
};

struct App {
    /* commodity */
    struct MsgPort *cxPort;
    CxObj          *broker;

    /* timer */
    struct MsgPort *timerPort;
    struct timerequest *timerIO;
    BOOL            timerOpen, timerRunning;

    /* gui */
    struct Screen  *scr;
    APTR            vi;
    struct Window  *win;
    struct Gadget  *glist, *gTarget, *gList, *gMsg, *gLine1, *gLine2;
    WORD            resizePending;

    /* listview backing store: fixed pool used as a ring */
    struct List     lines;
    struct Line     pool[MAXLINES];
    int             nextLine, usedLines;

    /* state */
    char            iface[IFACEMAX + 2];
    char            target[HOSTMAX + 2];
    char            line1[80], line2[80], msg[80];
    int             action;        /* ACT_xxx while busy, ACT_NONE idle */
    int             idleTicks;
    int             busyTicks;     /* watchdog: ticks since action start */
    BOOL            quit;
};

static struct App app;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void initList(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void setMsg(struct App *a, const char *s)
{
    strncpy(a->msg, s, sizeof(a->msg) - 1);
    a->msg[sizeof(a->msg) - 1] = 0;
    if (a->win && a->gMsg)
        GT_SetGadgetAttrs(a->gMsg, a->win, NULL,
                          (ULONG)GTTX_Text, (ULONG)a->msg, TAG_END);
}

static void setTopLines(struct App *a)
{
    if (!a->win) return;
    if (a->gLine1)
        GT_SetGadgetAttrs(a->gLine1, a->win, NULL,
                          (ULONG)GTTX_Text, (ULONG)a->line1, TAG_END);
    if (a->gLine2)
        GT_SetGadgetAttrs(a->gLine2, a->win, NULL,
                          (ULONG)GTTX_Text, (ULONG)a->line2, TAG_END);
}

/* detach/attach the listview labels around list surgery */
static void listDetach(struct App *a)
{
    if (a->win && a->gList)
        GT_SetGadgetAttrs(a->gList, a->win, NULL,
                          (ULONG)GTLV_Labels, (ULONG)~0, TAG_END);
}

static void listAttach(struct App *a)
{
    if (a->win && a->gList)
        GT_SetGadgetAttrs(a->gList, a->win, NULL,
                          (ULONG)GTLV_Labels, (ULONG)&a->lines,
                          (ULONG)GTLV_Top,
                          (ULONG)(a->usedLines > 4 ? a->usedLines - 4 : 0),
                          TAG_END);
}

static void clearLines(struct App *a)
{
    listDetach(a);
    initList(&a->lines);
    a->nextLine = 0; a->usedLines = 0;
    listAttach(a);
}

/* append one line, recycling the oldest node once the pool is full */
static void addLine(struct App *a, const char *text)
{
    struct Line *ln;
    int len;

    if (a->usedLines >= MAXLINES) {
        ln = (struct Line *)RemHead(&a->lines);   /* oldest       */
    } else {
        ln = &a->pool[a->nextLine++];
        a->usedLines++;
    }
    len = (int)strlen(text);
    if (len > LINELEN) len = LINELEN;
    memcpy(ln->text, text, len); ln->text[len] = 0;
    ln->node.ln_Name = ln->text;
    ln->node.ln_Type = 0; ln->node.ln_Pri = 0;
    AddTail(&a->lines, &ln->node);
}

/* ------------------------------------------------------------------ */
/* interface autodetect: first entry in DEVS:NetInterfaces            */
/* ------------------------------------------------------------------ */

static void detectIface(struct App *a)
{
    BPTR lk;
    struct FileInfoBlock *fib;

    if (a->iface[0]) return;                     /* already set (arg)  */

    lk = Lock((STRPTR)"DEVS:NetInterfaces", ACCESS_READ);
    if (!lk) return;
    fib = (struct FileInfoBlock *)AllocVec(sizeof(*fib), MEMF_CLEAR | MEMF_PUBLIC);
    if (fib && Examine(lk, fib)) {
        while (ExNext(lk, fib)) {
            if (fib->fib_DirEntryType < 0) {     /* a file             */
                strncpy(a->iface, fib->fib_FileName, IFACEMAX);
                a->iface[IFACEMAX] = 0;
                break;
            }
        }
    }
    if (fib) FreeVec(fib);
    UnLock(lk);
}

/* ------------------------------------------------------------------ */
/* async command running                                              */
/*                                                                    */
/* T:Roadster.script:   <command> >T:Roadster.out                     */
/*                      Echo >T:Roadster.done "x"                     */
/* launched with SystemTags(SYS_Asynch); the timer tick polls for the */
/* done-file, then the output file is loaded into the listview.       */
/* ------------------------------------------------------------------ */

static BOOL launchScript(struct App *a, const char *cmdline, int action)
{
    BPTR fh, in, out;
    LONG rc;

    if (a->action != ACT_NONE) return FALSE;     /* one at a time      */

    DeleteFile((STRPTR)DONEFILE);
    DeleteFile((STRPTR)OUTFILE);

    fh = Open((STRPTR)SCRIPTFILE, MODE_NEWFILE);
    if (!fh) return FALSE;
    /* FAILAT 31: Execute ABORTS a script when a command returns >= the fail
     * level (default 10) - a failing net command would skip the sentinel Echo
     * and leave us "busy" forever. Raise the level so the sentinel always runs. */
    FPuts(fh, (STRPTR)"FAILAT 31\n");
    /* every line of the command gets its output captured: first line
     * creates OUTFILE, later lines append to it */
    {
        const char *p = cmdline;
        int first = 1;
        while (*p) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            FWrite(fh, (APTR)p, 1, (LONG)(e - p));
            FPuts(fh, (STRPTR)(first ? " >" OUTFILE "\n" : " >>" OUTFILE "\n"));
            first = 0;
            p = (*e == '\n') ? e + 1 : e;
        }
    }
    FPuts(fh, (STRPTR)"Echo >" DONEFILE " \"x\"\n");
    Close(fh);

    /* SYS_Asynch needs its own in/out handles; DOS closes them for us */
    in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    rc = SystemTags((STRPTR)"Execute " SCRIPTFILE,
                    (ULONG)SYS_Input,  (ULONG)in,
                    (ULONG)SYS_Output, (ULONG)out,
                    (ULONG)SYS_Asynch, TRUE,
                    (ULONG)NP_StackSize, 8192,
                    TAG_END);
    if (rc == -1) {                              /* couldn't even launch */
        if (in)  Close(in);
        if (out) Close(out);
        return FALSE;
    }
    a->action = action;
    a->busyTicks = 0;
    return TRUE;
}

/* seconds before an action is declared stuck (its script may still finish
 * later - harmless, stale files are deleted at the next launch) */
static int actionTimeout(int act)
{
    return (act == ACT_TRACE) ? 240 : 90;
}

static BOOL scriptDone(void)
{
    BPTR lk = Lock((STRPTR)DONEFILE, ACCESS_READ);
    if (!lk) return FALSE;
    UnLock(lk);
    return TRUE;
}

/* pour OUTFILE into the listview */
static void loadOutput(struct App *a, BOOL clearFirst)
{
    BPTR fh;
    static char buf[200];

    listDetach(a);
    if (clearFirst) { initList(&a->lines); a->nextLine = 0; a->usedLines = 0; }

    fh = Open((STRPTR)OUTFILE, MODE_OLDFILE);
    if (fh) {
        while (FGets(fh, (STRPTR)buf, sizeof(buf) - 1)) {
            int len = (int)strlen(buf);
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
                buf[--len] = 0;
            addLine(a, buf);
        }
        Close(fh);
    }
    listAttach(a);
}

/* ------------------------------------------------------------------ */
/* status parsing (ShowNetStatus INTERFACES output)                   */
/*                                                                    */
/* Name       MTU Type     Address        Recv  Sent  Drop ... Status */
/* scsidayna 1500 Ethernet 192.168.50.36  1850   401     0 ...     Up */
/* ------------------------------------------------------------------ */

static BOOL stackUp(void)
{
    struct Node *n;
    Forbid();
    n = FindName(&((struct ExecBase *)SysBase)->LibList,
                 (STRPTR)"bsdsocket.library");
    Permit();
    return n != NULL;
}

static void parseStatus(struct App *a)
{
    BPTR fh;
    static char buf[240];
    char ip[40]; int haveRow = 0, up = 0;

    ip[0] = 0;

    fh = Open((STRPTR)OUTFILE, MODE_OLDFILE);
    if (fh) {
        while (FGets(fh, (STRPTR)buf, sizeof(buf) - 1)) {
            if (a->iface[0] &&
                strncmp(buf, a->iface, strlen(a->iface)) == 0) {
                /* tokenize: name mtu type address ... status(last) */
                char *p = buf, *tok[24]; int nt = 0;
                while (*p && nt < 24) {
                    while (*p == ' ' || *p == '\t') p++;
                    if (!*p || *p == '\n') break;
                    tok[nt++] = p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                    if (*p) *p++ = 0;
                }
                if (nt >= 4) {
                    strncpy(ip, tok[3], sizeof(ip) - 1); ip[sizeof(ip)-1] = 0;
                    haveRow = 1;
                    if (nt >= 5) {
                        char *st = tok[nt-1];
                        up = (st[0]=='U' || st[0]=='u');
                    }
                }
                break;
            }
        }
        Close(fh);
    }

    if (!stackUp()) {
        strcpy(a->line1, "Status: OFFLINE (stack down)");
        SetVar((STRPTR)"RoadshowOnline", (STRPTR)"Offline", -1, GVF_GLOBAL_ONLY);
    } else if (haveRow && up) {
        strcpy(a->line1, "Status: ONLINE   IP ");
        strncat(a->line1, ip, sizeof(a->line1) - strlen(a->line1) - 1);
        SetVar((STRPTR)"RoadshowOnline", (STRPTR)"Online", -1, GVF_GLOBAL_ONLY);
    } else if (haveRow) {
        strcpy(a->line1, "Status: OFFLINE (interface down)");
        SetVar((STRPTR)"RoadshowOnline", (STRPTR)"Offline", -1, GVF_GLOBAL_ONLY);
    } else {
        /* Offline removes the interface entirely, so "no row" IS the
         * normal offline state, not an error */
        strcpy(a->line1, "Status: OFFLINE");
        SetVar((STRPTR)"RoadshowOnline", (STRPTR)"Offline", -1, GVF_GLOBAL_ONLY);
    }

    strcpy(a->line2, "Iface:  ");
    strncat(a->line2, a->iface[0] ? a->iface : "(none found)",
            sizeof(a->line2) - strlen(a->line2) - 1);
    setTopLines(a);
}

/* ------------------------------------------------------------------ */
/* the actions                                                        */
/* ------------------------------------------------------------------ */

static void enableButtons(struct App *a, BOOL on);

static void startRefresh(struct App *a, int act)
{
    if (launchScript(a, "C:ShowNetStatus INTERFACES", act))
        setMsg(a, "checking status...");
}

/* Online = AddNetInterface, Offline = RemoveNetInterface FORCE.
 * Verified on hardware (A500 + scsidayna, 2026-08-22): this pair round-trips
 * cleanly. ConfigureNetInterface ONLINE/OFFLINE does NOT - an interface taken
 * OFFLINE could not be revived by ONLINE UP on either test machine, and
 * NetShutdown is a stack-wide teardown that defers (and later crashes) when
 * any program holds a socket. Remove/Add keeps the stack up and re-runs the
 * interface's own config file, exactly like the boot path. */
static void doOnline(struct App *a)
{
    static char cmd[200];
    if (!a->iface[0]) { setMsg(a, "no interface in DEVS:NetInterfaces!"); return; }
    strcpy(cmd, "C:AddNetInterface DEVS:NetInterfaces/");
    strcat(cmd, a->iface);
    if (launchScript(a, cmd, ACT_ONLINE)) {
        setMsg(a, "going online (may take ~30s on WiFi)...");
        enableButtons(a, FALSE);
    }
}

static void doOffline(struct App *a)
{
    static char cmd[120];
    if (!a->iface[0]) { setMsg(a, "no interface in DEVS:NetInterfaces!"); return; }
    strcpy(cmd, "C:RemoveNetInterface ");
    strcat(cmd, a->iface);
    strcat(cmd, " FORCE");
    if (launchScript(a, cmd, ACT_OFFLINE)) {
        setMsg(a, "going offline...");
        enableButtons(a, FALSE);
    }
}

static void doShow(struct App *a)
{
    if (launchScript(a, "C:ShowNetStatus", ACT_SHOW)) {
        setMsg(a, "collecting full net status...");
        enableButtons(a, FALSE);
    }
}

static void readTarget(struct App *a)
{
    if (a->win && a->gTarget) {
        STRPTR s = ((struct StringInfo *)a->gTarget->SpecialInfo)->Buffer;
        strncpy(a->target, (char *)s, HOSTMAX);
        a->target[HOSTMAX] = 0;
    }
    if (!a->target[0]) strcpy(a->target, "8.8.8.8");
}

static void doPing(struct App *a)
{
    static char cmd[140];
    readTarget(a);
    strcpy(cmd, "C:ping ");
    strcat(cmd, a->target);
    strcat(cmd, " -c 4");
    if (launchScript(a, cmd, ACT_PING)) {
        setMsg(a, "pinging (4 packets)...");
        enableButtons(a, FALSE);
    }
}

static void doTrace(struct App *a)
{
    static char cmd[140];
    readTarget(a);
    strcpy(cmd, "C:traceroute ");
    strcat(cmd, a->target);
    if (launchScript(a, cmd, ACT_TRACE)) {
        setMsg(a, "traceroute running (can take a while)...");
        enableButtons(a, FALSE);
    }
}

/* a script finished: consume its output according to what it was */
static void finishAction(struct App *a)
{
    int act = a->action;
    a->action = ACT_NONE;

    switch (act) {
    case ACT_REFRESH:
        parseStatus(a);
        setMsg(a, "ready");
        break;
    case ACT_ONLINE:
    case ACT_OFFLINE:
        loadOutput(a, FALSE);                 /* show any complaints   */
        enableButtons(a, TRUE);
        startRefresh(a, ACT_REFRESH);         /* then re-read status   */
        break;
    case ACT_SHOW:
        loadOutput(a, TRUE);
        enableButtons(a, TRUE);
        parseStatus(a);                       /* headline from same dump? no - cheap re-read */
        setMsg(a, "full net status below");
        break;
    case ACT_PING:
    case ACT_TRACE:
        loadOutput(a, TRUE);
        enableButtons(a, TRUE);
        setMsg(a, act == ACT_PING ? "ping done" : "traceroute done");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* window                                                             */
/* ------------------------------------------------------------------ */

static BOOL makeGadgets(struct App *a)
{
    struct NewGadget ng;
    struct Gadget *gad;
    struct Window *w = a->win;
    WORD fh, gh, cl, ct, cr, cb, y, bw, lvH;

    gad = CreateContext(&a->glist);
    if (!gad) return FALSE;

    fh = a->scr->RastPort.TxHeight;
    gh = fh + 4;
    cl = w->BorderLeft + 6;
    ct = w->BorderTop  + 4;
    cr = w->Width  - w->BorderRight  - 6;
    cb = w->Height - w->BorderBottom - 4;

    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr   = a->scr->Font;
    ng.ng_VisualInfo = a->vi;

    /* two status text lines */
    y = ct;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y;
    ng.ng_Width = cr - cl; ng.ng_Height = fh + 1;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = 0;
    gad = a->gLine1 = CreateGadget(TEXT_KIND, gad, &ng,
                                   (ULONG)GTTX_Text, (ULONG)a->line1, TAG_END);
    y += fh + 2;
    ng.ng_TopEdge = y;
    gad = a->gLine2 = CreateGadget(TEXT_KIND, gad, &ng,
                                   (ULONG)GTTX_Text, (ULONG)a->line2, TAG_END);

    /* button row: Offline Online Status Ping Trace Clear */
    y += fh + 5;
    bw = (cr - cl - 5 * 4) / 6;
    ng.ng_TopEdge = y; ng.ng_Height = gh; ng.ng_Width = bw; ng.ng_Flags = 0;

    ng.ng_LeftEdge = cl;
    ng.ng_GadgetText = (STRPTR)"Offline"; ng.ng_GadgetID = GAD_OFFLINE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    ng.ng_LeftEdge = cl + (bw + 4);
    ng.ng_GadgetText = (STRPTR)"Online"; ng.ng_GadgetID = GAD_ONLINE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    ng.ng_LeftEdge = cl + 2 * (bw + 4);
    ng.ng_GadgetText = (STRPTR)"Status"; ng.ng_GadgetID = GAD_STATUS;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    ng.ng_LeftEdge = cl + 3 * (bw + 4);
    ng.ng_GadgetText = (STRPTR)"Ping"; ng.ng_GadgetID = GAD_PING;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    ng.ng_LeftEdge = cl + 4 * (bw + 4);
    ng.ng_GadgetText = (STRPTR)"Trace"; ng.ng_GadgetID = GAD_TRACE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    ng.ng_LeftEdge = cl + 5 * (bw + 4);
    ng.ng_GadgetText = (STRPTR)"Clear"; ng.ng_GadgetID = GAD_CLEAR;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    /* target host string */
    y += gh + 5;
    ng.ng_LeftEdge = cl + 58; ng.ng_TopEdge = y;
    ng.ng_Width = cr - (cl + 58); ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)"Target"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GAD_TARGET;
    gad = a->gTarget = CreateGadget(STRING_KIND, gad, &ng,
                                    (ULONG)GTST_MaxChars, HOSTMAX,
                                    (ULONG)GTST_String, (ULONG)a->target,
                                    TAG_END);

    /* message line at the bottom */
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = cb - fh - 1;
    ng.ng_Width = cr - cl; ng.ng_Height = fh + 1;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_MSG;
    gad = a->gMsg = CreateGadget(TEXT_KIND, gad, &ng,
                                 (ULONG)GTTX_Text, (ULONG)a->msg, TAG_END);

    /* output listview fills the middle */
    y += gh + 5;
    lvH = (cb - fh - 5) - y;
    if (lvH < gh) lvH = gh;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y;
    ng.ng_Width = cr - cl; ng.ng_Height = lvH;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = GAD_LIST;
    gad = a->gList = CreateGadget(LISTVIEW_KIND, gad, &ng,
                                  (ULONG)GTLV_Labels,   (ULONG)&a->lines,
                                  (ULONG)GTLV_ReadOnly, TRUE,
                                  TAG_END);

    return (gad != NULL);
}

static void enableButtons(struct App *a, BOOL on)
{
    struct Gadget *g;
    if (!a->win) return;
    for (g = a->glist; g; g = g->NextGadget) {
        switch (g->GadgetID) {
        case GAD_OFFLINE: case GAD_ONLINE: case GAD_STATUS:
        case GAD_PING: case GAD_TRACE:
            GT_SetGadgetAttrs(g, a->win, NULL,
                              (ULONG)GA_Disabled, (ULONG)!on, TAG_END);
            break;
        }
    }
}

static void relayout(struct App *a)
{
    if (a->glist) {
        RemoveGList(a->win, a->glist, -1);
        FreeGadgets(a->glist);
        a->glist = NULL;
        a->gTarget = a->gList = a->gMsg = a->gLine1 = a->gLine2 = NULL;
        EraseRect(a->win->RPort, a->win->BorderLeft, a->win->BorderTop,
                  a->win->Width  - a->win->BorderRight  - 1,
                  a->win->Height - a->win->BorderBottom - 1);
    }
    if (makeGadgets(a)) {
        AddGList(a->win, a->glist, ~0, -1, NULL);
        RefreshGList(a->glist, a->win, NULL, -1);
        GT_RefreshWindow(a->win, NULL);
        if (a->action != ACT_NONE) enableButtons(a, FALSE);
    } else if (a->glist) {
        FreeGadgets(a->glist);
        a->glist = NULL;
        a->gTarget = a->gList = a->gMsg = a->gLine1 = a->gLine2 = NULL;
    }
}

static void closeWin(struct App *a)
{
    if (a->win) {
        if (a->glist) RemoveGList(a->win, a->glist, -1);
        CloseWindow(a->win);
        a->win = NULL;
    }
    if (a->glist) { FreeGadgets(a->glist); a->glist = NULL; }
    a->gTarget = a->gList = a->gMsg = a->gLine1 = a->gLine2 = NULL;
    if (a->vi)  { FreeVisualInfo(a->vi); a->vi = NULL; }
    if (a->scr) { UnlockPubScreen(NULL, a->scr); a->scr = NULL; }
}

static void openWin(struct App *a)
{
    WORD winw, winh, wtop, left;

    if (a->win) { WindowToFront(a->win); ActivateWindow(a->win); return; }

    a->scr = LockPubScreen(NULL);
    if (!a->scr) return;
    a->vi = GetVisualInfo(a->scr, TAG_END);
    if (!a->vi) { UnlockPubScreen(NULL, a->scr); a->scr = NULL; return; }

    winw = 480; if (winw > a->scr->Width) winw = a->scr->Width;
    winh = 236;
    if (winh > a->scr->Height - (a->scr->BarHeight + 2))
        winh = a->scr->Height - (a->scr->BarHeight + 2);
    wtop = a->scr->BarHeight + 2;
    left = (a->scr->Width - winw) / 2; if (left < 0) left = 0;

    a->win = OpenWindowTags(NULL,
        (ULONG)WA_Title,     (ULONG)"Roadster",
        (ULONG)WA_Left,      left, (ULONG)WA_Top, wtop,
        (ULONG)WA_Width,     winw, (ULONG)WA_Height, winh,
        (ULONG)WA_MinWidth,  380,  (ULONG)WA_MinHeight, 170,
        (ULONG)WA_MaxWidth,  ~0,   (ULONG)WA_MaxHeight, ~0,
        (ULONG)WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                             WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE,
        (ULONG)WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
                             IDCMP_NEWSIZE | BUTTONIDCMP | STRINGIDCMP |
                             LISTVIEWIDCMP,
        (ULONG)WA_PubScreen, (ULONG)a->scr,
        TAG_END);
    if (!a->win) {
        FreeVisualInfo(a->vi); a->vi = NULL;
        UnlockPubScreen(NULL, a->scr); a->scr = NULL;
        return;
    }
    relayout(a);

    /* fresh status whenever the window appears */
    if (a->action == ACT_NONE) startRefresh(a, ACT_REFRESH);
}

/* ------------------------------------------------------------------ */
/* timer + commodity                                                  */
/* ------------------------------------------------------------------ */

static void armTimer(struct App *a)
{
    if (!a->timerOpen) return;
    a->timerIO->tr_node.io_Command = TR_ADDREQUEST;
    a->timerIO->tr_time.tv_secs    = POLLSECS;
    a->timerIO->tr_time.tv_micro   = 0;
    SendIO((struct IORequest *)a->timerIO);
    a->timerRunning = TRUE;
}

static BOOL setupTimer(struct App *a)
{
    a->timerPort = CreateMsgPort();
    if (!a->timerPort) return FALSE;
    a->timerIO = (struct timerequest *)
        CreateIORequest(a->timerPort, sizeof(struct timerequest));
    if (!a->timerIO) return FALSE;
    if (OpenDevice((STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)a->timerIO, 0) != 0)
        return FALSE;
    a->timerOpen = TRUE;
    return TRUE;
}

static void cleanupTimer(struct App *a)
{
    if (a->timerRunning) {
        if (!CheckIO((struct IORequest *)a->timerIO))
            AbortIO((struct IORequest *)a->timerIO);
        WaitIO((struct IORequest *)a->timerIO);
        a->timerRunning = FALSE;
    }
    if (a->timerOpen) { CloseDevice((struct IORequest *)a->timerIO); a->timerOpen = FALSE; }
    if (a->timerIO)   { DeleteIORequest((struct IORequest *)a->timerIO); a->timerIO = NULL; }
    if (a->timerPort) { DeleteMsgPort(a->timerPort); a->timerPort = NULL; }
}

static BOOL setupBroker(struct App *a)
{
    struct NewBroker nb;
    CxObj *filter, *sender, *translate;

    a->cxPort = CreateMsgPort();
    if (!a->cxPort) return FALSE;

    memset(&nb, 0, sizeof(nb));
    nb.nb_Version = NB_VERSION;
    nb.nb_Name    = (BYTE *)"Roadster";
    nb.nb_Title   = (BYTE *)"Roadster 1.0";
    nb.nb_Descr   = (BYTE *)"Roadshow online/offline switch";
    nb.nb_Unique  = NBU_UNIQUE | NBU_NOTIFY;
    nb.nb_Flags   = COF_SHOW_HIDE;
    nb.nb_Port    = a->cxPort;
    nb.nb_Pri     = 0;

    a->broker = CxBroker(&nb, NULL);
    if (!a->broker) return FALSE;

    filter = CxFilter((STRPTR)"ctrl alt r");
    if (filter) {
        AttachCxObj(a->broker, filter);
        sender = CxSender(a->cxPort, EVT_HOTKEY);
        if (sender) AttachCxObj(filter, sender);
        translate = CxTranslate(NULL);
        if (translate) AttachCxObj(filter, translate);
    }

    ActivateCxObj(a->broker, 1L);
    return TRUE;
}

/* ------------------------------------------------------------------ */

static void handleGadget(struct App *a, UWORD id)
{
    switch (id) {
    case GAD_OFFLINE: doOffline(a); break;
    case GAD_ONLINE:  doOnline(a);  break;
    case GAD_STATUS:  doShow(a);    break;
    case GAD_PING:    doPing(a);    break;
    case GAD_TRACE:   doTrace(a);   break;
    case GAD_CLEAR:   clearLines(a); setMsg(a, "ready"); break;
    case GAD_TARGET:  readTarget(a); break;
    }
}

static void mainLoop(struct App *a)
{
    ULONG cxSig    = 1UL << a->cxPort->mp_SigBit;
    ULONG timerSig = a->timerPort ? (1UL << a->timerPort->mp_SigBit) : 0;

    armTimer(a);

    while (!a->quit) {
        ULONG winSig = a->win ? (1UL << a->win->UserPort->mp_SigBit) : 0;
        ULONG got = Wait(cxSig | timerSig | winSig | SIGBREAKF_CTRL_C);

        if (got & SIGBREAKF_CTRL_C) a->quit = TRUE;

        if (got & cxSig) {
            CxMsg *msg;
            while ((msg = (CxMsg *)GetMsg(a->cxPort))) {
                ULONG type = CxMsgType(msg);
                ULONG id   = CxMsgID(msg);
                ReplyMsg((struct Message *)msg);
                if (type == CXM_IEVENT && id == EVT_HOTKEY) {
                    if (a->win) closeWin(a); else openWin(a);
                } else if (type == CXM_COMMAND) {
                    switch (id) {
                    case CXCMD_DISABLE:   ActivateCxObj(a->broker, 0L); break;
                    case CXCMD_ENABLE:    ActivateCxObj(a->broker, 1L); break;
                    case CXCMD_APPEAR:
                    case CXCMD_UNIQUE:    openWin(a);  break;
                    case CXCMD_DISAPPEAR: closeWin(a); break;
                    case CXCMD_KILL:      a->quit = TRUE; break;
                    }
                }
            }
        }

        if (got & timerSig) {
            struct Message *tm;
            while ((tm = GetMsg(a->timerPort))) { (void)tm; }
            a->timerRunning = FALSE;

            if (a->action != ACT_NONE) {
                if (scriptDone()) {
                    finishAction(a);
                } else if (++a->busyTicks >= actionTimeout(a->action)) {
                    /* watchdog: never leave the buttons greyed out forever */
                    a->action = ACT_NONE;
                    loadOutput(a, FALSE);
                    enableButtons(a, TRUE);
                    setMsg(a, "timed out - command may still be running");
                }
            } else if (a->win) {
                /* idle: refresh the headline every IDLESECS */
                if (++a->idleTicks >= IDLESECS) {
                    a->idleTicks = 0;
                    startRefresh(a, ACT_REFRESH);
                }
            }
            armTimer(a);
        }

        if (a->win && (got & winSig)) {
            struct IntuiMessage *im;
            while ((im = GT_GetIMsg(a->win->UserPort))) {
                ULONG cls = im->Class;
                UWORD code = im->Code;
                struct Gadget *gad = (struct Gadget *)im->IAddress;
                (void)code;
                GT_ReplyIMsg(im);

                switch (cls) {
                case IDCMP_CLOSEWINDOW:
                    closeWin(a);               /* hide; commodity lives on */
                    break;
                case IDCMP_GADGETUP:
                    handleGadget(a, gad->GadgetID);
                    break;
                case IDCMP_NEWSIZE:
                    a->resizePending = TRUE;
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(a->win);
                    GT_EndRefresh(a->win, TRUE);
                    break;
                }
                if (!a->win) break;
            }
            if (a->win && a->resizePending) {
                a->resizePending = FALSE;
                relayout(a);
            }
        }
    }
}

/* ------------------------------------------------------------------ */

/* IFACE=<name> from CLI args or icon tooltypes */
static void readArgs(struct App *a, int argc, char **argv)
{
    int i;
    if (argc > 0) {
        for (i = 1; i < argc; i++) {
            if (strncmp(argv[i], "IFACE=", 6) == 0) {
                strncpy(a->iface, argv[i] + 6, IFACEMAX);
                a->iface[IFACEMAX] = 0;
            }
        }
    } else if (IconBase) {
        struct WBStartup *wbs = (struct WBStartup *)argv;
        if (wbs && wbs->sm_NumArgs > 0) {
            struct DiskObject *dob;
            BPTR old = CurrentDir(wbs->sm_ArgList[0].wa_Lock);
            dob = GetDiskObject((STRPTR)wbs->sm_ArgList[0].wa_Name);
            if (dob) {
                STRPTR v = FindToolType((CONST_STRPTR *)dob->do_ToolTypes,
                                        (STRPTR)"IFACE");
                if (v) {
                    strncpy(a->iface, (char *)v, IFACEMAX);
                    a->iface[IFACEMAX] = 0;
                }
                FreeDiskObject(dob);
            }
            CurrentDir(old);
        }
    }
}

int main(int argc, char **argv)
{
    int rc = 20;
    memset(&app, 0, sizeof(app));
    initList(&app.lines);
    strcpy(app.target, "8.8.8.8");
    strcpy(app.line1, "Status: checking...");
    strcpy(app.line2, "Iface:  ...");
    strcpy(app.msg,   "starting");

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    CxBase        = OpenLibrary("commodities.library", 37);
    IconBase      = OpenLibrary("icon.library", 36);      /* optional */
    if (!IntuitionBase || !GfxBase || !GadToolsBase || !CxBase) goto done;

    readArgs(&app, argc, argv);
    detectIface(&app);

    if (!setupBroker(&app)) goto done;
    if (!setupTimer(&app))  goto done;   /* async polling needs the timer */

    openWin(&app);
    mainLoop(&app);
    rc = 0;

done:
    closeWin(&app);
    cleanupTimer(&app);
    if (app.broker) DeleteCxObjAll(app.broker);
    if (app.cxPort) {
        struct Message *m;
        while ((m = GetMsg(app.cxPort))) ReplyMsg(m);
        DeleteMsgPort(app.cxPort);
    }
    DeleteFile((STRPTR)SCRIPTFILE);
    DeleteFile((STRPTR)DONEFILE);
    DeleteFile((STRPTR)OUTFILE);
    if (IconBase)      CloseLibrary(IconBase);
    if (CxBase)        CloseLibrary(CxBase);
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
