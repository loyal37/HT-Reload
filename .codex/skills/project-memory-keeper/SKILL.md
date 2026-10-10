---
name: project-memory-keeper
description: Maintain and restore persistent project context for HT Reload, the pak mod hot-reload ASI plugin for Neverness to Everness. Use this skill before starting project work, after finishing project work, after context compression, in a new conversation, or when the user says to continue development, resume the project, restore context, check current progress, or that the AI forgot prior state.
---

# Project Memory Keeper

## Purpose

This skill keeps the project state outside chat history. The primary memory source is the Markdown files in the project root `.ai-memory/` directory. Chat history can help, but code, tests, Git state, and `.ai-memory` are the authoritative sources.

Use this skill for the HT Reload project whenever development starts, resumes, or ends.

## Required Memory Files

The project root must contain:

- `.ai-memory/PROJECT_OVERVIEW.md`
- `.ai-memory/ARCHITECTURE.md`
- `.ai-memory/CURRENT_STATE.md`
- `.ai-memory/DECISIONS.md`
- `.ai-memory/TASKS.md`
- `.ai-memory/KNOWN_ISSUES.md`
- `.ai-memory/SESSION_LOG.md`

Do not store passwords, tokens, cookies, private keys, or other secrets in any memory file.

## Start Workflow

Before modifying project code:

1. Read `.ai-memory/PROJECT_OVERVIEW.md`.
2. Read `.ai-memory/CURRENT_STATE.md`.
3. Read `.ai-memory/TASKS.md`.
4. Read `.ai-memory/ARCHITECTURE.md`, `.ai-memory/DECISIONS.md`, and `.ai-memory/KNOWN_ISSUES.md` when they are relevant to the current task.
5. Check Git status, the latest commit, and uncommitted changes.
6. Inspect the real source files related to the task.
7. If the user reports a game run, read the game's `Win64\HTReload.log` before drawing conclusions.
8. Briefly state:
   - current project state
   - current task
   - modules likely to change
   - existing behavior that must not be broken

Do not rely only on conversation history to decide what the project currently does.

## Development Rules

- Code, test output, build output, the game's log, and Git status override memory files.
- If memory files disagree with code or test results, treat the code/test result as true and update memory.
- Do not reimplement functionality that already exists.
- Before changing how the mount entry is located, how it is called, or how packages are named, check `.ai-memory/DECISIONS.md`.
- Record new long-term constraints, important bugs, or architectural choices as soon as they appear.
- Keep changes scoped to the user's request.
- Run `test\run.ps1` before deploying. Deploy only while the game is closed.
- Keep `.ps1` files ASCII-only.
- Never add anything that hides the plugin from, or works around, the game's anti-cheat.

## Finish Workflow

Before ending a development turn:

1. Run the build and `test\run.ps1` when feasible.
2. Update `.ai-memory/CURRENT_STATE.md`, including the deployed file's hash when a deployment happened.
3. Update `.ai-memory/TASKS.md`.
4. Update `.ai-memory/DECISIONS.md` if a new decision was made.
5. Update `.ai-memory/KNOWN_ISSUES.md` if an unresolved issue was found.
6. Append a concise entry to `.ai-memory/SESSION_LOG.md`.
7. Record a clear next step so another AI with no chat history can continue.
8. Check that memory files match the current code and test state.

Update memory after every commit, recording the exact commit.

`CURRENT_STATE.md` must stay short enough to read in a few minutes. Move old detail to `SESSION_LOG.md` instead of appending endlessly.

## Recovery Mode

Automatically enter recovery mode when the user says something like:

- continue development
- resume from last time
- restore project context
- check where the project is
- AI forgot the previous context
- recover progress from project files
- continue in a new conversation

In recovery mode:

1. Do not immediately modify code.
2. Read the required memory files.
3. Check Git status and recent changes.
4. Read the source files relevant to the likely next task.
5. Output a short recovery report:
   - what the project is
   - current state
   - recently completed work
   - current issues
   - most reasonable next step
6. Continue only after the recovered state is consistent with code and Git state.

## Maintenance Rules

- Each fact should have one primary home. Do not duplicate the same state across many files.
- Delete outdated state or move it to `SESSION_LOG.md`.
- Keep `SESSION_LOG.md` as summaries only; never paste full chat logs or large code blocks.
- After major refactors, update `ARCHITECTURE.md` and `CURRENT_STATE.md`.
- Important technical decisions must not be reversed without a clear reason recorded in `DECISIONS.md`.
