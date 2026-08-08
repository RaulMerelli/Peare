# Test-data policy

Peare's tests must be redistributable with the project. That means **no
copyrighted or licence-encumbered sample files may be committed**, ever.

Every fixture under `tests/formats/**/fixtures/` must be one of:

1. **Synthetic** — generated from scratch by a script in `tests/generate/`, whose
   content is authored in that script (so no third-party bytes are involved). This
   is the default and preferred source. The generator doubles as proof of
   provenance: anyone can read it and confirm the fixture carries no external
   content.

2. **Permissively licensed** — content whose licence explicitly allows
   redistribution (e.g. FreeDoom for WAD, CC0/public-domain assets). When used,
   record the source and licence in the format's `fixtures/README.md`.

**Never commit** a real Acronis/Veeam/game/OS image, a ripped disc, firmware you
were not licensed to redistribute, or any file whose licence you have not checked.

## When a format cannot be sampled legally

Some formats have no synthetic path and no licence-safe sample (proprietary backup
containers, for instance). For those:

- Do **not** commit a real file.
- Where possible, hand-craft a **minimal synthetic header** that exercises
  detection and top-level parsing without reproducing copyrighted payloads.
- Otherwise leave the format in the README roadmap under "implemented but not yet
  verified against any real sample", and verify it privately, out of tree.

The goal: the entire `tests/` tree can be published, audited, and rebuilt by anyone
without a single licensing question.
