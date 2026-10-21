# PC menu startup triage (Windows / MSYS2 CLANG64)

The native System PC menu has two independent startup components:

1. `run.py` launches `scripts/enhancements.py`, which generates
   `out/enhancements/pc-menu/dvdroot_ps4/menu/optionsetting.gfx` and patched
   `msg/*/menu.msgbnd.dcx` from the user's own game files; both title movies
   may also get a Quit Game option. The generated files enter the virtual game
   directory through `scripts/mods.py`.
2. `src/pc_menu.c` installs engine hooks for the System options, the title
   Quit Game option (if present) and the menu-list helpers. These use hardcoded
   locations for the clean CUSA03173 1.09 executable.

`BB_PC_MENU=0` avoids both components; it cannot identify which component
caused a failure. To isolate a startup crash, use **only one change at a time**
and keep your normally working GPU/upscaler settings unchanged.

| Startup option | Generated PC menu assets | PC menu System hook | Title Quit hook |
| --- | --- | --- | --- |
| `BB_PC_MENU=1` | on | on | on when assets exist |
| `BB_PC_MENU=1 BB_PC_MENU_QUIT=0` | on | on | off |
| `BB_PC_MENU=1 BB_PC_MENU_HOOKS=0` | on | off | off |
| `BB_PC_MENU=0` | off | off | off |

The branch fixes `run.py` to preserve an explicit `BB_PC_MENU_QUIT=0`.
Previously `run.py` overwrote it according to whether the title assets
existed. The new `BB_PC_MENU_HOOKS=0` opt-out in `pc_menu.c` is
diagnostic-only and does not disable other engine features.

## Testing

Build the current branch with `bash build.sh`, then launch as usual with
`BB_PC_MENU=1 BB_PC_MENU_QUIT=0`, using your known-working DLAA command.

- If this works, the *title Quit hook* is a strong suspect. You can still use
  System > PC menu without the extra Quit Game row.
- If it still fails, try `BB_PC_MENU=1 BB_PC_MENU_HOOKS=0`. If this works,
  something in the PC menu's *engine hooks* is responsible; the generated
  assets alone are not enough to trigger the failure.
- If it still fails with hooks disabled, try additionally
  `BB_MODS_ENABLED=0` to exclude third-party mod overlays. Built-in generated
  enhancements still load with this setting. This can expose collisions with
  `menu/optionsetting.gfx` or `msg/*/menu.msgbnd.dcx`.
- If the failure occurs only after opening a PC section, look for a message
  about the menu's Scaleform heap. The port normally patches its 34 MiB menu
  heap with `BB_MENU_HEAP_EXTRA_MB=8`; `16` is another limited test. Do not
  raise the reserve indiscriminately: the parent guest heap has little room
  and larger allocations have prevented startup.

Capture the latest run log (launcher's configured save/user directory, not
necessarily the repository's `user/`) and look for:

```bash
grep -iE 'PC menu|enhancement|menu heap|engine hooks|mods|ERROR|Traceback|Exception|crash|failed' user/last_run.log | tail -80
tail -80 user/last_run.log
```

Also note whether the game exits before the first window, opens but freezes at
the title screen, or exits only after opening System. These point to different
failure paths and need different fixes.

## Compatibility

The native menu code and assets are tailored to Bloodborne CUSA03173, version
1.09. The menu generator intentionally returns no files if it detects
unexpected binary formats; unsafe or inconsistent third-party replacements
should be diagnosed rather than bypassing binary validation.

None of these diagnostic flags changes the DLAA Preset K selection or your
save data; remove the flags once the failing component is identified.
