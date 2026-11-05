# Peare

**Portable Extractor of Archives, Resources, and Executables**

<p align="center">
  <img src="resources/peare.png" alt="Peare icon" width="192">
</p>

<p align="center">
  <img src="resources/preview.gif" alt="Peare application preview">
</p>

Peare opens executable files, archives, packages, file systems, and disk images.
It shows their contents in a resource tree and decodes supported payloads.
PeareWriter can create or update ZIP and TAR archives, FAT, exFAT, NTFS, and
Ext2/3/4 file systems, and supported VHD, VHDX, and VMDK images. It exposes
generic create, replace, rename, and delete operations for writable targets.
Support and limits depend on the format variant and platform.

> **Development status:** Peare is under active development. Format and sample
> coverage vary. Do not use Peare alone for forensic work, data recovery, or
> integrity checks. See the [project scope](docs/about/PROJECT_SCOPE.md).

## Supported formats

The [format matrix](docs/SUPPORTED_FORMATS.md) lists supported variants and
marks read and write support as `Yes` or `No`. The matrix does not claim that
every variant is production-ready.

## Build and use

- [Build instructions](docs/development/BUILDING.md)
- [GUI guide](docs/user/GUI.md)
- [AI agent use](docs/agents/AI_AGENT_USAGE.md)
- [Test guide](docs/testing/TESTING.md)
- [Platform status](docs/platform/PORTABILITY.md)

## Technical documentation

- [C ABI references](docs/ABI/README.md)
- [Architecture](docs/architecture/ARCHITECTURE.md)
- [Developer guide](docs/development/DEVELOPMENT.md)
- [Add an Opener module](docs/development/ADDING_OPENER.md)
- [Resource compatibility](docs/formats/RESOURCE_COMPATIBILITY.md)
- [Raw payload decoding](docs/formats/RAW_PAYLOAD.md)
- [Desired targets](docs/NEXT_TARGETS.md)
- [Project origin](docs/about/ORIGIN.md)
- [Roadmap](docs/about/ROADMAP.md)

## License and notices

Peare uses the GNU Affero General Public License, version 3. See [LICENSE](LICENSE),
[copyright and trademark notices](COPYRIGHT.md), and [third-party notices](THIRDPARTY.md).
