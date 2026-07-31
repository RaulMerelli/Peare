# Adding an Opener module

This guide is written for AI agents and maintainers modifying Peare. Follow it as a checklist. The central rule is that a module parses only its own container and exposes embedded byte payloads exactly once. Nested opening is performed exclusively by `OpenerSession` through `ModuleFactory`.

## 1. Define the format

Add the format to `ModuleFormat` in `src/opener/modules/ModuleFormat.h`, then update:

- `ModuleFormatDetector::detectFile()` with signature-based detection;
- `ModuleFormatDetector::formatName()`;
- `platformFor()` in `src/opener/OpenerSession.cpp`;
- public API mappings when the format is represented in the C ABI.

Detection must use file contents. Do not depend on extensions or resource names. The same detector is used for top-level files and embedded payloads.

## 2. Implement the module

Create `<Format>Module.h/.cpp` under `src/opener/modules` and implement `IModule`. Implement `IResourceContainer` when the format contains resources or embedded files.

Provide the file-opening entry point required by `ModuleFactory`. Keep parsing, validation, decompression, decryption, and reconstruction that belong to the container inside this module.

A module must not instantiate another opener module. In particular, do not call `PeFileModule`, `PeImageModule`, `XuizModule`, another `<Format>Module`, or `ModuleFactory` from a module implementation.

## 3. Expose resources correctly

For every contained file or reconstructed executable, create one `ResourceEntry` containing the complete byte payload in `data`.

Set:

- `name` to the contained logical filename or stable identifier;
- `type` to a type owned by the current container, such as `XEX_RESOURCE` or `OS2_PACK_FILE`;
- `dataSize` to the payload size;
- `dataOffset` to the meaningful source offset when available;
- `hierarchyPath` to the path inside the current container only.

Do not pre-classify an embedded payload as PE, NE, XUIZ, or another foreign format. Leave `format` as `ModuleFormat::Unknown` unless it describes the current module's own structural resource. `ModuleFactory` will detect the payload from its bytes.

Do not append the child module's resources yourself. This produces duplicate paths, format-specific behavior, and recursion outside the central pipeline.

Synthetic entries containing headers, tables, areas, or a whole-container navigation root are not embedded files. Their types must be clearly structural so `OpenerSession` can exclude them from nested detection.

## 4. Register the module once

Add the module sources to `CMakeLists.txt`. Include the module in `ModuleFactory.cpp` and add one dispatch branch in `ModuleFactory::open(const QString&)`.

`ModuleFactory` is the only format-to-module dispatcher. Do not add parallel dispatch tables or special nested-format branches elsewhere.

## 5. Preserve centralized nesting

`OpenerSession::appendEmbeddedResources()` passes every eligible payload to `ModuleFactory::open(const QByteArray&, logicalName)`. This is the single nested-opening path for all combinations, for example:

- PE or XUIZ inside XEX;
- NE inside OS/2 PACK;
- any future supported format inside any container that exposes its complete payload.

When a new structural resource type is not an independent file, add the narrowest possible exclusion to `isStructuralEntry()`. Never exclude a general suffix such as `_MODULE`, because reconstructed executable payloads may legitimately use it.

## 6. Update documentation and tests

Update `docs/API_OPENER.md` and other format lists. Add focused tests or fixtures covering:

1. top-level detection and opening;
2. malformed and truncated input;
3. payload extraction without byte changes;
4. the new format nested inside an existing container;
5. an existing opener nested inside the new format;
6. absence of duplicate `name -> name` nodes and synthetic top-level roots;
7. recursion-cycle and depth protection.

## Review checklist

Before submitting, verify all of the following:

- the module parses only its own format;
- embedded files are exposed once as complete payloads;
- no module directly opens another module;
- detection is signature-based and registered in `ModuleFormatDetector`;
- all dispatch goes through `ModuleFactory`;
- all nested expansion goes through `OpenerSession`;
- standalone trees have no extra filename root;
- nested trees have no duplicated filename wrapper;
- existing nested combinations still open.
