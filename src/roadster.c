/*
 * Roadster - a native Roadshow on/off switch + status panel (Commodity)
 *
 * Fills the same role as Roadie (which inspired it); written from scratch
 * after chasing crashes on this machine's 68EC030 that turned out to be
 * data-cache related (see the daynaport project notes, 2026-08-22).
 * Plain C + GadTools, runs on any 68000+ with OS 2.0+.
 *
 *  +--------------------------------------------------------------+
 *  | Status: ONLINE   IP 192.168.1.36                            |
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
 *  - Offline = RemoveNetInterface FORCE, Online = AddNetInterface: the pair
 *    verified to round-trip on hardware. Deliberately NOT NetShutdown: a full
 *    stack teardown blocks on every program holding a bsdsocket socket
 *    (smb2fs, netharness, ...), and Roadshow then DEFERS the teardown, which
 *    fires asynchronously later - the class of crash that took the A500 down
 *    repeatedly. And not ConfigureNetInterface ONLINE/OFFLINE - an interface
 *    taken OFFLINE could not be revived by ONLINE UP on either test machine.
 *  - ARexx port "ROADSTER" (v1.1): ONLINE / OFFLINE / SHOW / HIDE / STATUS /
 *    QUIT, so DOpus buttons and scripts can drive it like Roadie's port.
 *    STATUS returns the status line as the ARexx result.
 *  - Commands run ASYNCHRONOUSLY: we write T:Roadster.script ending in an
 *    "Echo >T:Roadster.done" sentinel, SystemTags() it with SYS_Asynch, and
 *    a timer tick polls for the sentinel. The GUI never freezes, even during
 *    a slow traceroute.
 *  - The interface name is auto-detected from DEVS:NetInterfaces/ (first
 *    entry), overridable with an IFACE=<name> CLI argument or icon tooltype.
 *  - ENV:RoadshowOnline is maintained like Roadie did (OS 3.2 title-bar use).
 *  - Menus (v1.2): Project (About / Hide / Quit), Network (the actions, with
 *    Amiga-key shortcuts) and Settings (two pages, Save / Use / Defaults /
 *    Cancel, built on amigui.c).
 *
 * SETTINGS AND THEIR PRECEDENCE (lowest first, each one overrides the last):
 *    1. built-in defaults
 *    2. icon tooltypes (Workbench start) or KEY=value CLI arguments
 *    3. the settings file ENV:Roadster.prefs (else ENVARC:Roadster.prefs),
 *       written by Settings -> Save (ENV: + ENVARC:) or Use (ENV: only)
 *  Keys, the same in all three places:
 *    IFACE=<name>        interface (empty = first file in DEVS:NetInterfaces)
 *    TARGET=<host>       Ping / Trace target                (8.8.8.8)
 *    REFRESH=<secs>      status refresh while the window is open, 0 = off (15)
 *    CX_POPKEY=<keys>    hotkey that shows/hides the window (ctrl alt r)
 *    CX_PRIORITY=<n>     commodity priority, -128..127      (0)
 *    CX_POPUP=YES|NO     open the window at start           (YES)
 *  The file holds every key, so once saved it wins over the tooltypes; delete
 *  it from ENV: and ENVARC: to go back to the tooltypes.
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
#include <rexx/storage.h>
#include <rexx/rxslib.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/commodities.h>
#include <proto/icon.h>

#include <string.h>

#include "amigui.h"

#define ROADSTER_VERSION "1.2"
#define ROADSTER_DATE    "30.10.2026"
#define ROADSTER_ABOUT   "A Commodity that switches a Roadshow TCP/IP\n" \
                         "interface on and off, with status, ping and trace.\n\n" \
                         "Licence: not yet stated (see the README)"

static const char verstag[] __attribute__((used)) =
    "$VER: Roadster " ROADSTER_VERSION " (" ROADSTER_DATE ")";

unsigned long __stack = 20000;

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *CxBase        = NULL;
struct Library       *IconBase      = NULL;
struct RxsLib        *RexxSysBase   = NULL;   /* optional: ARexx result strings */

#include <proto/rexxsyslib.h>

/* ------------------------------------------------------------------ */

#define OUTFILE   "T:Roadster.out"
#define DONEFILE  "T:Roadster.done"
#define SCRIPTFILE "T:Roadster.script"

#define MAXLINES  250              /* listview line pool                */
#define LINELEN   78
#define HOSTMAX   60
#define IFACEMAX  32

#define POLLSECS  1                /* sentinel poll while busy          */
#define KEYMAX    64

#define PREFSFILE "Roadster.prefs"     /* in ENV: / ENVARC:             */

/* built-in defaults (precedence level 1, see the top of the file) */
#define DEF_TARGET  "8.8.8.8"
#define DEF_REFRESH 15             /* auto status refresh when idle     */
#define DEF_POPKEY  "ctrl alt r"
#define DEF_PRI     0
#define DEF_POPUP   TRUE

#define EVT_HOTKEY 1L

enum {                             /* menu user data                    */
    MN_ABOUT = 1, MN_HIDE, MN_QUIT,
    MN_ONLINE, MN_OFFLINE, MN_STATUS, MN_PING, MN_TRACE, MN_CLEAR,
    MN_SET_NET, MN_SET_CX
};
#define NETMENU      1             /* menu number of "Network"          */
#define NETMENU_ACTS 5             /* its first 5 items run commands    */

static struct NewMenu menuDef[] = {
    {NM_TITLE, (STRPTR)"Project",        NULL, 0, 0, NULL},
    {NM_ITEM,  (STRPTR)"About...",       NULL, 0, 0, (APTR)MN_ABOUT},
    {NM_ITEM,  NM_BARLABEL,              NULL, 0, 0, NULL},
    {NM_ITEM,  (STRPTR)"Hide",   (STRPTR)"H",  0, 0, (APTR)MN_HIDE},
    {NM_ITEM,  (STRPTR)"Quit",   (STRPTR)"Q",  0, 0, (APTR)MN_QUIT},
    {NM_TITLE, (STRPTR)"Network",        NULL, 0, 0, NULL},
    {NM_ITEM,  (STRPTR)"Online",      (STRPTR)"O", 0, 0, (APTR)MN_ONLINE},
    {NM_ITEM,  (STRPTR)"Offline",     (STRPTR)"F", 0, 0, (APTR)MN_OFFLINE},
    {NM_ITEM,  (STRPTR)"Full status", (STRPTR)"S", 0, 0, (APTR)MN_STATUS},
    {NM_ITEM,  (STRPTR)"Ping",        (STRPTR)"P", 0, 0, (APTR)MN_PING},
    {NM_ITEM,  (STRPTR)"Trace",       (STRPTR)"T", 0, 0, (APTR)MN_TRACE},
    {NM_ITEM,  NM_BARLABEL,              NULL, 0, 0, NULL},
    {NM_ITEM,  (STRPTR)"Clear output", (STRPTR)"K", 0, 0, (APTR)MN_CLEAR},
    {NM_TITLE, (STRPTR)"Settings",       NULL, 0, 0, NULL},
    {NM_ITEM,  (STRPTR)"Connection...",  NULL, 0, 0, (APTR)MN_SET_NET},
    {NM_ITEM,  (STRPTR)"Commodity...",   NULL, 0, 0, (APTR)MN_SET_CX},
    {NM_END,   NULL,                     NULL, 0, 0, NULL}
};

/* what can be set (tooltypes / CLI / settings file / Settings pages) */
struct Cfg {
    char iface[IFACEMAX + 2];      /* empty = auto-detect                */
    char target[HOSTMAX + 2];
    LONG refresh;                  /* seconds, 0 = off                   */
    char popkey[KEYMAX + 2];
    LONG pri;
    BOOL popup;
};

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
    CxObj          *hotkey;        /* the CX_POPKEY filter              */

    struct Cfg      cfg;

    /* ARexx */
    struct MsgPort *rxPort;        /* public port "ROADSTER"            */

    /* timer */
    struct MsgPort *timerPort;
    struct timerequest *timerIO;
    BOOL            timerOpen, timerRunning;

    /* gui */
    struct Screen  *scr;
    APTR            vi;
    struct Window  *win;
    struct Menu    *menu;
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
            int len = (int)strlen(fib->fib_FileName);
            /* skip icon files: an "X-Surf.info" next to "X-Surf" is NOT an
             * interface, and ExNext order is filesystem-dependent */
            if (len > 5) {
                const char *e = fib->fib_FileName + len - 5;
                if ((e[0]=='.') &&
                    (e[1]=='i'||e[1]=='I') && (e[2]=='n'||e[2]=='N') &&
                    (e[3]=='f'||e[3]=='F') && (e[4]=='o'||e[4]=='O'))
                    continue;
            }
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
/* scsidayna 1500 Ethernet 192.168.1.36  1850   401     0 ...     Up */
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
    if (!a->target[0]) strcpy(a->target, a->cfg.target[0] ? a->cfg.target : DEF_TARGET);
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
    int i;
    if (!a->win) return;
    if (a->menu)                       /* ghost the matching menu items too */
        for (i = 0; i < NETMENU_ACTS; i++) {
            if (on) OnMenu(a->win, FULLMENUNUM(NETMENU, i, NOSUB));
            else    OffMenu(a->win, FULLMENUNUM(NETMENU, i, NOSUB));
        }
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
        if (a->menu) ClearMenuStrip(a->win);
        if (a->glist) RemoveGList(a->win, a->glist, -1);
        CloseWindow(a->win);
        a->win = NULL;
    }
    if (a->menu) { FreeMenus(a->menu); a->menu = NULL; }
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
                             IDCMP_NEWSIZE | IDCMP_MENUPICK | BUTTONIDCMP |
                             STRINGIDCMP | LISTVIEWIDCMP,
        (ULONG)WA_PubScreen, (ULONG)a->scr,
        (ULONG)WA_NewLookMenus, TRUE,    /* V39 tag; ignored on 2.x */
        TAG_END);
    if (!a->win) {
        FreeVisualInfo(a->vi); a->vi = NULL;
        UnlockPubScreen(NULL, a->scr); a->scr = NULL;
        return;
    }
    /* the menu strip; GTMN_NewLookMenus is V39 and ignored on 2.x */
    if ((a->menu = CreateMenus(menuDef, TAG_END)) != NULL) {
        if (LayoutMenus(a->menu, a->vi, (ULONG)GTMN_NewLookMenus, TRUE, TAG_END))
            SetMenuStrip(a->win, a->menu);
        else { FreeMenus(a->menu); a->menu = NULL; }
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

/* (Re)install the show/hide hotkey. A bad key description leaves the old
 * hotkey in place and returns FALSE. */
static BOOL setHotkey(struct App *a, const char *desc)
{
    CxObj *filter, *sender, *translate;

    filter = CxFilter((STRPTR)desc);
    if (!filter) return FALSE;
    sender = CxSender(a->cxPort, EVT_HOTKEY);
    if (sender) AttachCxObj(filter, sender);
    translate = CxTranslate(NULL);
    if (translate) AttachCxObj(filter, translate);
    if (CxObjError(filter) != 0 || !sender || !translate) {
        DeleteCxObjAll(filter);
        return FALSE;
    }
    if (a->hotkey) DeleteCxObjAll(a->hotkey);   /* unlinks it from the broker */
    a->hotkey = filter;
    AttachCxObj(a->broker, filter);
    return TRUE;
}

static BOOL setupBroker(struct App *a)
{
    struct NewBroker nb;

    a->cxPort = CreateMsgPort();
    if (!a->cxPort) return FALSE;

    memset(&nb, 0, sizeof(nb));
    nb.nb_Version = NB_VERSION;
    nb.nb_Name    = (BYTE *)"Roadster";
    nb.nb_Title   = (BYTE *)"Roadster " ROADSTER_VERSION;
    nb.nb_Descr   = (BYTE *)"Roadshow online/offline switch";
    nb.nb_Unique  = NBU_UNIQUE | NBU_NOTIFY;
    nb.nb_Flags   = COF_SHOW_HIDE;
    nb.nb_Port    = a->cxPort;
    nb.nb_Pri     = (BYTE)a->cfg.pri;

    a->broker = CxBroker(&nb, NULL);
    if (!a->broker) return FALSE;

    if (!setHotkey(a, a->cfg.popkey))           /* bad CX_POPKEY: default */
        setHotkey(a, DEF_POPKEY);

    ActivateCxObj(a->broker, 1L);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* ARexx port "ROADSTER"                                              */
/* ------------------------------------------------------------------ */

static BOOL setupRexx(struct App *a)
{
    a->rxPort = CreateMsgPort();
    if (!a->rxPort) return FALSE;
    a->rxPort->mp_Node.ln_Name = (char *)"ROADSTER";
    a->rxPort->mp_Node.ln_Pri  = 0;
    AddPort(a->rxPort);
    return TRUE;
}

static void cleanupRexx(struct App *a)
{
    struct RexxMsg *rm;
    if (!a->rxPort) return;
    RemPort(a->rxPort);
    /* anything still queued must be replied, never deleted */
    while ((rm = (struct RexxMsg *)GetMsg(a->rxPort))) {
        rm->rm_Result1 = 20; rm->rm_Result2 = 0;
        ReplyMsg((struct Message *)rm);
    }
    DeleteMsgPort(a->rxPort);
    a->rxPort = NULL;
}

/* case-insensitive keyword match against the start of the command,
 * ending at NUL or whitespace */
static BOOL rxIs(const char *cmd, const char *kw)
{
    while (*kw) {
        char c = *cmd++, k = *kw++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != k) return FALSE;
    }
    return (*cmd == 0 || *cmd == ' ' || *cmd == '\t' || *cmd == '\n');
}

static void handleRexx(struct App *a)
{
    struct RexxMsg *rm;
    while ((rm = (struct RexxMsg *)GetMsg(a->rxPort))) {
        const char *cmd = (const char *)ARG0(rm);
        LONG rc = 0;
        const char *result = NULL;

        if (!cmd) {
            rc = 20;
        } else if (rxIs(cmd, "ONLINE")) {
            if (a->action != ACT_NONE) rc = 5;         /* busy: try again  */
            else doOnline(a);
        } else if (rxIs(cmd, "OFFLINE")) {
            if (a->action != ACT_NONE) rc = 5;
            else doOffline(a);
        } else if (rxIs(cmd, "SHOW")) {
            openWin(a);
        } else if (rxIs(cmd, "HIDE")) {
            closeWin(a);
        } else if (rxIs(cmd, "STATUS")) {
            /* stack-down is knowable instantly; otherwise report the last
             * refreshed headline (kept current by the idle poll / actions) */
            if (!stackUp()) result = "OFFLINE (stack down)";
            else            result = a->line1;
        } else if (rxIs(cmd, "QUIT")) {
            a->quit = TRUE;
        } else {
            rc = 10;                                    /* unknown command  */
        }

        rm->rm_Result1 = rc;
        rm->rm_Result2 = 0;
        if (rc == 0 && result && RexxSysBase &&
            (rm->rm_Action & RXFF_RESULT))
            rm->rm_Result2 = (LONG)CreateArgstring((STRPTR)result,
                                                   (ULONG)strlen(result));
        ReplyMsg((struct Message *)rm);
    }
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

/* ------------------------------------------------------------------ */
/* settings: defaults < tooltypes / CLI args < ENV:Roadster.prefs     */
/* (see the top of the file)                                          */
/* ------------------------------------------------------------------ */

static void copyStr(char *d, const char *s, int max)
{
    int n;
    strncpy(d, s, max);
    d[max] = 0;
    n = (int)strlen(d);                    /* no trailing blanks / CR     */
    while (n > 0 && (d[n-1] == ' ' || d[n-1] == '\t' || d[n-1] == '\r'))
        d[--n] = 0;
}

static BOOL keyIs(const char *s, const char *kw)   /* case-insensitive    */
{
    while (*kw) {
        char c = *s++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != *kw++) return FALSE;
    }
    return *s == 0;
}

static LONG toLong(const char *s, LONG def)
{
    LONG v;
    return (StrToLong((STRPTR)s, &v) > 0) ? v : def;
}

static void cfgDefaults(struct Cfg *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->target, DEF_TARGET);
    c->refresh = DEF_REFRESH;
    strcpy(c->popkey, DEF_POPKEY);
    c->pri     = DEF_PRI;
    c->popup   = DEF_POPUP;
}

static void cfgClamp(struct Cfg *c)
{
    if (c->refresh < 0) c->refresh = 0;
    if (c->refresh > 0 && c->refresh < 5) c->refresh = 5;
    if (c->refresh > 3600) c->refresh = 3600;
    if (c->pri < -128) c->pri = -128;
    if (c->pri > 127)  c->pri = 127;
    if (!c->target[0]) strcpy(c->target, DEF_TARGET);
    if (!c->popkey[0]) strcpy(c->popkey, DEF_POPKEY);
}

static const char *cfgKeys[] = {
    "IFACE", "TARGET", "REFRESH", "CX_POPKEY", "CX_PRIORITY", "CX_POPUP"
};

static void cfgSet(struct Cfg *c, const char *key, const char *v)
{
    while (*v == ' ' || *v == '\t') v++;
    if      (keyIs(key, "IFACE"))       copyStr(c->iface, v, IFACEMAX);
    else if (keyIs(key, "TARGET"))      copyStr(c->target, v, HOSTMAX);
    else if (keyIs(key, "REFRESH"))     c->refresh = toLong(v, c->refresh);
    else if (keyIs(key, "CX_POPKEY"))   copyStr(c->popkey, v, KEYMAX);
    else if (keyIs(key, "CX_PRIORITY")) c->pri = toLong(v, c->pri);
    else if (keyIs(key, "CX_POPUP")) {
        char ch = v[0];
        c->popup = !(ch == 'N' || ch == 'n' || ch == 'F' || ch == 'f' ||
                     ch == '0' || keyIs(v, "OFF"));
    }
}

/* "KEY=value" (a CLI argument or a line of the settings file) */
static void cfgLine(struct Cfg *c, const char *line)
{
    char key[16];
    int i = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (*line && *line != '=' && i < (int)sizeof(key) - 1) key[i++] = *line++;
    if (*line != '=') return;
    while (i > 0 && (key[i-1] == ' ' || key[i-1] == '\t')) i--;
    key[i] = 0;
    cfgSet(c, key, line + 1);
}

/* precedence level 2: icon tooltypes or KEY=value CLI arguments */
static void readArgs(struct App *a, int argc, char **argv)
{
    int i;
    if (argc > 0) {
        for (i = 1; i < argc; i++)
            cfgLine(&a->cfg, argv[i]);
    } else if (IconBase) {
        struct WBStartup *wbs = (struct WBStartup *)argv;
        if (wbs && wbs->sm_NumArgs > 0) {
            struct DiskObject *dob;
            BPTR old = CurrentDir(wbs->sm_ArgList[0].wa_Lock);
            dob = GetDiskObject((STRPTR)wbs->sm_ArgList[0].wa_Name);
            if (dob) {
                for (i = 0; i < (int)(sizeof(cfgKeys) / sizeof(cfgKeys[0])); i++) {
                    STRPTR v = FindToolType((CONST_STRPTR *)dob->do_ToolTypes,
                                            (STRPTR)cfgKeys[i]);
                    if (v) cfgSet(&a->cfg, cfgKeys[i], (const char *)v);
                }
                FreeDiskObject(dob);
            }
            CurrentDir(old);
        }
    }
}

/* precedence level 3: ENV:Roadster.prefs, else ENVARC:Roadster.prefs */
static void readPrefsFile(struct Cfg *c)
{
    static char buf[512];
    char *p;
    if (amg_env_read(PREFSFILE, buf, sizeof(buf)) < 0) return;
    for (p = buf; *p; ) {
        char *e = p;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        cfgLine(c, p);
        p = e;
    }
}

static void putKV(char *buf, const char *key, const char *val)
{
    strcat(buf, key); strcat(buf, "="); strcat(buf, val); strcat(buf, "\n");
}

static void ltoa10(LONG v, char *out)
{
    char tmp[12]; int n = 0;
    ULONG u = (v < 0) ? (ULONG)-v : (ULONG)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *out++ = '-';
    while (n) *out++ = tmp[--n];
    *out = 0;
}

static BOOL writePrefsFile(struct Cfg *c, BOOL keep)
{
    static char buf[400];
    char num[12];
    buf[0] = 0;
    putKV(buf, "IFACE", c->iface);
    putKV(buf, "TARGET", c->target);
    ltoa10(c->refresh, num); putKV(buf, "REFRESH", num);
    putKV(buf, "CX_POPKEY", c->popkey);
    ltoa10(c->pri, num);     putKV(buf, "CX_PRIORITY", num);
    putKV(buf, "CX_POPUP", c->popup ? "YES" : "NO");
    return amg_env_write(PREFSFILE, buf, (ULONG)strlen(buf), keep) != 0;
}

/* take a new configuration into the running program */
static void applyCfg(struct App *a, const struct Cfg *old)
{
    if (strcmp(a->cfg.iface, old->iface) != 0) {
        strcpy(a->iface, a->cfg.iface);
        detectIface(a);                      /* empty = auto-detect again */
        if (a->action == ACT_NONE) startRefresh(a, ACT_REFRESH);
    }
    if (strcmp(a->cfg.target, old->target) != 0) {
        strcpy(a->target, a->cfg.target);
        if (a->win && a->gTarget)
            GT_SetGadgetAttrs(a->gTarget, a->win, NULL,
                              (ULONG)GTST_String, (ULONG)a->target, TAG_END);
    }
    a->idleTicks = 0;
    if (strcmp(a->cfg.popkey, old->popkey) != 0 && !setHotkey(a, a->cfg.popkey)) {
        strcpy(a->cfg.popkey, old->popkey);  /* keep the hotkey that works */
        setMsg(a, "bad hotkey - kept the old one");
    }
    if (a->cfg.pri != old->pri && a->broker)
        SetCxObjPri(a->broker, a->cfg.pri);
}

/* ---- the Settings pages ------------------------------------------ */

enum { FK_STR, FK_INT, FK_CHECK };

struct Field {
    const char *label;
    int kind;
    int maxc;
};

#define PG_FIELDS 3
#define PG_STRLEN (KEYMAX + 2)

struct PageVals {
    char s[PG_FIELDS][PG_STRLEN];        /* FK_STR                         */
    LONG n[PG_FIELDS];                   /* FK_INT and FK_CHECK            */
};

struct PageCtx {
    const struct Field *f;
    const struct PageVals *def;
};

static const struct Field netFields[PG_FIELDS] = {
    {"Interface",   FK_STR, IFACEMAX},
    {"Ping target", FK_STR, HOSTMAX},
    {"Refresh (s)", FK_INT, 4}
};
static const struct Field cxFields[PG_FIELDS] = {
    {"Hotkey",      FK_STR, KEYMAX},
    {"Priority",    FK_INT, 4},
    {"Open window at start", FK_CHECK, 0}
};

static void cfgToNet(const struct Cfg *c, struct PageVals *v)
{
    memset(v, 0, sizeof(*v));
    strcpy(v->s[0], c->iface); strcpy(v->s[1], c->target); v->n[2] = c->refresh;
}

static void cfgToCx(const struct Cfg *c, struct PageVals *v)
{
    memset(v, 0, sizeof(*v));
    strcpy(v->s[0], c->popkey); v->n[1] = c->pri; v->n[2] = c->popup;
}

static void setField(AmgPage *pg, const struct Field *f, int i, const struct PageVals *v)
{
    switch (f[i].kind) {
    case FK_STR:
        GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, (ULONG)GTST_String, (ULONG)v->s[i], TAG_END);
        break;
    case FK_INT:
        GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, (ULONG)GTIN_Number, (ULONG)v->n[i], TAG_END);
        break;
    case FK_CHECK:
        GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, (ULONG)GTCB_Checked, (ULONG)v->n[i], TAG_END);
        break;
    }
}

static void pageGadget(AmgPage *pg, int id, UWORD code, void *ctx)
{
    struct PageCtx *pc = (struct PageCtx *)ctx;
    int i;
    (void)code;
    if (id == AMG_ID_DEFAULTS)
        for (i = 0; i < PG_FIELDS; i++)
            setField(pg, pc->f, i, pc->def);
}

/* One page: labelled fields, a one-line note, Save / Use / Defaults /
 * Cancel. Sized from the screen font: with Topaz 8 it is about 400 x 100,
 * so it fits a 640x200 NTSC screen. Returns AMG_SAVE / AMG_USE / AMG_CANCEL
 * and, unless cancelled, the edited values in v. */
static int runPage(struct App *a, const char *title, const struct Field *f,
                   const char *note, struct PageVals *v, const struct PageVals *def)
{
    struct RastPort *srp = &a->scr->RastPort;     /* measures in the screen font */
    struct PageCtx pc;
    struct NewGadget ng;
    struct Gadget *gad;
    AmgPage pg;
    WORD fh = srp->TxHeight, bh = fh + 6, rowH = bh + 4;
    WORD labW = 0, strW, intW, w, h, x0, y, chkW = 0;
    int i, res;

    for (i = 0; i < PG_FIELDS; i++) {
        WORD l = TextLength(srp, (STRPTR)f[i].label, strlen(f[i].label));
        if (f[i].kind == FK_CHECK) { if (l + 34 > chkW) chkW = l + 34; }
        else if (l + 8 > labW) labW = l + 8;
    }
    strW = TextLength(srp, (STRPTR)"0", 1) * 28;
    intW = TextLength(srp, (STRPTR)"0", 1) * 7 + 12;
    w = 8 + labW + (strW > chkW ? strW : chkW) + 8;
    i = TextLength(srp, (STRPTR)note, strlen(note)) + 16;
    if (w < i) w = i;
    if (w < amg_min_width(a->scr->Font)) w = amg_min_width(a->scr->Font);
    h = 8 + PG_FIELDS * rowH + fh + 8 + bh + 8;

    if (!amg_page_open(&pg, a->scr, a->vi, a->scr->Font, a->win, title, w, h))
        return AMG_CANCEL;
    w = pg.win->Width - pg.win->BorderLeft - pg.win->BorderRight;   /* may be clipped */

    gad = CreateContext(&pg.glist);
    x0 = pg.win->BorderLeft + 8;
    y  = pg.win->BorderTop + 8;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr   = a->scr->Font;
    ng.ng_VisualInfo = a->vi;
    for (i = 0; i < PG_FIELDS && gad; i++) {
        ng.ng_LeftEdge = x0 + labW; ng.ng_TopEdge = y;
        ng.ng_Height = bh; ng.ng_GadgetText = (STRPTR)f[i].label;
        ng.ng_GadgetID = i + 1; ng.ng_Flags = PLACETEXT_LEFT;
        switch (f[i].kind) {
        case FK_STR:
            ng.ng_Width = strW;
            gad = CreateGadget(STRING_KIND, gad, &ng,
                               (ULONG)GTST_String, (ULONG)v->s[i],
                               (ULONG)GTST_MaxChars, (ULONG)f[i].maxc, TAG_END);
            break;
        case FK_INT:
            ng.ng_Width = intW;
            gad = CreateGadget(INTEGER_KIND, gad, &ng,
                               (ULONG)GTIN_Number, (ULONG)v->n[i],
                               (ULONG)GTIN_MaxChars, (ULONG)f[i].maxc, TAG_END);
            break;
        case FK_CHECK:
            ng.ng_LeftEdge = x0; ng.ng_Width = 26; ng.ng_Height = fh + 3;
            ng.ng_TopEdge = y + (bh - (fh + 3)) / 2;
            ng.ng_Flags = PLACETEXT_RIGHT;
            gad = CreateGadget(CHECKBOX_KIND, gad, &ng,
                               (ULONG)GTCB_Checked, (ULONG)v->n[i], TAG_END);
            break;
        }
        pg.g[i] = gad;
        y += rowH;
    }
    if (gad) {
        ng.ng_LeftEdge = x0; ng.ng_TopEdge = y; ng.ng_Width = w - 16;
        ng.ng_Height = fh + 2; ng.ng_GadgetText = NULL; ng.ng_GadgetID = 0;
        ng.ng_Flags = 0;
        gad = CreateGadget(TEXT_KIND, gad, &ng, (ULONG)GTTX_Text, (ULONG)note, TAG_END);
        y += fh + 8;
    }
    if (gad)
        gad = amg_buttons(&pg, gad, y - pg.win->BorderTop, w, 1);
    if (!gad) { amg_page_close(&pg); return AMG_CANCEL; }

    amg_page_show(&pg);
    pc.f = f; pc.def = def;
    res = amg_page_run(&pg, pageGadget, &pc);
    if (res != AMG_CANCEL)
        for (i = 0; i < PG_FIELDS; i++) {
            if (f[i].kind == FK_STR)
                copyStr(v->s[i], amg_string(pg.g[i]), f[i].maxc);
            else if (f[i].kind == FK_INT)
                v->n[i] = toLong(amg_string(pg.g[i]), v->n[i]);
            else
                v->n[i] = amg_checked(pg.g[i]);
        }
    amg_page_close(&pg);
    return res;
}

static void settingsPage(struct App *a, BOOL commodity)
{
    struct PageVals v, def;
    struct Cfg old = a->cfg, dc;
    struct Requester block;
    BOOL blocked;
    int res;

    if (!a->win) return;
    cfgDefaults(&dc);
    if (commodity) { cfgToCx(&a->cfg, &v); cfgToCx(&dc, &def); }
    else           { cfgToNet(&a->cfg, &v); cfgToNet(&dc, &def); }

    /* an empty requester keeps the main window still while the page is up */
    InitRequester(&block);
    blocked = Request(&block, a->win);

    if (commodity)
        res = runPage(a, "Roadster: Commodity", cxFields,
                      "Hotkey e.g. ctrl alt r   Priority -128..127", &v, &def);
    else
        res = runPage(a, "Roadster: Connection", netFields,
                      "Empty interface = auto. Refresh 0 = off", &v, &def);

    if (blocked) EndRequest(&block, a->win);
    if (res == AMG_CANCEL) return;

    if (commodity) {
        copyStr(a->cfg.popkey, v.s[0], KEYMAX);
        a->cfg.pri = v.n[1]; a->cfg.popup = v.n[2] ? TRUE : FALSE;
    } else {
        copyStr(a->cfg.iface, v.s[0], IFACEMAX);
        copyStr(a->cfg.target, v.s[1], HOSTMAX);
        a->cfg.refresh = v.n[2];
    }
    cfgClamp(&a->cfg);
    applyCfg(a, &old);
    if (!writePrefsFile(&a->cfg, res == AMG_SAVE))
        setMsg(a, "could not write " PREFSFILE);
    else if (res == AMG_SAVE)
        setMsg(a, "settings saved");
    else
        setMsg(a, "settings in use until reboot");
}

/* ---- menu --------------------------------------------------------- */

static void doMenu(struct App *a, ULONG item)
{
    if (item >= MN_ONLINE && item <= MN_TRACE && a->action != ACT_NONE) {
        setMsg(a, "busy - wait for the current command");
        return;
    }
    switch (item) {
    case MN_ABOUT:
        amg_about(a->win, "Roadster", ROADSTER_VERSION, ROADSTER_ABOUT);
        break;
    case MN_HIDE:    closeWin(a); break;      /* commodity keeps running */
    case MN_QUIT:    a->quit = TRUE; break;
    case MN_ONLINE:  doOnline(a);  break;
    case MN_OFFLINE: doOffline(a); break;
    case MN_STATUS:  doShow(a);    break;
    case MN_PING:    doPing(a);    break;
    case MN_TRACE:   doTrace(a);   break;
    case MN_CLEAR:   clearLines(a); setMsg(a, "ready"); break;
    case MN_SET_NET: settingsPage(a, FALSE); break;
    case MN_SET_CX:  settingsPage(a, TRUE);  break;
    }
}

/* ------------------------------------------------------------------ */

static void mainLoop(struct App *a)
{
    ULONG cxSig    = 1UL << a->cxPort->mp_SigBit;
    ULONG timerSig = a->timerPort ? (1UL << a->timerPort->mp_SigBit) : 0;
    ULONG rxSig    = a->rxPort ? (1UL << a->rxPort->mp_SigBit) : 0;

    while (!a->quit) {
        ULONG winSig = a->win ? (1UL << a->win->UserPort->mp_SigBit) : 0;
        ULONG got;

        /* run the heartbeat only while there is something to poll: a command
         * in flight (done-file check) or an open window (status refresh).
         * A hidden idle commodity sleeps completely instead of waking every
         * second to do nothing. */
        if (!a->timerRunning &&
            (a->action != ACT_NONE || (a->win && a->cfg.refresh > 0)))
            armTimer(a);

        got = Wait(cxSig | timerSig | winSig | rxSig | SIGBREAKF_CTRL_C);

        if (got & SIGBREAKF_CTRL_C) a->quit = TRUE;

        if (rxSig && (got & rxSig)) handleRexx(a);

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
                if (a->cfg.refresh > 0 && ++a->idleTicks >= a->cfg.refresh) {
                    a->idleTicks = 0;
                    startRefresh(a, ACT_REFRESH);
                }
            }
            /* re-armed (or not) at the top of the loop */
        }

        if (a->win && (got & winSig)) {
            struct IntuiMessage *im;
            while ((im = GT_GetIMsg(a->win->UserPort))) {
                ULONG cls = im->Class;
                UWORD code = im->Code;
                struct Gadget *gad = (struct Gadget *)im->IAddress;
                GT_ReplyIMsg(im);

                switch (cls) {
                case IDCMP_CLOSEWINDOW:
                    closeWin(a);               /* hide; commodity lives on */
                    break;
                case IDCMP_GADGETUP:
                    handleGadget(a, gad->GadgetID);
                    break;
                case IDCMP_MENUPICK:
                    /* NextSelect is read BEFORE acting: Hide frees the menus */
                    while (code != MENUNULL && a->win && a->menu && !a->quit) {
                        struct MenuItem *it = ItemAddress(a->menu, code);
                        if (!it) break;
                        code = it->NextSelect;
                        doMenu(a, (ULONG)GTMENUITEM_USERDATA(it));
                    }
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

int main(int argc, char **argv)
{
    int rc = 20;
    memset(&app, 0, sizeof(app));
    initList(&app.lines);
    cfgDefaults(&app.cfg);
    strcpy(app.line1, "Status: checking...");
    strcpy(app.line2, "Iface:  ...");
    strcpy(app.msg,   "starting");

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    CxBase        = OpenLibrary("commodities.library", 37);
    IconBase      = OpenLibrary("icon.library", 36);      /* optional */
    RexxSysBase   = (struct RxsLib *)OpenLibrary("rexxsyslib.library", 0);
    if (!IntuitionBase || !GfxBase || !GadToolsBase || !CxBase) goto done;

    readArgs(&app, argc, argv);          /* tooltypes / CLI over defaults  */
    readPrefsFile(&app.cfg);             /* the settings file over those   */
    cfgClamp(&app.cfg);
    strcpy(app.iface, app.cfg.iface);
    strcpy(app.target, app.cfg.target);
    detectIface(&app);

    if (!setupBroker(&app)) goto done;
    if (!setupTimer(&app))  goto done;   /* async polling needs the timer */
    setupRexx(&app);                     /* non-fatal: no port, still works */

    if (app.cfg.popup) openWin(&app);
    else startRefresh(&app, ACT_REFRESH);   /* keep ENV:RoadshowOnline current */
    mainLoop(&app);
    rc = 0;

done:
    closeWin(&app);
    cleanupRexx(&app);
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
    if (RexxSysBase)   CloseLibrary((struct Library *)RexxSysBase);
    if (IconBase)      CloseLibrary(IconBase);
    if (CxBase)        CloseLibrary(CxBase);
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
