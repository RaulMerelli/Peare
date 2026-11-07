# Peare

**Portable Extractor of Archives, Resources, and Executables**

<p align="center">
  <img src="resources/peare.png" alt="Peare icon" width="192">
</p>

<p align="center">
  <img src="resources/preview.gif" alt="Peare application preview">
</p>

<p align="center">
  <a href="https://github.com/RaulMerelli/Peare/releases/download/ci-latest/peare-windows-setup.exe"><img alt="Latest Windows installer" src="https://img.shields.io/badge/Install-Windows-2f6f9f?style=for-the-badge"></a>
  <a href="https://github.com/RaulMerelli/Peare/releases/download/ci-latest/peare-windows-x64.zip"><img alt="Latest portable Windows build" src="https://img.shields.io/badge/Portable-Windows-527fa2?style=for-the-badge"></a>
  <a href="https://github.com/RaulMerelli/Peare/releases/download/ci-latest/peare-linux-x64.tar.gz"><img alt="Latest Linux CI build" src="https://img.shields.io/badge/Latest_CI-Linux-4b7d52?style=for-the-badge"></a>
  <a href="https://github.com/RaulMerelli/Peare/releases/download/ci-latest/peare-android-arm64.apk"><img alt="Latest Android CI build" src="https://img.shields.io/badge/Latest_CI-Android-3c8b52?style=for-the-badge"></a>
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
- [Install Peare](docs/user/INSTALLING.md)
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
