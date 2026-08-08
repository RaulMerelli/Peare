# Opener test suite — how to configure and maintain a test

This guide is written for AI agents and maintainers. It explains how the Opener
tests work and, step by step, how to add or update one **without letting the suite
rot over time**. Read it fully before touching `tests/`.

## What these tests are (and are not)

- They drive the **shipping artifact** — the built `PeareOpener` shared library
  (`.dll`/`.so`) — through the public **C ABI only** (`peare_opener_*`). They never
  touch C++ internals. If it isn't reachable through the ABI, it isn't tested here.
- They cover the **Opener**, not the Decoder. Decoding (PNG/text/font rendering)
  is out of scope for now.
- They are **golden-master** tests: for each fixture, a committed
  `expected/<name>.json` records the exact resource tree the Opener must produce.
  A behavioural change makes the test fail with a readable diff, so drift is always
  visible and intentional. That diff is the anti-rot mechanism — nothing changes
  silently.

## Layout

```
tests/
  POLICY.md            # legal policy for fixtures — READ IT before adding data
  peare_opener.py      # ctypes binding over the C ABI + the normalized-tree walker
  conftest.py          # locates the built library (or skips if absent)
  runner? -> folded into peare_opener.py (Opener.snapshot())
  generate/            # scripts that synthesise every fixture (provenance)
  formats/<fmt>/
    fixtures/          # the committed input files (synthetic or licence-safe)
    expected/          # the golden manifests, one <fixture-stem>.json each
  test_formats.py      # golden-master comparison
  test_robustness.py   # malformed input must not crash
```

## Running the suite

```bash
# from the repo root, after building PeareOpener
export PEARE_OPENER_LIB=build-windows/Release/PeareOpener.dll   # or the .so path
python -m pytest tests -q
```

If the library is not found and `PEARE_OPENER_LIB` is unset, the ABI tests **skip**
(they do not fail): a checkout without a build must stay green.

## The normalized tree (what a manifest contains)

`Opener.snapshot()` returns `{"container_format", "tree"}`. `tree` is a
deterministic, path-sorted list; each row is:

| field | meaning |
|---|---|
| `path` | slash-joined resource path, recursing into nested containers |
| `size` | `data_size` from the resource context (not the materialised length) |
| `is_container` | whether the resource opens as a nested container |
| `format` | the resource's own container format name (e.g. `PE`, `FAT`) |
| `sha256` | sha256 of the payload for **leaf** files under 64 MiB; `""` otherwise |

Payloads are summarised by sha256 so manifests stay tiny while still proving
**byte-exact** extraction — a single wrong byte anywhere fails the test.

## Adding a new format test — step by step

1. **Get a legal fixture.** Read `tests/POLICY.md`. Prefer a **synthetic**
   generator in `tests/generate/` (author the bytes yourself); otherwise use
   licence-safe content and record its source in a `fixtures/README.md`. Never
   commit a copyrighted sample.

2. **Generate it deterministically.** Put the recipe in
   `tests/generate/make_fixtures.py` (or a new script). Determinism matters: fix
   timestamps/UUIDs, avoid tool output that changes run to run. Run the generator;
   it writes into `tests/formats/<fmt>/fixtures/`.

3. **Bootstrap the golden — then verify it by hand.** Run:

   ```bash
   PEARE_UPDATE_EXPECTED=1 python -m pytest tests/test_formats.py -q
   ```

   This writes `expected/<name>.json` from the *current* Opener output. **You must
   now read that JSON and confirm it is correct against ground truth** — the
   generator knows what bytes it put in each file, so cross-check the leaf
   `sha256`, the `container_format`, and the tree shape. The golden is only
   trustworthy if a human/agent has confirmed it once; after that it guards
   forever. Do not commit a golden you have not inspected.

4. **Run it for real** (without update mode) and confirm green:

   ```bash
   python -m pytest tests -q
   ```

5. **Commit** the fixture(s), the `expected/*.json`, and the generator together.

## Updating a golden when behaviour changes intentionally

When a code change alters the Opener output on purpose (a bug fix, a new field, a
better name):

1. Run `python -m pytest tests` and read the failing diff. Confirm the change is
   the one you intended and nothing else moved.
2. Regenerate: `PEARE_UPDATE_EXPECTED=1 python -m pytest tests/test_formats.py`.
3. **Review the `expected/*.json` diff in git** as carefully as code — this is the
   moment where a real regression can be rubber-stamped by accident. Only commit
   if every changed line is explained by your intended change.

If a test fails and you did **not** intend a change, it is a regression: fix the
code, do not update the golden.

## Robustness tests

`test_robustness.py` feeds every fixture truncated and scrambled variants and only
requires that the reader returns without crashing, hanging, or exploding memory.
Each case runs in a child process so a native fault fails one test instead of
killing the run. Adding a fixture automatically extends robustness coverage — no
extra work.

## Rules that keep the suite honest

- **ABI only.** Never reach into C++ internals or parse private structures.
- **Deterministic fixtures.** No timestamps, random UUIDs, or network in a fixture.
- **Ground-truth goldens.** A golden must be confirmed correct once by inspection,
  not blindly captured.
- **Small and legal.** Fixtures are KB-scale and redistributable (see POLICY.md).
- **A failure is a signal, not a nuisance.** Never silence a failing test by
  regenerating the golden unless you have proven the change is intended.
