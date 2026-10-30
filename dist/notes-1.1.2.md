Menus and settings pages. Roadster does the same job as before, and now everything it can be set to is in a Settings menu.

- **Menu bar** (right mouse button): Project (About, Hide, Quit), Network (Online, Offline, Full status, Ping, Trace, Clear output) and Settings. The Network items have Amiga-key shortcuts and are ghosted while a command runs, like the buttons.
- **Settings > Connection**: the interface, the ping target and the status refresh interval (0 = off).
- **Settings > Commodity**: the hotkey, the commodity priority and whether the window opens at start. These were fixed before.
- Each page has **Save** (`ENV:` and `ENVARC:Roadster.prefs`), **Use** (`ENV:` only, until reboot), **Defaults** and **Cancel**. Changes take effect at once.
- The same keys still work as tooltypes and CLI arguments. Once you have saved from the Settings menu, `Roadster.prefs` wins over them; delete it to go back to the tooltypes.
- The window opens at 400x180 (it was 480x236) and the pages fit a 640x200 screen with Topaz 8. It can still be sized up.
- About shows the licence: Roadster is GPL-3.0, and the `LICENSE` file is now in the drawer.
