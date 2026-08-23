# Roadster

A native AmigaOS Commodity that switches a Roadshow TCP/IP interface on and
off, with a live status readout and Ping / TraceRoute built in.

Written to replace [Roadie](http://aminet.net/package/comm/net/Roadie) on
machines where its AmiBlitz3 runtime is unstable — on a GVP A530 (68EC030,
no FPU) Roadie gurus with `#8000002B` or hard-resets whenever the CPU data
cache is enabled, which is the boot default. Roadster is plain C + GadTools:
no FPU use, no Blitz runtime, runs on any 68000+ with Kickstart 2.0+.

![window](docs/window.png)

## What it does

- **Status line** — interface Up/Down and current IP, refreshed every 15 s
  and after every action. Maintains `ENV:RoadshowOnline` (`Online`/`Offline`)
  like Roadie did, for OS 3.2 custom title bars.
- **Offline / Online** — `ConfigureNetInterface <iface> OFFLINE` /
  `AddNetInterface` + `ConfigureNetInterface ONLINE UP`. Deliberately *not*
  `NetShutdown`: a full stack teardown blocks on every program holding a
  socket open, and Roadshow then defers the teardown to fire asynchronously
  later — a reliable way to crash a busy machine. Interface-offline stops
  traffic without ripping the stack out from under anything.
- **Status button** — full `ShowNetStatus` dump into the scrolling list.
- **Ping / Trace** — `ping -c 4` / `traceroute` against the Target host
  (default 8.8.8.8). Commands run **asynchronously**: the GUI never freezes,
  a slow traceroute just ghosts the buttons until it lands.
- **Commodity** — shows in Exchange, hotkey `ctrl alt r` toggles the window,
  the close gadget hides it (Exchange *Remove* or Ctrl-C quits).

## Interface selection

The interface name is auto-detected as the first file in
`DEVS:NetInterfaces/`. Override with `IFACE=<name>` — as a CLI argument or
an icon tooltype. Works with any Roadshow interface type (Ethernet, SLIP…).

## Building

`./build.sh` under WSL with the amiga-gcc (bebbo) toolchain on PATH.
Strip at link time (`gcc -s`); the standalone `m68k-amigaos-strip` corrupts
hunk relocs on this toolchain.

## Files

- `src/roadster.c` — the whole program
- `build.sh` — WSL build script
- icon: generated with `C:\projects\tools\makeicon_app.py <out> R`
