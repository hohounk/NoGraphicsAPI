# NoGraphicsAPI fork of hohounk

This checkout is the one shared home for every change to NoGraphicsAPI that a
project of the owner needs. torus-engine is the first consumer.

The upstream coding rules apply to every change here:

@AGENTS.md

## Remotes and branches

- `origin` is the fork `hohounk/NoGraphicsAPI`. `upstream` is `sebbbi/NoGraphicsAPI`.
- `main` follows upstream. Do not commit fork work on it.
- `linux-wsi` is the consumer branch. It starts at upstream
  `04004140f5b3b8ec7c566fd43bfed77d586155f8` and carries these commits:
  1. `feat(wsi)`: the Linux WSI callbacks in `DeviceDesc`
     (`instance_extensions`, `instance_extension_count`, `create_surface`,
     `drawable_extent`, `wsi_user`) and the `surface_extent` helper for a
     surface that reports the extent `0xFFFFFFFF`. This commit builds alone.
  2. `feat(validation)`: each validation message also reaches the error
     callback with the prefix `NoGraphicsAPI validation: `.
     `DeviceDesc::validation` (default true) turns the layer off.
  3. This file.

## Rules for consumers

- A consumer fetches this fork at a pinned commit SHA of `linux-wsi`, through
  CMake FetchContent, with no patch step. Never pin a branch name.
- Make every NoGraphicsAPI change in this checkout. Never edit a FetchContent
  copy such as `.deps/` of a consumer. FetchContent writes those trees again,
  and the change is lost.
- A consumer that needs a change starts an agent-deck session in this
  checkout: `agent-deck launch ~/katsepolygon/forks/NoGraphicsAPI -t "<title>" -c claude --message "<the change>"`.
  The session commits here. The consumer then moves its pin to the new SHA.
- A local build against this checkout uses
  `-DFETCHCONTENT_SOURCE_DIR_NOGRAPHICSAPI=<this checkout>`. Remove the
  override before a consumer records a pin.

## Gate for every change

Build and run the suite in a scratch directory, never in this tree:

```bash
B=$(mktemp -d /tmp/nga.XXXX)
cmake -S . -B "$B" -G Ninja -DNOGRAPHICSAPI_BUILD_TESTS=ON -DNOGRAPHICSAPI_BUILD_EXAMPLES=OFF \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
    "-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++ -lc++abi"
cmake --build "$B"
timeout -k 5 540 ctest --test-dir "$B" --timeout 120
tmp-rm "$B"
```

The expected result is 23 of 23. The upstream test `test_command_context`
fails sometimes ("Timestamp readback failed in batch 3"), also without the
fork commits. If it is the only failure, run the suite one more time and
record the failure. Do not fix it here.

## Upstream sync

Rebase `linux-wsi` on a newer upstream commit only as its own change. Run the
gate after the rebase. Consumers take the result with a SHA bump.

A `git push` to `origin` needs the owner's yes.

Never open a pull request or an issue on `sebbbi/NoGraphicsAPI`, and never
push to `upstream`. The fork does not contribute upstream unless the owner
asks for it.
