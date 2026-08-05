# Developer guide

## Source layout

```text
src/app/                              Peare executable and Qt application code
src/opener/                           PeareOpener library
src/opener/api/                       Opener C ABI implementation
src/opener/api/peare/                 Opener-owned public header sources and shared ABI types
src/opener/modules/                   executable and package modules
src/decoder/resources/                 resource parsers used by PeareDecoder
src/decoder/                          PeareDecoder library
src/decoder/api/                      Decoder C ABI implementation
src/decoder/api/peare/                Decoder public header sources and umbrella header
```

Only the three produced components exist directly below `src`: the application, the Opener library, and the Decoder library.

### Public header source and installation layout

The authoritative public header sources live with the library that owns them:

```text
src/opener/api/peare/peare_types.h
src/opener/api/peare/peare_opener.h
src/decoder/api/peare/peare_decoder.h
src/decoder/api/peare/peare.h
```

CMake installs these files under `include/peare`, so external consumers continue to use stable includes such as `<peare/peare.h>`, `<peare/peare_opener.h>`, and `<peare/peare_decoder.h>`.

The shared ABI types are owned by the Opener API because resource handles, container kinds, payload contexts, and status values originate at the container boundary and are also consumed by the Decoder API.

### API implementation placement

`src/opener/api/Api.cpp` implements the exported C ABI of `PeareOpener.dll`. `src/decoder/api/Api.cpp` implements the exported C ABI of `PeareDecoder.dll`. Private bridge and snapshot helpers remain beside the API implementation that owns them.

## Architectural rules

1. `PeareOpener.dll` does not decode payloads.
2. `PeareDecoder.dll` does not open containers.
3. `PeareDecoder.dll` does not depend on `PeareOpener.dll`.
4. The GUI uses only the public C ABI headers to communicate with the DLLs.
5. Qt and C++ types are not allowed in public headers.
6. Do not use `WINDOWS_EXPORT_ALL_SYMBOLS`.
7. Keep the exported C ABI explicit and synchronized with the public headers.
8. The original payload always remains available and is not modified.

## Adding a decoder

- add or update the parser under `src/decoder/resources`;
- add routing classification in `DecoderRoute`;
- produce internal representations without modifying the payload;
- convert images to PNG and text to UTF-8 in the C API;
- clearly distinguish `NOT_APPLICABLE` from `DECODE_FAILED`;
- add focused validation for the new routing and output behavior.

## Routing

Do not use RawDetect as a universal entry point. Routing must prefer the supplied type and platform, then inspect the payload. RawDetect is required for standalone use or absent context.

## ABI ownership

Every function must initialize its output even on failure. All returned memory must be releasable by the DLL that produced it.

## Exports

Exports are defined by the public C ABI declarations and the platform visibility macros. Keep new symbols intentional, documented, and owned by the library that allocates their returned memory.

## Pre-release checks

```sh
./build_linux.sh --arch x64 --clean
```

Confirm that the GUI links against both shared libraries and that `PeareDecoder` does not link against `PeareOpener`.

## XEX AES image reconstruction

`XexModule` supports normal XEX image encryption before applying the existing basic or LZX reconstruction path. The encrypted 16-byte image key is read from the XEX security information, decrypted with the known retail and development master-key candidates using AES-128-CBC with a zero IV, and then used as the session key for the complete image payload. For normal compression, a candidate is accepted only when the XEX block SHA-1 chain validates. Decryption is implemented directly in `src/opener/modules/Crypto.cpp` by the project's internal AES-128-CBC implementation; no external cryptography backend is used. It does not require Xbox hardware, a console-specific user key, or a platform cryptography framework.

### Embedded module detection

Embedded executables and containers must be detected from their binary signatures, not from file-name extensions. In particular, an extensionless XEX native resource beginning with a valid `MZ` header and a reachable `PE\\0\\0` signature is opened as a nested PE module and exposes its internal sections and resources in the tree.

## Adding an Opener

Follow [Adding an Opener module](ADDING_OPENER.md). All nested formats must be detected by `ModuleFactory` and expanded by `OpenerSession`; individual modules only expose their own contained payloads.
