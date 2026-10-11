# Tasks

## In Progress

- None.

## Next

- Tighten the verdict of a reload: a third result for "partly done" and a final line after the collection window on whether every old object went away (see KNOWN_ISSUES). Raised by a review on 2026-10-10; the user has not said who is to do it.
- The packager's automatic numbering is done (see Completed). The commit is local to the packager's repository and not pushed; waiting for the user.
- Two body material slots have no dynamic material after a reload (see KNOWN_ISSUES). Decide once the user reports whether anything is visibly wrong.
- References outside object bodies (the heap storage of `TArray` and `TMap`) are not rewritten. The Blackbird mod is not affected, since whatever references its assets is either inside the pack or a pointer in an object body. A mod that only replaces materials or textures and uses a mesh of the game is affected (the mesh's material array is on the heap). Needs a walk over container properties through reflection.
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
- 2026-10-11: core 0.7.2, log and report messages reworded; README and project memory rewritten. Commit `3716259` and the one after it.
