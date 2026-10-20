# NoGraphicsAPI fork of hohounk

This checkout is the one shared home for every change to NoGraphicsAPI that a
project of the owner needs.

The upstream coding rules apply to every change here:

@AGENTS.md

## Branches

`main` is a fast-forward mirror of `upstream/main`. Every other branch except
`pr/16-device-diagnostics` is a patch queue on `main`. An upstream sync
replays that queue onto the new `main` and rebuilds `integration` from the
result. Between syncs, a topic gains new commits only.

| Branch | Role | On an upstream sync |
|---|---|---|
| `main` | Mirror of `upstream/main` | Fast-forward only. No fork commits. |
| `linux-wsi` | Linux WSI callbacks, validation forwarding | Replay its code commits onto `main`. |
| `device-lost` | Lost-device reports from submit and the waits | Replay its commits onto `main`. |
| `presentation-device-lost` | Tests that need both `linux-wsi` and `device-lost` | Recreate by merging those two replayed topics, then replay its own commits. |
| `pr/16-device-diagnostics` | Upstream PR #16, frozen at `a48f70e` | Does not move. Not a queue entry. Moves only when the owner takes a newer PR head. |
| `pr/16-fixes` | The PR #16 carry and the local fixes to it | Replay the PR's code commits and the local fixes onto `main`. The frozen branch is not an ancestor of the result. |
| `presentation-pr/16` | Tests that need both `linux-wsi` and `pr/16-fixes` | Recreate by merging those two replayed topics, then replay its own commits. |
| `integration` | **Every consumer pins a commit SHA of this branch** | Rebuild from the new `main` by `--no-ff` merges of the queue, plus `CLAUDE.md` commits. The previous tip is tagged before the branch moves. |

## Rules

- Consumers pin a commit SHA of `integration` through CMake FetchContent,
  with no patch step. Never pin a branch name or a topic.
- Make every NoGraphicsAPI change in this checkout. Never edit a FetchContent
  copy such as `.deps/` of a consumer.
- This checkout stays on `integration`.
- A fix goes on its topic branch, in a worktree under `.worktrees/`. Then the
  topic merges into `integration` with `--no-ff`, in this checkout. A conflict
  in that merge is resolved on the topic and the merge is repeated: the next
  upstream sync drops `integration` and replays the topics, so a resolution
  that exists only on `integration` is lost. Commit only `CLAUDE.md` on
  `integration` directly. Check: this command prints nothing.

  ```bash
  git log --first-parent --no-merges main..integration -- . ':!CLAUDE.md'
  ```

  If it prints a commit, do not push. See "When a check fails".

- When a replay conflicts, keep upstream's API and edit the patch to match.
  Do not put back an API, a field, or a signature that the new `main` removed.
  Before the rebuilt `integration` is committed, `git diff main integration`
  is the whole fork and must not contain such an API.
- The unversioned hook `.git/hooks/pre-commit` rejects a commit with staged
  conflict markers. It covers every worktree. Do not bypass it with
  `--no-verify`. If the hook is missing, restore it from the notes of bead
  torus-fqx.5.11 in torus-engine.
- A new topic starts from `main`, gets its own branch, and joins the queue.
  A topic that fixes code that only another topic holds starts from that
  topic, as `pr/16-fixes` does.
- A consumer that needs a change starts an agent-deck session here:
  `agent-deck launch ~/katsepolygon/forks/NoGraphicsAPI -t "<title>" -c claude --message "<the change>"`.
  The consumer then moves its pin to the new `integration` SHA.
- A local build against this checkout uses
  `-DFETCHCONTENT_SOURCE_DIR_NOGRAPHICSAPI=<this checkout>`. Remove the
  override before a consumer records a pin.
- Never open a pull request or an issue on `sebbbi/NoGraphicsAPI`, and never
  push to `upstream`. The upstream push URL is `DISABLED` in this clone.
- A `git push` to `origin` needs the owner's yes. After an upstream sync the
  push is a force-with-lease of the rewritten branches. Push the
  `pins/integration-<sha>` tag first, in the same update, so pins that
  already exist stay fetchable.

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
  the topic again. Before a push, the owner may instead undo the rebuild.
  The tag `pins/integration-<sha>` is the previous tip.
- A commit that the branch check prints belongs on a topic branch. Before a
  push, ask the owner to move it there. A sync drops it from `integration`
  unless the topic also has it. The tag keeps a pushed copy reachable.
- Never push past a failed check without the owner's yes.

## Upstream sync

1. `git fetch upstream`, then fast-forward `main` to `upstream/main`.
2. Tag the current `integration` tip as `pins/integration-<sha>` before the
   branch moves. The tag is what keeps already-published pins fetchable.
3. Replay the queue onto the new `main`, in this order: `device-lost`,
   `linux-wsi`, `pr/16-fixes`, then recreate `presentation-device-lost` and
   `presentation-pr/16`. Replay code commits. Drop a commit whose tree change
   cancels out, such as a `CLAUDE.md` edit on a topic that does not carry
   that file. Resolve conflicts by the adaptation rule above. Record each
   resolution and its reason in the commit or merge message.
4. Replace `integration` with the new `main`. Merge the queue branches with
   `--no-ff` in the same order. Run the gate.
5. Consumers move their pin to the new `integration` SHA.

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
