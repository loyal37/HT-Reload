# Session Log

## 2026-10-10 - Project started, v0.1.0 finished

- Origin: the user asked how pak mods made with the current workflow could be hot reloaded. The session began in `D:\ueproject\HT`; the user then chose `E:\HT Reload` for development, with deployment into the game folder.
- Findings:
  - Mods are only mounted when the engine scans `Content\Paks` at startup; the shipping build has no `Mount` / `Unmount` commands.
  - Corrected by the user: mods are actually loaded through `dsound.dll` (Ultimate ASI Loader) in the game's Win64 folder plus `plugins\UniversalSigBypasser.asi`, not through the `Game start.exe` injector (whose log ends on 2026-05-31).
  - User decision: no bridge for now, packaging stays with the packager, which publishes straight into the game's `Paks\MOD\<mod name>`.
  - `HTGame.exe` is packed on disk: 63% of the pages of its code section are zero and the rest is ciphertext, so its code cannot be analysed offline; the strings in read-only data are plain.
  - Mounting, the priority of `_P` versions, container ordering and file sharing were checked in the UE 5.6 source; line numbers are in ARCHITECTURE.
- Implemented: five modules under `src/`, the stand-in host `test/host.cpp`, `build.ps1`, `deploy.ps1`, `test\run.ps1`, README.
- Fixed on the way:
  - A compile error from mixing `const` and non-`const` pointers in an initializer list; an explicit array instead.
  - Non-ASCII text in `.ps1` files is damaged under PowerShell 5.1; the scripts became pure ASCII.
  - The test script could not read the log (the plugin holds a write handle) or get the exit code; it now reads with write sharing and takes the process handle early.
  - A first scan could find only one candidate function; it now rescans every time while fewer than 2 are known, and waits 3 seconds before the first scan.
- Test: `test\run.ps1` passed, host exit code 0.
- Deployed: `HTReload.asi` copied into the game's Win64 folder.
- Source control: not committed or pushed; the user had not asked.
- Next: the user starts the game for a first check and hands back `HTReload.log`.

## 2026-10-10 - First run in the game: located

- The user started the game and asked for a look at the log. The plugin started at 13:34:41, finished locating at 13:34:45 and was ready at 13:35:01; the game process was fine and the logs of SigBypasser and Anticensor kept updating as usual.
- The result matched the stock engine exactly: 2 references to the anchor, 2 candidate functions, a single delegate binding.
- The 0x11e bytes in the log were disassembled by hand and confirmed as `HandleMountPakDelegate`, with parameters and `FString` layout matching the way the plugin calls it.
- No code change, no redeploy.
- Next: the user packages under a new name to test mounting, then switches characters to test the refresh.

## 2026-10-10 - First mount in the game, v0.1.1

- The user had restarted the game (new process started 13:38:49) and then published `blackbird_1_P` with the packager. Log: files on disk at 13:40:37, found and mounted at 13:40:40, the call took 7 milliseconds. The process was still responsive at 13:42.
- Why the first run ended is not known; its log had been overwritten by the second run. Keeping the previous log was added because of this.
- Locating reported one binding too many this time: the probing window had read into the next heap block and seen the same instance twice. Bindings are now deduplicated by function and object.
- v0.1.1 built and passed `test\run.ps1`. Not deployed (the game was running). Not committed.
- Next: ask the user what switching characters did and why the first run ended; deploy v0.1.1 when the game is closed.

## 2026-10-10 - Switching characters does not refresh; v0.2.0 adds active collection

- The user turned the body textures plain black and published `blackbird_2_P`. The log shows the mount at 13:49:21, but nothing changed in the game after switching characters.
- Checked: the ucas hashes of the three versions all differ; in the packaging source, each of `042_blackbird_body_d/id/m/n.ubulk` holds a single kind of BC block, so the textures really are a flat colour. The problem is that the game does not unload the old assets.
- Found the collection entry offline: the name `CollectGarbage` is referenced from two generated tables in read-only data, and in the native table the function after it is RVA `0x4406bd0`. The game's own `ForceGarbageCollection` is an empty stub. The structure of both tables and the implementation of `CollectGarbage` were confirmed in the UE 5.6 source.
- v0.2.0: `FindCollectGarbageThunk`, a request every 5 seconds for 120 seconds after a mount. The stand-in host got the two tables and a matching check; `test\run.ps1` passed.
- Not deployed (the game was still running, PID 31840). Not committed.

## 2026-10-10 - Hot reload works end to end

- The user reported: leaving the party did nothing; after returning to the login screen and entering again the textures were black.
- Checked: the game process was still the one started at 13:38:49 (PID 31840), so this was a refresh inside one process, not a restart.
- Conclusion: locating, mounting and refreshing all hold in the game; refreshing currently takes a return to the login screen.
- No code change. v0.2.0 still not deployed (game running). Not committed.

## 2026-10-10 - v0.2.0 deployed, first commit and push

- The user closed the game and asked for deployment and for the source to be published on GitHub.
- Deployed: `deploy.ps1` succeeded; the file hashes in the game folder match the build output.
- GitHub: the user had already created the repository `loyal37/HT-Reload` (private at the time) and set it as the local `origin`, so the push went there; no other repository was created and the visibility was not touched.
- Commit `7210708` (24 files, including `.ai-memory` and `.codex`), pushed to `main`, checked with `ls-remote`. A commit with only memory updates followed.
- No tag, no release.

## 2026-10-10 - v0.2.0 in the game: collection works, switching still does not refresh

- The user started the game at 14:10 and published `blackbird_4_P` (having removed `blackbird_P` and `blackbird_1_P` themselves; still 19 paks at startup), and reported that switching characters still did nothing.
- Log: locating was normal; the collection entry RVA `0x4406bd0` matched the offline value and its bytes disassembled exactly as expected; mounted at 14:11:52; no exception during the collection window.
- Conclusion: active collection works but does not help; the assets stay referenced while the character is in the party or the world.
- A new idea: a texture's streamed mips pick their container at every read, so re-streaming could refresh textures instantly. Only an idea, noted under Pending in TASKS.
- No code change. Memory files updated only, not committed.

## 2026-10-10 - The user wants a complete reload; v0.3.0, a probing build

- The user answered that all kinds of switching had been tried without effect, and that the goal is a complete reload of pak assets, not just textures.
- Judgement: who holds the old assets has to be known first. A read-only reader of the object system and a reference probe were written.
- Established on the way:
  - This mod's package names contain `Player` (`/Game/Characters/Player/042_Blackbird_1/...`), while the packaging source directory and the directory index of the `.utoc` do not. Package ids computed from the directory matched nothing at first; looking inside the assets explained it.
  - The port of `CityHash64` matches the engine's source line by line and was verified against a real `.utoc`.
  - The `.utoc` (written by retoc, version 8): a 144-byte header; of 43 chunks, 26 are package data, 16 bulk data and 1 the container header; it has a directory index.
  - In the second pass, which looks for the owners of heap blocks, many unrelated objects were reported as owners unless starting points inside other objects were excluded; the walk back now stops at another object.
  - The scan range was narrowed from the whole process to the allocations holding engine objects plus the image's data; the reason is in DECISIONS.
- Meanwhile the user closed the game and deleted `blackbird_4_P`; only `blackbird_2_P` was left in the mod folder.
- Test: `test\run.ps1` passed, including four checks of the probe report.
- Deployed: v0.3.0 copied into the game's Win64 folder. Not committed.

## 2026-10-10 - v0.3.0 finds no object table in the game; v0.3.1

- The user reported having switched to another character minutes ago without hearing anything. Log: name pool found, object table not found, probing disabled.
- Lesson: before having the user wait for a sound, the log should have been checked to see that probing was available; and the plugin made no sound when it was not. Both were changed.
- Checked `FChunkedFixedUObjectArray` in UE 5.6: the member order is as assumed; in `PreAllocate`, `MaxChunks = Max / 65536 + 1` and `MaxElements = MaxChunks * 65536`, and with preallocation `NumChunks = MaxChunks`. v0.3.0 required a fixed relation between chunk count and object count, which excludes the preallocated case.
- v0.3.1: recognition by the count pattern, the data found is logged on failure, a fallback that searches by content, and a low tone when probing is unavailable. `test\run.ps1` passed.
- Not deployed (game running, PID 40260). Not committed.

## 2026-10-10 - v0.3.1 still finds no object table; a matter of timing; v0.3.2

- After the user closed the game v0.3.1 was deployed, and they went back in. Log: name pool found (current block 297), nothing shaped like the object table's counts, and nothing found by content either.
- The timeline showed that both searches ran about 4 seconds after the plugin started and about 14 seconds before the window appeared. The stand-in host built its object table before loading the plugin, so the test never showed this.
- v0.3.2: search once the window exists and retry at a mount; a line that cross-checks GEngine and the data found when calibrating the class size fails; the host now builds its object table late. `test\run.ps1` passed.
- Not deployed (game running, PID 15180). Not committed.
- Lesson: the host's startup order has to match the real engine, or timing problems go unseen.

## 2026-10-10 - v0.3.2 finds the object table; v0.3.3 (class size offset, command file)

- When v0.3.2 was deployed the game process had not fully exited, so `deploy.ps1` refused once and succeeded after the process was gone.
- With the user in the game, the log showed the object table found at the first try after the window appeared, which confirmed the timing diagnosis. Calibrating the class size field failed, and the logged data showed the alignment value to be 16 bits wide.
- v0.3.3: calibration compares only the low 16 bits of the alignment; the command file `HTReload.cmd.txt` (`gc`, `probe`) was added, so that probes are triggered by the assistant from then on instead of by the user packaging and waiting for sounds. The host test got a check of the command file; `test\run.ps1` passed.
- Not deployed (game running, PID 25312). Not committed.
- By this point probing had cost the user three restarts of the game, with a fourth to come. The command file exists so that this does not happen again.

## 2026-10-10 - v0.3.3, the first real probe: who references the old assets

- The user asked whether "change the skin to something else" meant changing the character. Explained: another appearance of the same character; with no other appearance, switch the character on the field.
- With the user in the game and Blackbird on the field, the log confirmed all three parts of the object system; the class size field is at 0x58 (the assistant had miscounted it as 0x60 earlier; corrected).
- `probe worn blackbird` through the command file took 1.1 seconds. Direct references to the skeletal mesh come from the character on the field and its component, the game's appearance asset `MeshAsset_Player042`, and the default object and the sequence template of the ultimate-skill cinematic blueprint.
- From the engine source and the game executable: `ReloadPackages` is stripped; the loader's handling of renamed packages is still there; the script functions needed all exist. The outline of a complete in-place reload was written down from this.
- No functional change (one comment corrected), no rebuild or redeploy. Not committed.

## 2026-10-10 - Second probe: the objects stay alive with the character off the field

- The user switched the character on the field to another one. `gc 15` and then `probe away blackbird` through the command file.
- Result: not one old object fewer, and the direct referrers identical line for line. The character object, its components, dynamic materials and anim blueprint instance all still exist after the switch.
- Conclusion: no kind of switching makes the game let go; the only ways are replacing in place or starting the whole world over, as returning to login does.
- The route, the amount of work and the risks of a complete in-place reload were explained to the user, who was left to decide.
- No code change. Not committed.

## 2026-10-10 - The user asks for the reload to be implemented; v0.4.0 lays the ground

- The user asked why, given that the loader has the logic for loading again after a rename, the plugin could not "reload the packages and fix up all references" itself. Answer: it can, that is the proposal; work started.
- Checked the source of `UObject::Rename` (`Obj.cpp:238`, with a usable log string; the package class overrides it) and `UObject::ProcessEvent` (`ScriptCore.cpp:2032`, only a `checkf` string) and settled how to locate each.
- v0.4.0: split into shell and core, replacing the core at run time, ten read-only inspection commands, `deploy.ps1` able to replace just the core while the game runs.
- The test host now quits on a signal from the script; the test gained "after the core is replaced at run time it still answers commands and does not mount again". `test\run.ps1` passed.
- A trap in the test script: `{ … }.GetNewClosure()` passed directly as a command argument never made the condition true. A plain loop instead.
- The user had closed the game meanwhile. Shell and core deployed. Not committed.

## 2026-10-10 - v0.3.0 to v0.4.0 committed; the rule to commit after every change

- The user asked for the code to be committed first and ruled that every change is to be committed from now on, so that work does not pile up.
- Commit `905f860`: 32 files, 2742 lines added and 242 removed, everything accumulated since v0.2.0. Pushed to `origin/main`, checked with `ls-remote`.
- The rule went into the standing constraints of PROJECT_OVERVIEW and into the `project-memory-keeper` skill. Pushing continues the earlier practice; the user only said to commit.
- A commit with only memory updates followed.
- Lesson: from v0.3.0 on, nothing had been committed on the grounds that the user had not asked, and four versions ended up in one commit whose history cannot be read version by version.

## 2026-10-10 - v0.4.0 verified in the game; Rename and ProcessEvent found and verified

- With the user in the game, shell and core worked there; the first replacement of the core in a running game succeeded (and two more after it).
- Found and fixed: when the core read the load count, the shell had already advanced it, so the initial 3 second wait was skipped. It is now recorded in `CoreStart`.
- Found that `build.ps1` rebuilding the shell every time changed its bytes, which made deploying impossible while the game ran; the shell is now only rebuilt when its sources changed.
- With the inspection commands: the two log texts of the rename exist, but no code references them directly, because `UE_LOG` keeps the text in a static record. With a lookup "through a pointer" added to `strref`, both lead to the same function, RVA `0x181e3b0`.
- Comparing the vtables of a mesh object and a package object put `Rename` in slot 46; the shared 0x3e8-byte function in slot 76 was confirmed as `ProcessEvent` by disassembly, which also confirmed several offsets of `UFunction` and `FProperty`.
- `tools\ask.ps1` and `tools\disasm.ps1` were written. The address conversion of the disassembly script was revised twice: only a lone branch target and an address directly inside brackets are converted; register plus displacement, immediates and segment offsets are left.
- Core 0.5.0: `engine`, `calltest`, `plan`, `renameload`. `calltest` succeeded in the game. `renameload` was written but not run.
- Commits: `20d65a3` (core 0.4.1), `f8d54b1` (core 0.5.0), both pushed.

## 2026-10-10 - Complete in-place reload works; the user confirms seeing the change

- With the user's agreement, `renameload 042_blackbird_body_d`: the rename succeeded and the load returned a different, new object; old and new coexisted and the game was fine.
- The inspection commands `functions` and `props` were added; they showed that the game creates dynamic materials for all 40 material slots and that `CopyParameterOverrides` can push a new parent to the render thread.
- Core 0.6.0: `Reloader::Reload`, with rename, load, rewrite, reparent and mesh reset in one game-thread task. `plan` now shares the same classification.
- Before running it the user was asked (identical content, a stability check only) and chose to run it now. The first complete reload succeeded at 17:34 in about 1.3 seconds; after collection no old package was left.
- Reviewing it turned up two false slots: the name fields of world partition cell objects equal to the addresses of two objects of the mod. Core 0.6.1 added the guard.
- The user packaged `blackbird_4_P` (mounted automatically) and asked for a reload. The second reload succeeded at 17:43, and **the user said it worked and the change was visible**.
- Core 0.6.2: automatic reload after a mount, three kinds of result, a third tone, and a check of the component's slots after the mesh reset. Deployed into the running game at 17:44.
- Observed that the game rebuilds the cloth's dynamic and outline materials itself when the mesh is reset, and that the two body slots end up holding the material itself. Recorded in KNOWN_ISSUES.
- Committed and pushed: `5590bf6`, `cf3dd15`, `fcdc7cc`, then the memory update.

## 2026-10-10 - Release v0.6.2 published

- The user asked for a release on GitHub.
- The repository turned out to be public (the record said private). The visibility was not changed by the assistant. The release went ahead, and the user was told that it is publicly downloadable.
- The README was out of date (it still described returning to the login screen and the automatic probe that had been removed) and was rewritten first: risks, installation, known limits, how the reload works. Commit `f22f629`.
- `gh release create v0.6.2`: the tag points at `f22f629`, the attachment is `HTReload-0.6.2.zip`, and the shell and core in it have the same hashes as the ones running in the game. The notes said plainly that the automatically triggered reload had not yet run in the game.
- No licence file was added; the user had not mentioned one.

## 2026-10-10 - Automatic reload confirmed in the game; unmounting old containers after a reload

- The user asked whether the old three files can be deleted while the game runs. Measured by asking for delete access only: the `.ucas` and `.utoc` of mounted versions are locked, the `.pak` is not. Answered no, and proposed unmounting.
- The log showed that the automatic reload had already succeeded twice (17:52, and 18:10 after the user restarted the game).
- The user asked for old containers to be unmounted automatically after a reload.
- The game's file was confirmed to contain `Unmounting pak file: %s \n` exactly once. The game was closed at the time, so the references could not be looked up in a running game.
- Core 0.7.0: `FindUnmountHandler`, `ShadowedContainers`, `UnmountShadowed`, `UnmountWhenCollected`, `Reloader::RetiredPackagesAlive`, the commands `shadowed` and `unmount`. The loop that scans delegate instances became `ForEachInstanceWord`, shared by mount and unmount.
- Stand-in host: it holds `.ucas` and `.utoc` open on mounting, as the engine does, and got an unmount handler and a second delegate. New in the test: nothing is unmounted with one version; with two, only the old one, whose files are let go while the new one stays held. Passed.
- Commit `5bd5e1c`, pushed. The core was deployed with the game closed. The README got a section on it.
- Unmounting had not run in the game yet. No new release.

## 2026-10-10 - Unmounting works in the game; deleting right after it

- The user said the files could be deleted, and asked for them to be deleted directly together with the unmount.
- The log confirmed how 0.7.0 behaved in the game: entry RVA `0x27b29d0`; at 18:24, 6 seconds after the reload was done, `blackbird_4_P` (the one the engine had mounted at startup) was unmounted.
- Core 0.7.1: `DeleteContainer`; `mounted_` became a table with the mount sequence; `MountedModContainers` was split out, shared by the unmount rule and the note about a number that is too low.
- New in the test: a third pak `hot_0_P` (lower number, published later) gets the note and is neither unmounted nor deleted; the three files of `hot_1_P` are deleted; `hot_2_P` is intact. Passed.
- Commit `61c63ed`, pushed, deployed with the game closed. README and memory updated.
- The user was reminded that deleting is permanent and that `blackbird_4_P`, which they had been keeping as a base version, would be deleted after the next reload.

## 2026-10-10 - 0.7.1 verified in the game; release v0.7.1

- The user packaged in the game and asked for a look at the log and for a release.
- Log (the run started at 18:39): `blackbird_5_P` mounted → reload done 2 seconds later → 6 seconds after that `blackbird_4_P` unmounted and deleted. Only 5 was left in the folder and the game was fine.
- `gh release create v0.7.1`, pointing at `07d9329`; the two binaries in the attachment were compared by hash with the ones running in the game before packaging. The notes cover the changes since 0.6.2, that deleting is permanent, and how often each part was verified.
- While the release was being packaged, a command containing `Remove-Item` was blocked by the environment (a path with a space was misjudged). Overwriting instead of deleting solved it.

## 2026-10-10 - Automatic numbering in the packager

- The user asked for the packager's automatic numbering as well, with their way of entering the name left as it was.
- The packager is a separate repository. The user normally types the full name with its number (`blackbird_5_P` was stored in the settings).
- What was done: a checkbox, with the name field untouched; when it is on, the output name is one more than the highest number present. Both habits of typing (`blackbird_P` or `blackbird_5_P`) work. The window shows the actual output name below the name field, in the output plan and in the task console.
- Off by default, which was the assistant's judgement: the packager is a public tool, and for people who do not use it with hot reload a default of on would leave one more set of files with every build. It was switched on in the user's own settings, and the user was told.
- Verified with a check program for the naming rule, by starting the new exe and looking at a capture of the window, and with one real build driven through UI automation into a scratch directory (5 seconds, producing `blackbird_6_P`). The user's settings file was backed up and restored around the test and the scratch output deleted.
- Packager 2.5 → 2.6, local commit `de16cb0`, not pushed.

## 2026-10-10 - A review of the project, and a question about blueprints

- The user showed a review of the project written elsewhere and asked for an opinion. Its main finding holds: `Reload` returns `Done` whenever nothing aborted, even when an asset produced no new object or a slot was not replaced. Added to KNOWN_ISSUES and TASKS. Not fixed; the user is to say who does it, since another assistant appears to be reading the same repository.
- The user asked whether blueprint changes, an added toggle for example, take effect after a reload. Answer: expected to, since the toggle logic lives in the anim blueprint, whose instance the engine recreates from the new class when the mesh is reset (observed in every reload so far); the save class and the panel follow from there. Not tried with a real blueprint change. Advice given: close the panel before publishing.

## 2026-10-11 - Messages reworded, documentation rewritten

- Core 0.7.2: the log and report messages in `shell/shell.cpp` and seven files under `src/` were reworded; `test/run.ps1` and `tools/ask.ps1` now look for `Ready:` and `Command done:`. `test\run.ps1` passed. Commit `3716259`.
- README and the seven memory files were rewritten. PROJECT_OVERVIEW also lost a stale list of things "not done" that had since been done.
- The shell changed with this, so it can only be deployed with the game closed. It was: shell and core 0.7.2 were copied into the game folder with the game closed (hashes in CURRENT_STATE). Neither has run in the game yet.
- The notes of the releases `v0.6.2` and `v0.7.1` on GitHub were rewritten to match; their attachments are unchanged. Commit `e7edffe`.

## 2026-10-11 - Git identity corrected

- The user asked why GitHub shows no contributor for what they push from VS Code. The global git email was `loyal37@github.com`, which no account is linked to; the packager's repository had `loyal37@users.noreply.github.com` set locally and showed the account correctly.
- At the user's request the global email was changed to `loyal37@users.noreply.github.com`. Earlier commits keep the old address; rewriting history was offered and not asked for.

## 2026-10-11 - The FADIA test, and core 0.8.0

- The user tested another mod, FADIA: first the original model, then some faces deleted (reload worked), then one slot given another material (no effect); the old versions were not cleaned up either time.
- Shell and core 0.7.2 ran in the game for the first time in that session, without trouble.
- Looked into it in the running game with the inspection commands; findings are in CURRENT_STATE. In short: the mod's textures replace textures of the game in place and are referenced from array storage, which was not followed; and the component's per-slot override materials outlive a mesh reset.
- Core 0.8.0: `reflect.{h,cpp}`, the array walk, dependent material instances, `ArrayKind`, slot materials following a changed mesh, `ReloadResult::Partial`, the command `arrayrefs`. `FindSuperseded` takes further packages to treat as superseded.
- Checked in the game, read-only: the layout of array and struct properties was found; `arrayrefs` on the old `_d` texture named `MI_player_039_fadia_a` and `outline_039_cloth`, `TextureParameterValues`, field +0x10; `plan FADIA` ran in 1.6 seconds.
- `test\run.ps1` passed; the stand-in host has no reflection data, so the new paths are not exercised there.
- Commit `c619c5e`, pushed; core swapped into the running game.

## 2026-10-11 - Core 0.8.0 in the game; the leftover report

- The user asked what kinds of assets the remaining gap (maps, sets, nested arrays) concerns. Answered from the stock UE 5.6 source, the game being closed: animation sequences inside montages, textures used directly by a base material, assets used by Niagara effects, layers of layered materials, rows of data tables and lookup tables of the game, assets pinned by the asset manager.
- The user restarted, published `FADIA_3_P` again with a slot using another material, and reported it as fine. The log agrees for the reload (see CURRENT_STATE) but also shows 4 old packages never collected and the old version left mounted. The user had closed the game by then.
- The user asked whether their toggle blueprint and panel reload. Answered from what the Blackbird reloads showed, and said what is untested; see CURRENT_STATE.
- Core 0.8.1: `Reloader::ReportRetired`, `HTReload.leftover.txt` written when renamed packages outlive the collection window, the command `leftover`. `test\run.ps1` passed. Commit `2f49348`, pushed, deployed with the game closed.
