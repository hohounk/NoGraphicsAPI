# NoGraphicsAPI fork of hohounk

This checkout is the one shared home for every change to NoGraphicsAPI that a
project of the owner needs.

The upstream coding rules apply to every change here:

@AGENTS.md

## Branches

| Branch | Role | Moves by |
|---|---|---|
| `main` | Mirror of `upstream/main` | Fast-forward only. No fork commits. |
| `linux-wsi` | Topic: Linux WSI callbacks, validation forwarding | New commits only, never rebased. |
| `device-lost` | Topic: lost-device reports from submit and the waits | New commits only, never rebased. |
| `presentation-device-lost` | Topic: tests that need both `linux-wsi` and `device-lost`, such as a lost device at the present fences. Merges both topics in. | New commits and `--no-ff` merges of those two topics only, never rebased. |
| `pr/16-device-diagnostics` | Topic: upstream PR #16, frozen at `a48f70e` | Moves only when the owner takes a newer PR head. |
| `pr/16-fixes` | Topic: fixes to the PR #16 code (torus-fqx.5.8, torus-fqx.5.10). Starts from `pr/16-device-diagnostics`. | New commits and `--no-ff` merges of `pr/16-device-diagnostics` only, never rebased. |
| `presentation-pr/16` | Topic: tests that need both `linux-wsi` and `pr/16-fixes`, such as presentation on a driver that exposes only `VK_EXT_swapchain_maintenance_1`. Merges both topics in. | New commits and `--no-ff` merges of those two topics only, never rebased. |
| `integration` | **Every consumer pins a commit SHA of this branch** | `--no-ff` merges of `main` and the topics, plus `CLAUDE.md` commits. Never rebased. |

## Rules

- Consumers pin a commit SHA of `integration` through CMake FetchContent,
  with no patch step. Never pin a branch name or a topic.
- Make every NoGraphicsAPI change in this checkout. Never edit a FetchContent
  copy such as `.deps/` of a consumer.
- This checkout stays on `integration`.
- A fix goes on its topic branch, in a worktree under `.worktrees/`. Then the
  topic merges into `integration` with `--no-ff`, in this checkout. The merge
  message records each conflict resolution and the reason. Commit only
  `CLAUDE.md` on `integration` directly. Check: this command prints nothing.

  ```bash
  git log --first-parent --no-merges main..integration -- . ':!CLAUDE.md'
  ```

  If it prints a commit, do not push. See "When a check fails".

- The unversioned hook `.git/hooks/pre-commit` rejects a commit with staged
  conflict markers. It covers every worktree. Do not bypass it with
  `--no-verify`. If the hook is missing, restore it from the notes of bead
  torus-fqx.5.11 in torus-engine.
- A new topic starts from `main`, gets its own branch, and merges like the
  others. A topic that fixes code that only another topic holds starts from
  that topic, as `pr/16-fixes` does.
- A consumer that needs a change starts an agent-deck session here:
  `agent-deck launch ~/katsepolygon/forks/NoGraphicsAPI -t "<title>" -c claude --message "<the change>"`.
  The consumer then moves its pin to the new `integration` SHA.
- A local build against this checkout uses
  `-DFETCHCONTENT_SOURCE_DIR_NOGRAPHICSAPI=<this checkout>`. Remove the
  override before a consumer records a pin.
- Never open a pull request or an issue on `sebbbi/NoGraphicsAPI`, and never
  push to `upstream`. The upstream push URL is `DISABLED` in this clone.
- A `git push` to `origin` needs the owner's yes.

## Gate

Run after each merge into `integration` and at every sync, from the root of
this checkout:

```bash
B=$(mktemp -d /tmp/nga-gate.XXXX)
F=(-G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-stdlib=libc++ "-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++ -lc++abi")
cmake -S . -B "$B/suite" "${F[@]}" -DNOGRAPHICSAPI_BUILD_TESTS=ON -DNOGRAPHICSAPI_BUILD_EXAMPLES=OFF > "$B/suite.cfg" 2>&1 \
  && cmake --build "$B/suite" > "$B/suite.build" 2>&1 \
  && timeout -k 5 540 ctest --test-dir "$B/suite" --timeout 120 > "$B/suite.ctest" 2>&1; echo "suite=$?"
cmake -S . -B "$B/rel" "${F[@]}" -DCMAKE_BUILD_TYPE=Release -DNOGRAPHICSAPI_BUILD_TESTS=ON -DNOGRAPHICSAPI_BUILD_EXAMPLES=OFF > "$B/rel.cfg" 2>&1 \
  && cmake --build "$B/rel" --target test_startup_failures > "$B/rel.build" 2>&1 \
  && timeout -k 5 300 ctest --test-dir "$B/rel" -R '^test_startup_failures' --timeout 120 > "$B/rel.ctest" 2>&1; echo "release=$?"
cmake -S . -B "$B/ex" "${F[@]}" -DNOGRAPHICSAPI_BUILD_TESTS=OFF -DNOGRAPHICSAPI_BUILD_EXAMPLES=ON > "$B/ex.cfg" 2>&1
cmake --build "$B/ex" -- -k 0 > "$B/ex.build" 2>&1; grep '^FAILED:' "$B/ex.build"
grep -E 'tests passed|Failed|\*\*\*' "$B/suite.ctest" "$B/rel.ctest"
tmp-rm "$B"
```

The gate passes when `suite=0`, `release=0`, and the only `FAILED:` line
names `example_window_win32.cpp.o`. The examples use a Win32 window on every
platform except Apple, so on Linux only that file fails, and every other
example source still compiles. Any other `FAILED:` line fails the gate. On
Linux a missing `FAILED:` line also fails the gate: it means that the
examples step did not build at all.

The Release run exists because `assert_vk` is a no-op under `NDEBUG`, and the
first build has no `CMAKE_BUILD_TYPE`.

The upstream test `test_command_context` fails sometimes ("Timestamp readback
failed in batch 3"), also without the fork commits. If it is the only
failure, run the gate one more time. Record the flake in the notes of the bd
bead that drove the merge, or tell the owner when no bead exists. Do not fix
it here.

## When a check fails

- A failed gate blocks the push. A second `test_command_context` failure in
  a row counts as a failed gate. Fix the cause on its topic branch and merge
  the topic again. Before a push, the owner may instead undo the merge.
- A commit that the branch check prints belongs on a topic branch. Before a
  push, ask the owner to move it there. After a push, `integration` keeps it,
  because the branch is never rewritten. Tell the owner. When the owner
  accepts it, record its SHA in this file, and from then on run the check
  with that SHA in place of `main`.
- Never push past a failed check without the owner's yes.

## Upstream sync

1. `git fetch upstream`, then fast-forward `main` to `upstream/main`.
2. Merge `main` into `integration` with `--no-ff`. Run the gate.
3. Merge `main` into a topic only when a fix on that topic needs the new
   upstream code.
4. Consumers move their pin to the new `integration` SHA.

## Issue tracking

This project uses **bd** (beads). The database is `nga` on the shared Dolt
server (`bd dolt show`). `git config --local beads.role maintainer` is set in
this clone. Without that role, bd sends new beads to `~/.beads-planning`.

Issues live in that database. `.beads/` is local to this clone
(`.git/info/exclude`), so it never reaches a pull request. `.beads/issues.jsonl`
is a passive export, not the source of truth.

`bd dolt show` lists remotes. The list is empty: this repo has no beads sync,
and `bd dolt push` fails. Do not point `sync.remote` at `origin`. That remote
is the public fork.

## Bead epics
File every new bead except a top-level epic under one of these (`bd create --parent`), or under one of their sub-epics (`bd children <epic>`).
If nothing fits, use the triage epic, `bd config get custom.filing.triage-epic`. A child inherits the `theme:` label on create.

| id | theme | label |
|----|-------|-------|
| nga-col | Triage — unclassified, needs placing | theme:triage |

The rules are in `~/.claude/CLAUDE.md` under `## Filing Beads`. An orphan lands in triage through the `on_create` hook, and the daily `beads-hygiene` sweep reports what the hook missed.
