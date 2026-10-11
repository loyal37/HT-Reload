# Tasks

## In Progress

- Find what keeps old FADIA packages alive after a reload.
  - Files: `Reloader::ReportRetired` in `src/reload.cpp`, `Watcher::UnmountWhenCollected` in `src/main.cpp`; output in the game's `Win64\HTReload.leftover.txt`.
  - Done when: after a reload of the FADIA mod every renamed package is collected and the older version is unmounted and deleted.
  - State: with core 0.8.0 the reload itself worked (15:09 on 2026-10-11: dependent material instances reloaded, slot 2 followed, the user saw the change), but 4 old packages stayed alive and `FADIA_2_P` stayed mounted. Core 0.8.1 (commit `2f49348`, deployed) writes the leftover report when that happens.
  - Needs: the user to do another reload with the FADIA mod.
- Try a reload after a blueprint change (an added toggle) with the Blackbird mod; see CURRENT_STATE for what is known.

## Next

- The packager's automatic numbering is done (see Completed). The commit is local to the packager's repository and not pushed; waiting for the user.
- Two body material slots have no dynamic material after a reload (see KNOWN_ISSUES). Decide once the user reports whether anything is visibly wrong.
- References in maps, sets and nested arrays are still not followed (top-level arrays are, since core 0.8.0).
- A reload after a blueprint change (an added toggle, for example) has not been tried. Expected to work when the panel is closed while publishing; the user was told what to watch for.

## Pending

- Shelved: instant texture refresh through re-streaming. The complete reload covers it.
- Optional: a configuration file (switch for the automatic reload, sounds, watched directory, length of the collection window).
- Optional: when NTE Bridge is used later, hook publishing under a new name into the bridge's `_publish_outputs` (`NTE bridge/blender_addon/nte_bridge/packaging.py`).

## Completed

- 2026-10-10: v0.1.0, sources, build scripts, stand-in host and end-to-end test, deploy script, README, project memory.
- 2026-10-10: locating and mounting at run time verified in the game.
- 2026-10-10: v0.2.0, active collection requests. Deployed and verified, committed as `7210708` and pushed to `loyal37/HT-Reload`.
- 2026-10-10: confirmed that returning to the login screen and entering again refreshes everything without restarting the process.
- 2026-10-10: v0.3.0–v0.3.3, reading the object table, the reference probe, the command file. Two probes in the game showed who holds the references.
- 2026-10-10: v0.4.0, split into shell and core, replacing the core at run time, read-only inspection commands. v0.3.0 to v0.4.0 went in together as `905f860`.
- 2026-10-10: core 0.5.0, `Rename` and `ProcessEvent` found and verified, call test succeeded, `renameload` succeeded in the game.
- 2026-10-10: core 0.6.0–0.6.2, **complete in-place reload**. Two manual reloads succeeded in the game, the second with the user's newly packaged `blackbird_4_P`; the user confirmed seeing the change, and all old objects were collected. Commits `5590bf6`, `cf3dd15`, `fcdc7cc`.
- 2026-10-10: the automatic reload after a mount succeeded twice in the game (17:52 and 18:10, the second in a new run after a restart).
- 2026-10-10: release `v0.6.2` published.
- 2026-10-10: core 0.7.0, old containers that are superseded are unmounted after a reload. Succeeded in the game (at 18:24 `blackbird_4_P`, mounted at startup, was unmounted); the user confirmed the files could be deleted. Commit `5bd5e1c`.
- 2026-10-10: core 0.7.1, the old three files are deleted right after the unmount. Succeeded in the game (at 18:41 `blackbird_4_P` was unmounted and deleted). Commit `61c63ed`. Release `v0.7.1` published.
- 2026-10-10: the packager (a separate repository, `D:\Neverness to Everness Mod Loader\cook\packager`, `loyal37/NTE-Mod-Packager`) got "automatic numbering" in 2.6. Local commit `de16cb0`, not pushed.
- 2026-10-11: core 0.7.2, log and report messages reworded; README and project memory rewritten. Commit `3716259` and the one after it. Shell and core 0.7.2 ran in the game without trouble (run started 12:31).
- 2026-10-11: core 0.8.0, references in array properties, dependent material instances reloaded along, override materials following a changed mesh, the `Partial` result. Commit `c619c5e`. The reload worked in the game at 15:09; cleaning up the old version did not.
- 2026-10-11: core 0.8.1, the leftover report. Commit `2f49348`.
- 2026-10-11: release `v0.8.1` published.
