# Developer guide

## Source layout

| Path | Content |
|---|---|
| `src/app/` | GUI application |
| `src/opener/` | Opener library, C ABI, and container modules |
| `src/decoder/` | Decoder library, C ABI, and resource parsers |
| `src/writer/` | Writer library, C ABI, and write backends |
| `include/peare/` | Installed public headers |

Each library owns its public header source. CMake installs those headers under
`include/peare`.

The shared ABI types live in `src/opener/api/peare/peare_types.h`. The Opener
owns these types because it creates resources, contexts, and status values.
The Decoder also uses them.

## API implementation

The files below implement the exported C interfaces:

- `src/opener/api/Api.cpp`
- `src/decoder/api/Api.cpp`
- `src/writer/api/Api.cpp`

Keep private helpers beside the API that owns them.

## Component rules

- The Opener opens containers. It does not decode payloads.
- The Decoder reads payloads. It does not open containers.
- The Decoder does not link to the Opener.
- The Writer does not link to the Opener.
- The GUI calls the libraries through public C ABI headers.
- Public headers contain no Qt or C++ types.
- Do not enable `WINDOWS_EXPORT_ALL_SYMBOLS`.
- Keep exported symbols explicit and synchronized with public headers.
- Keep the original payload unchanged during decoding.

## Add a decoder

1. Add or update the parser under `src/decoder/resources`.
2. Add its route to `DecoderRoute`.
3. Keep the input payload unchanged.
4. Return images as PNG and text as UTF-8 through the C API.
5. Return `NOT_APPLICABLE` when the payload uses another format.
6. Return `DECODE_FAILED` when the format applies but the payload is invalid.
7. Add tests for routing, output data, and failure cases.

Use the supplied resource type and platform before payload detection. Use
RawDetect for standalone payloads or when context is absent.

## ABI ownership

Initialize each output before the function returns. Release returned memory
through the matching library function. Never release a library-owned pointer
with `free()`.

The public declarations define exported symbols. Keep each new symbol
intentional and document its ownership rules.

## XEX image decryption

`XexModule` decrypts supported encrypted XEX images before it runs the existing
basic or LZX reconstruction path. It reads the encrypted image key from XEX
security data.

The module tries the known retail and development master-key candidates. It
uses AES-128-CBC with a zero IV. For normal compression, it accepts a key only
when the XEX block SHA-1 chain passes.

The implementation is in `src/opener/modules/Crypto.cpp`. It uses the internal
AES-128-CBC code. It does not use an external cryptography library or console
hardware.

## Nested module detection

Detect embedded modules from binary signatures, not file extensions. For
example, an extensionless XEX resource can contain an `MZ` header and a later
`PE\0\0` signature. The Opener then opens the PE module and lists its sections
and resources.

## Add an Opener module

Follow the [Opener module guide](ADDING_OPENER.md). The `ModuleFactory` detects
each nested format. `OpenerSession` opens eligible payloads. Individual modules
expose their own payloads and do not open child containers.

## Add a Writer format

Follow the [Writer format guide](ADDING_WRITER.md). A request for Writer support
means complete CUD support for every advertised writable variant. The guide
covers create-from-zero, existing-target operations, ABI integration, and tests.

## Pre-release build

Run the Linux build command:

```sh
./tools/build/build_linux.sh --arch x64 --clean
```

Make sure that the GUI links to all three libraries. The Decoder and Writer
must not link to the Opener.
