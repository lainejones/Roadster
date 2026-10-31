# Roadster

A native AmigaOS Commodity that switches a Roadshow TCP/IP interface on and
off, with a live status readout and Ping / TraceRoute built in.

It fills the same role as Graham Thomson's
[Roadie](http://aminet.net/package/comm/net/Roadie), which inspired it.
Roadster is written from scratch in plain C + GadTools and runs on any
68000+ with Kickstart 2.0+ — it started life chasing crashes on one
particular accelerator setup here, and grew into a full replacement.

![window](docs/window.png)

*(live on the Amiga 500 it was written for; icon below)*

![icon](docs/icon.png)

## What it does

- **Status line** — interface Up/Down and current IP, refreshed every 15 s
  and after every action. Maintains `ENV:RoadshowOnline` (`Online`/`Offline`)
  like Roadie did, for OS 3.2 custom title bars.
- **Offline / Online** — `RemoveNetInterface <iface> FORCE` /
  `AddNetInterface DEVS:NetInterfaces/<iface>`, the pair verified to
  round-trip on hardware. Deliberately *not* `NetShutdown` (a full stack
  teardown blocks on every program holding a socket, and Roadshow then defers
  it to fire asynchronously later — a reliable way to crash a busy machine),
  and not `ConfigureNetInterface ONLINE/OFFLINE` (an interface taken OFFLINE
  could not be revived by ONLINE UP on any machine tested). Remove/Add keeps
  the stack up and re-runs the interface's own config file, like the boot path.
- **Status button** — full `ShowNetStatus` dump into the scrolling list.
- **Ping / Trace** — `ping -c 4` / `traceroute` against the Target host
  (default 8.8.8.8). Commands run **asynchronously**: the GUI never freezes,
  a slow traceroute just ghosts the buttons until it lands.
- **Commodity** — shows in Exchange, hotkey `ctrl alt r` toggles the window,
  the close gadget hides it (Exchange *Remove* or Ctrl-C quits).

## Menus (v1.1.2)

Hold the right mouse button over the window:

| Menu | Item | Key | What it does |
|---|---|---|---|
| Project | About... | | name, version, what it is, licence |
| | Hide | Amiga-H | closes the window; the commodity keeps running |
| | Quit | Amiga-Q | quits Roadster |
| Network | Online | Amiga-O | same as the Online button |
| | Offline | Amiga-F | same as the Offline button |
| | Full status | Amiga-S | same as the Status button |
| | Ping / Trace | Amiga-P / Amiga-T | ping / traceroute the Target host |
| | Clear output | Amiga-K | empties the output list |
| Settings | Connection... | | interface, ping target, refresh interval |
| | Commodity... | | hotkey, priority, open window at start |

The Network items are ghosted while a command runs, like the buttons.
Roadster has no update check, so there is no *Check for updates* item.

## Settings

Each Settings page has **Save** (writes `ENV:Roadster.prefs` and
`ENVARC:Roadster.prefs`, kept across reboots), **Use** (`ENV:` only, until
reboot), **Defaults** (fills in the built-in values) and **Cancel** (Esc).
Changes take effect at once, no restart needed.

| Page | Field | Key | Default | Controls |
|---|---|---|---|---|
| Connection | Interface | `IFACE` | *(empty)* | the Roadshow interface; empty = first file in `DEVS:NetInterfaces/` |
| | Ping target | `TARGET` | `8.8.8.8` | host for Ping / Trace (the Target field starts with it) |
| | Refresh (s) | `REFRESH` | `15` | status refresh while the window is open; `0` = off, else 5..3600 |
| Commodity | Hotkey | `CX_POPKEY` | `ctrl alt r` | shows/hides the window (a bad description keeps the old hotkey) |
| | Priority | `CX_PRIORITY` | `0` | commodity priority, -128..127 |
| | Open window at start | `CX_POPUP` | `YES` | `NO` starts hidden (Exchange *Show* or the hotkey opens it) |

The same keys work as **icon tooltypes** and as `KEY=value` **CLI
arguments** (`Roadster IFACE=scsidayna "CX_POPKEY=ctrl alt n"`).
Precedence, lowest first: built-in defaults, then tooltypes / CLI
arguments, then the settings file (`ENV:` first, else `ENVARC:`). The file
holds every key, so once you have saved from the Settings menu it wins
over the tooltypes; delete `ENV:Roadster.prefs` and `ENVARC:Roadster.prefs`
to go back to the tooltypes. The file is plain `KEY=value` lines.

Pages are sized from the screen font and fit a 640x200 NTSC screen with
Topaz 8.

## ARexx port `ROADSTER` (v1.1)

Commands: `ONLINE`, `OFFLINE`, `SHOW`, `HIDE`, `STATUS` (returns the status
line as the result string), `QUIT`. rc 5 = busy with another command,
rc 10 = unknown command. Needs RexxMast running.

```rexx
/* example */
address ROADSTER
options results
STATUS
say result      /* e.g. "Status: ONLINE   IP 192.168.1.36" */
```

## Efficiency

Built `-Os`, ~20 KB binary (12 KB before the 1.1.2 menus and settings pages), no dynamic allocation in normal operation (the
listview is a fixed ring pool). The heartbeat timer only runs while a command
is in flight or the window is open - a hidden idle commodity sleeps completely.

## Interface selection

The interface name is auto-detected as the first file in
`DEVS:NetInterfaces/`. Override with `IFACE=<name>` — as a CLI argument, an
icon tooltype or on the Settings > Connection page. Works with any Roadshow
interface type (Ethernet, SLIP…).

## Install

The release archive holds `Roadster.readme` and a `Roadster` drawer
(`Roadster`, `Roadster.info`, `Install_Roadster` with its icon, and `LICENSE`), with a
plain drawer icon `Roadster.info` beside the drawer so it shows on Workbench. Needs
Kickstart 2.0+ and Roadshow.

Unpack the archive anywhere (`LhA x Roadster-1.1.3.lha RAM:`), open the Roadster drawer and
double-click **Install_Roadster**. It asks where the Roadster drawer should go (default
`SYS:`, so it becomes `SYS:Roadster`), copies Roadster, its icon, `Roadster.readme` and
`LICENSE` there, and gives the new drawer your Workbench's own drawer icon. Run it again to
update: your settings (`ENVARC:Roadster.prefs`) are kept, and so is the `Roadster.info`
already there, with any tooltypes you gave it. Quit a running Roadster before you start
the new one.

Roadster also runs straight from the unpacked drawer, or copy the drawer anywhere by hand
and double-click `Roadster`. The interface name is auto-detected from
`DEVS:NetInterfaces/` - override with an `IFACE=<name>` tooltype or CLI argument if you
have several.

Use the **`.lha`**: a `.zip` can't carry AmigaDOS protection bits, so after
unpacking a `.zip` run `protect Roadster +e` in the drawer (the installer sets it on the
copy it installs).

## Building

`./build.sh` under WSL with the amiga-gcc (bebbo) toolchain on PATH. Then
`python3 tools/mkpkg.py` stages the release in `out/release/` (readme,
drawer, icons, installer, licence).
Strip at link time (`gcc -s`); the standalone `m68k-amigaos-strip` corrupts
hunk relocs on this toolchain.

## Files

- `src/roadster.c` — the program
- `src/amigui.c`, `src/amigui.h` — shared About box / settings-page / ENV: helpers
- `build.sh` — WSL build script
- `tools/mkpkg.py` — stages the release layout in `out/release/`
- `Install_Roadster` — the Installer script shipped in the drawer
- `icons/` — `Roadster.info` (the program's GlowIcon), `Roadster-drawer.info`
  (a plain blue drawer from `../tools/makeicon_drawer32.py`, shipped as `Roadster.info`
  beside the drawer) and `Install_Roadster.info` (the installer's icon: default tool
  `Installer`, tooltypes `APPNAME=Roadster` and `DEFUSER=AVERAGE`)
- `Roadster-1.1.lha` / `Roadster.readme` — the 1.1 release archive (built with LhA
  on a real Amiga) and its Aminet-style readme
- `docs/` — screenshots; the GlowIcon ships inside the archive
- `LICENSE` — the GNU GPL version 3

## Licence

GPL-3.0 (the GNU General Public License, version 3) - see
[`LICENSE`](LICENSE). Project > About... says the same.
