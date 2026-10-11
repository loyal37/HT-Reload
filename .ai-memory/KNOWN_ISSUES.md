# Known Issues

## Old FADIA packages stay alive after a reload

- Seen on 2026-10-11 at 15:09 with core 0.8.0: the reload completed and looked right, yet 4 renamed packages were not collected and the older version stayed mounted.
- Not diagnosed; the game was closed first. One lead from the earlier session: a third heap location held the old `_d` texture besides the two texture parameter arrays, inside elements that look like 0x28-byte records with a GUID, which the array walk did not attribute to any property.
- Core 0.8.1 writes `HTReload.leftover.txt` when this happens: the old assets and everything that still refers to them.

## A reload can only see what it can find

- Since core 0.8.0 a reload that ran to its end but left an asset or a reference as it was returns `Partial` and is reported as such, instead of as done.
- That covers what the reload found and could not settle. A reference it does not find at all (in a map or set, or in an array nested in another container) is not counted; it shows afterwards as renamed packages that are not collected, and as old versions that stay mounted. Since core 0.8.1 that case is logged with a pointer to `HTReload.leftover.txt`.

## Override materials follow a mesh only across a reload

- When a new mesh uses a different material in a slot, the override the game made for that slot is moved to it (since core 0.8.0). The comparison is between the mesh before and after the reload, so an override that was already out of step, for instance from a reload done by an older core, stays as it is until the character is set up again (return to login).
- The outline material the game pairs with a dynamic material (the dynamic material points at it at +0x178) is not changed; it keeps the outline of the slot's former material.
- An override that is not derived from the slot's former material is left alone and noted in the report; it may be something the game or a mod's blueprint put there on purpose.

## Two body material slots have no dynamic material after a reload

- Symptom: Blackbird's `CharacterMesh0` has 40 override material slots. Before a reload each holds a dynamic material; afterwards slots 13 and 14 (`MI_mint_042_blackbird_body1`) hold the new material itself.
- Cause: when its mesh is reset, the game's component only rebuilds the dynamic and outline materials of the 14 cloth slots; the pass that gives every slot a dynamic material only runs when the character is initialised.
- Impact: the user looked at the result and found it fine. Whether effects driven by dynamic material parameters (hit flash, dissolve and the like) still work on the body was not checked. Returning to the login screen and entering again restores dynamic materials everywhere.
- Possible fix: call `PrimitiveComponent.CreateDynamicMaterialInstance` for such slots after the mesh reset. Whether the game's own bookkeeping of dynamic materials would accept the new ones has to be found out first.

## Reparenting clears the parameters the game set on a dynamic material

- Symptom: `CopyParameterOverrides` empties the dynamic material's own parameters before copying those of the new parent. Parameters the game set at run time are lost until it sets them again.
- Impact: none in practice for the Blackbird mod, because the game replaces those dynamic materials when the mesh is reset. A mod without its own mesh may show a brief visual difference.
- Cause: this is the only way found to trigger `InitResources()` from a script function and so push the new parent to the render thread.

## Only some references outside object bodies are followed

- Before core 0.8.0 a reload only rewrote pointer slots inside the bodies of other objects. It showed with the FADIA mod on 2026-10-11, which replaces three textures of the game at their original paths: the game's material instances hold them in their texture parameter arrays, so the old textures stayed in use, the new copies were collected for lack of a referrer, and the old versions were never unmounted.
- Since core 0.8.0 the top-level array properties of every object are walked through reflection (see ARCHITECTURE).
- Not followed: maps and sets; arrays nested inside structs that are themselves in containers; arrays that are members of a struct property; a base material (`Material`, not an instance) that refers to a texture.

## Slots are found by value, so other data can be taken for a pointer

- Symptom: in testing, the name fields of two world partition cell objects happened to equal the addresses of two objects of the mod. The range of name ids overlaps low heap addresses.
- Status: since core 0.6.1 the words of an object's header other than the class pointer are not slots; and where an old object's address is also a name id, only an object property declared by the class counts, anything else is marked doubtful and left.
- Remaining risk: other kinds of data (integers, hashes) equal to an object's address are extremely unlikely but not impossible; an object property nested in a struct is not recognised in the doubtful case and keeps the old pointer.

## Instances of old classes are not replaced

- Symptom: a reload replaces assets. Anim blueprint instances are rebuilt by the engine when the mesh is reset; an instance of the panel (`WBP_NTEPanel_C`) or of the save class (`Blackbird_1_save_C`) that is alive at that moment stays an instance of the old class, which also keeps the old class from being collected and the old version from being unmounted.
- Impact: in the reloads so far none of these classes had a live instance. A reload with the panel open has not been tried, and neither has a reload after a blueprint change such as an added toggle.

## Returning to the login screen is the fallback

- Established earlier on 2026-10-10: switching within the party, leaving the party, changing the skin and teleporting do not refresh assets, because a character's objects stay alive in the level while it is off the field; returning to the login screen and entering again refreshes everything.
- With the in-place reload this is only needed when a reload did not complete. The log says so.

## The "possible owners" section of the probe is noisy

- Symptom: in the first probe in the game the direct referrers were only 25 and clean; there were 1950 "pointers outside objects", and the second pass derived 473 "possible owners", among them many plainly unrelated classes (audio events, Niagara scripts, vehicles and so on).
- Cause: the second pass only guesses, by "points at an address within 0x800 bytes before the slot", and unrelated blocks next to it in the same heap are counted too.
- Impact: direct referrers can be trusted; indirect owners are leads, not conclusions, and never a basis for writing to memory.
- Possible fix: walk containers exactly through the engine's reflection instead of guessing by address.

## The offset of the class size field was once recorded wrongly

- On 2026-10-10, reading the data v0.3.2 had logged, the assistant miscounted a group and took `PropertiesSize` to be at 0x60 in the game. v0.3.3 measured 0x58, as in the stock engine; the only real difference is that the alignment value after it is 16 bits wide. Memory files and the code comment were corrected. The user had been told the field sits 8 bytes later than in the stock engine, which was wrong.

## The probe reads a large part of the engine's heap

- A probe reads most of the engine's heap memory, twice. Earlier versions read only the image and a few heap pointers. How the anti-cheat reacts cannot be judged for this either.

## Collection requests only run for a window after a mount

- Symptom: collections are requested for 20 seconds after a completed reload and 120 seconds otherwise. In-game actions after that depend on the engine's own periodic collection.
- Workaround: publishing another version starts the window again; the command `gc [seconds]` opens one by hand.
- Correction (2026-10-10): this entry used to say that renaming a package in memory does not get around the loader's record of loaded packages. That was wrong. The loader drops the record of a loaded package once it has been renamed and reads from disk again; the in-place reload rests on exactly that and is verified in the game.

## The locating window reads past small heap blocks

- Symptom: locating reads 12 machine words behind every heap pointer. A delegate instance is only about 0x30 bytes, so the window runs into the neighbouring allocation. In the second run of v0.1.0 one instance was therefore counted as two bindings (global `0xfa2a710` at offset 0x20 and global `0xfa2a730` at offset 0x50 led to the same address).
- Impact: function and object were the same, so the result was right; there was just one misleading warning.
- Status: since v0.2.0 bindings are deduplicated by function and object.

## A reload blocks the game thread while it loads

- Symptom: rename, load and replace run in one task on the game thread, and the game stands still meanwhile. About 1.3 seconds for Blackbird's 26 packages.
- Risk: what happens if it is triggered during the game's own loading or a scene change has not been tried. The automatic reload does not pick its moment.

## Packaging under the same name fails while the game runs

- Symptom: on publishing, the packager first renames the old three files and then moves the new ones in. The old files are held by the game, the rename fails, and the whole build reports an error at the last step and rolls back.
- Workaround: use a new name while the game is open.
- Since 2026-10-10 the packager (2.6) has an "automatic numbering" option; with it on, every build uses the next number and no manual renaming is needed.

## Old versions

- Measured on 2026-10-10: while the game runs, the `.ucas` and `.utoc` of every mounted version are locked and cannot be deleted; the `.pak` is not locked, but deleting it alone is pointless. A version that is no longer used after a reload is locked just the same.
- Since core 0.7.0 old containers that a new version fully supersedes are unmounted after a reload, which lets go of the files. Verified in the game.
- Since core 0.7.1 the three files are deleted right after the unmount, permanently and not to the recycle bin. An old version worth keeping has to be copied elsewhere beforehand. Verified in the game (2026-10-10 18:41).
- What still stays behind: several versions that were all on disk at startup (until the next reload); a reload that did not complete; old assets that were not collected; an old version holding a package the new one lacks.
- A name that has been unmounted is not mounted again in the same run (the watcher records names as handled). Publish again under a new number.

## A pak whose name does not end in `_P` has too little priority

- Symptom: without the `_P` suffix the order is the same as that of the game's own paks, so overriding the game's assets is not guaranteed.
- Behaviour: it is mounted as usual and the log has a note.

## An exception in the mount call disables mounting for the run

- Symptom: the call is wrapped in SEH. If an exception is raised, its code is logged, one long low tone sounds, and nothing more is mounted.
- Impact: the engine's state cannot be trusted at that point (it may still hold a lock); the game should be restarted.

## The anti-cheat's reaction is unknown

- The game has `AntiCheatExpert`. The plugin reads memory of the game, installs a thread message hook, calls engine functions and writes pointers in engine objects.
- Known: SigBypasser and Anticensor in the same folder scan and modify memory at run time as well and have been in use all along.
- Unknown: whether calling engine functions draws extra attention. It cannot be judged.
- Advice: development use only, on a secondary account.

## The log file is held open

- Symptom: the plugin keeps a write handle on `HTReload.log` (others may read and delete).
- Impact: Notepad opens it; programs that open with default sharing (PowerShell's `Select-String` and `Get-Content`, for example) fail while the game runs.
- Workaround: read it with write sharing allowed; see `Read-Shared` in `test\run.ps1`.

## One line about vswhere in the build output

- Symptom: `vcvars64.bat` prints a line when it cannot find `vswhere.exe` by itself.
- Handling: `build.ps1` discards all output of `vcvars64.bat`; the build is not affected.
