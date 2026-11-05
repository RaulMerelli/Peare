# Project scope

Peare focuses on historical executable files, application packages, archives,
resources, file systems, and disk images.

## Goals

- Open executable modules and application packages.
- Show their contents as resource trees.
- Preserve the original payload bytes during extraction.
- Decode supported images, text, fonts, menus, dialogs, and other resources.
- Keep the GUI separate from the backends.
- Use a stable C ABI without Qt or C++ types in public calls.
- Add format support to the core behind the shared ABI.

## Desired work

See the [desired targets list](../NEXT_TARGETS.md) for planned formats,
improvements, and sample validation. These items are not current support claims.

## Project status

Peare is under active development. It is not presented as production-stable
software.

Historical formats can contain undocumented structures, vendor variants,
damaged data, and platform-specific assumptions. The project prioritizes:

- Preserving original payload bytes.
- Reporting errors instead of fabricating output.
- Testing public ABI behavior.
- Adding support from verified samples and documented structures.

Do not use Peare as the only tool for forensic work, data recovery, or integrity
verification. Independently compare any critical output.
