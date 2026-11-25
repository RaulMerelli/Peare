# PeareOpener C API

Header:

```c
#include <peare/peare_opener.h>
```

An Opener handle owns one open session. The API opens files and resources through
source handles. See the [format matrix](../SUPPORTED_FORMATS.md) for supported
formats and variants.

The Opener has no font-format support. It opens containers and returns their
resource records as raw bytes with structural context. Pass any resource bytes
to the Decoder without asking the Opener to classify their payload. Apple
resource forks are listed as generic typed records.

## Exports

```c
peare_status peare_opener_create(peare_opener_handle *out_opener);
void peare_opener_destroy(peare_opener_handle opener);
peare_status peare_opener_set_password(
    peare_opener_handle opener,
    const char *password_utf8);
peare_status peare_opener_set_tibx_private_key(
    peare_opener_handle opener,
    const uint8_t *bytes, size_t length);

peare_status peare_source_create_callback(
    uint64_t length, const char *name_utf8, void *context,
    peare_source_read_callback read, peare_source_release_callback release,
    peare_source_handle *out_source);

peare_status peare_resource_get_source(
    peare_resource_handle resource,
    peare_source_handle *out_source);

peare_status peare_opener_open(
    peare_opener_handle opener,
    peare_source_handle source);

peare_status peare_opener_open_parts(
    peare_opener_handle opener,
    const peare_source_handle *parts, size_t part_count);

void peare_source_destroy(peare_source_handle source);

peare_status peare_opener_open_file(
    peare_opener_handle opener,
    const char *path_utf8);

peare_status peare_opener_close(peare_opener_handle opener);

peare_status peare_opener_get_container_format(
    peare_opener_handle opener,
    peare_container_format *out_format);

peare_status peare_opener_get_description(
    peare_opener_handle opener,
    peare_blob *out_description_utf8);

peare_status peare_opener_get_folder_count(
    peare_opener_handle opener,
    size_t *out_count);

peare_status peare_opener_get_folder_type(
    peare_opener_handle opener,
    size_t folder_index,
    peare_blob *out_type_utf8);

peare_status peare_opener_get_resource_count(
    peare_opener_handle opener,
    size_t folder_index,
    size_t *out_count);

peare_status peare_opener_open_resource_at(
    peare_opener_handle opener,
    size_t folder_index,
    size_t resource_index,
    peare_resource_handle *out_resource);

peare_status peare_opener_open_resource(
    peare_opener_handle opener,
    size_t folder_index,
    const char *identifier_utf8,
    const char *preferred_language_utf8,
    peare_resource_handle *out_resource);

peare_status peare_opener_find_resource(
    peare_opener_handle opener,
    const char *type_utf8,
    const char *identifier_utf8,
    const char *preferred_language_utf8,
    peare_resource_handle *out_resource);

void peare_resource_destroy(peare_resource_handle resource);

peare_status peare_resource_get_converted_extensions(
    peare_resource_handle resource,
    peare_blob_array *out_extensions_utf8);

peare_status peare_resource_convert(
    peare_resource_handle resource,
    const char *extension_utf8,
    peare_blob_array *out_files);

void peare_resource_conversion_array_free(peare_blob_array *array);

peare_status peare_resource_get_payload(
    peare_resource_handle resource,
    peare_blob *out_payload);

peare_status peare_resource_read(
    peare_resource_handle resource,
    uint64_t offset,
    uint8_t *buffer,
    size_t capacity,
    size_t *out_read);

peare_status peare_resource_read_original(
    peare_resource_handle resource,
    uint64_t offset,
    uint8_t *buffer,
    size_t capacity,
    size_t *out_read);

peare_status peare_resource_get_context(
    peare_resource_handle resource,
    peare_resource_context *out_context);

void peare_blob_free(peare_blob *blob);
void peare_resource_context_free(peare_resource_context *context);
const char *peare_status_message(peare_status status);
```

## Callback sources

Use `peare_source_create_callback()` to open a source with positioned reads.
`name_utf8` is an optional format hint, not a path. The maximum length is
`INT64_MAX`.

The read callback can run concurrently on different threads. It must return
`PEARE_STATUS_OK` and at least one byte for each nonempty read. Peare retries
short reads and does not call the callback at EOF. A zero-byte result before
EOF, an excessive count, or an error invalidates the source. Later reads and
opens report `PEARE_STATUS_INTEGRITY_FAILED`. Callbacks must not throw across
the C interface.

On success, Peare retains `context` until the last dependent source, opener,
or resource is destroyed. If `release` is not null, Peare calls it exactly
once on the thread that releases the last reference. On failure, ownership
remains with the caller. If `release` is null, the caller must keep `context`
valid until all dependent handles are destroyed.

## Physical Parts

Use `peare_opener_open_parts()` for an ordered set of physical sources. It
does not concatenate the inputs before format-specific validation. The first
source identifies the format; multipart TIB, TIBX and WIM are supported. A single
source is equivalent to `peare_opener_open()`. Unsupported multipart formats
return `PEARE_STATUS_OPEN_FAILED`. An empty set, null source, or directory
source in a multi-part set returns `PEARE_STATUS_INVALID_ARGUMENT`.

Supply each TIBX source with its actual base filename as `name_utf8`. Supply
the base file first, followed by its numbered continuations in order. Peare
checks those names and the archive's volume table; it does not derive a part
number from the array index. Part numbers must be unique and increasing, but
need not be consecutive. Every volume listed in the archive table must have a
matching supplied source. Retention layouts with removed volumes are not yet
verified on an authentic sample.

The call does not consume source handles. The session and its dependent
resources retain the underlying sources and their validation guards. The caller
can destroy the input handles after the call. Each callback's existing release
and concurrency rules still apply. A failed source invalidates dependent
resources. CRC and volume-table checks do not authenticate the individual parts.

Mac TIB version 1 accepts the archive and its `.tib.metadata` companion as two
explicit sources. Slice directories and lazy file reads use experimental parsing
rules; no authentic Mac sample has verified this support.

For numbered TIB backups, supply all physical volumes in order, starting with
volume 1. Peare validates the archive identity, volume numbers and footer table.

## Session lifetime

Call `peare_opener_create()` before other functions. Call
`peare_opener_destroy()` when the handle is no longer needed. Destroy closes
the session and releases the handle.

Call `peare_opener_close()` to close the current container and keep the handle
for another open operation. Use `peare_opener_open_file()` as a convenience
function for a file path.

Call `peare_opener_set_password()` before opening an encrypted archive. Pass a
UTF-8 password. Pass `NULL` to clear the password. Pass an empty string to use
an empty password. A missing or invalid password returns a password status.

For a TIBX archive with a public-key record, call
`peare_opener_set_tibx_private_key()` with an RSA private key in PEM PKCS#1
or PKCS#8 format before opening it. The Opener copies the bytes.
Pass `NULL, 0` to clear the key. The key is distinct from the archive password.
For an encrypted PKCS#8 key, call `peare_opener_set_tibx_key_passphrase()`
with its UTF-8 passphrase. Pass `NULL` to clear it; an empty string is a valid
empty passphrase. Supported encryption is PBES2 with PBKDF2 and AES-CBC.
Openers created for nested sources need their own credentials. Closing or
destroying an Opener clears the credential copies owned by that handle.
An archive that requires a private key returns `PEARE_STATUS_PRIVATE_KEY_REQUIRED`;
an invalid key returns `PEARE_STATUS_PRIVATE_KEY_INVALID`. When both password
and private-key records are available and neither credential was supplied,
the status is `PEARE_STATUS_PASSWORD_OR_PRIVATE_KEY_REQUIRED`.
A missing or invalid key passphrase returns
`PEARE_STATUS_PRIVATE_KEY_PASSPHRASE_REQUIRED` or
`PEARE_STATUS_PRIVATE_KEY_PASSPHRASE_INVALID`, respectively.
PBKDF2 is limited to 1,000,000 total HMAC evaluations across all derived-key
blocks. A key exceeding that limit returns `PEARE_STATUS_PRIVATE_KEY_KDF_LIMIT_EXCEEDED`;
a valid archive password can still unlock an archive that supports both methods.

## Browse resources

1. Call `peare_opener_get_folder_count()`.
2. For each folder, call `peare_opener_get_folder_type()`.
3. Call `peare_opener_get_resource_count()` for that folder.
4. Open an entry with `peare_opener_open_resource_at()`.

Use `peare_opener_open_resource()` to find an entry by identifier within a
folder. Use `peare_opener_find_resource()` to search by type and identifier.
Numeric identifiers use a `#` prefix, such as `#101`.

Call `peare_resource_get_source()` to create a source from a resource.
Pass that source to `peare_opener_open()` to open it. Nested dispatch uses the
same format detection as a top-level file.

## Resource data

`peare_resource_get_payload()` returns a Peare-allocated copy of the payload.
Use `peare_resource_read()` to read a file in chunks. It reads up to `capacity`
bytes and reports the actual count in `out_read`. The resource context reports
the full file size. A read at the end returns zero bytes; an offset past the end
returns `PEARE_STATUS_OUT_OF_RANGE`.

`peare_resource_read_original()` reads the original resource stream with the
same offset and error rules. It differs from `peare_resource_read()` when a
resource has a reconstructed view, such as a filtered Acronis FAT partition.
For other resources, both functions read the same bytes.

`peare_resource_get_context()` returns the container format, platform, type,
identifier, language, code page, and origin. Release it with
`peare_resource_context_free()`.

For a complete module resource, `peare_resource_convert()` can provide a
container-level conversion. For `PE_MODULE`, it rebuilds a file-layout PE from
the loaded image. The Opener owns this operation because it uses module
headers, sections, RVAs, and alignments.

## Descriptions

`peare_opener_get_description()` returns the module description used by the
GUI. For floppy images, it includes capacity, geometry, sector size, and the
detected file system.

For PS3 PUP v1 packages, it reports package and image versions, segment count,
header and data lengths, and readable firmware version text from segment
`0x100`. Segments use lazy stores and appear as `PUP_SEGMENT` resources.
Signature metadata is available, but the Opener does not verify the HMAC.

For APPX, MSIX, APPXBUNDLE, and XAP packages, the description uses the package
manifest. APPX packages inside an APPXBUNDLE remain lazy resources. Package
signatures and AppxBlockMap hashes appear as files, but the Opener does not
verify them cryptographically.

## Siemens firmware

`SIEMENS_IMG` validates the ProSave footer and reads section and chunk streams.
It supports images with no sections. It exposes a rebuilt `NK.bin` and section
data.

`SIEMENS_FWF` reads OMS object scopes and sized blob attributes. It restores
parent names at object boundaries. It exposes rebuilt `NK.bin`, raw flash
images, image parts, and nested OMS streams. When an FSF volume spans several
image parts, it also exposes an assembled FSF and keeps the original parts.

Windows CE ROM inputs include B000FF, raw NB0/XIP, NOSAJ, ARNOLDBOOTBLOCK,
iPAQ NBF, `_wmstore`, Symbol whole-flash, and IMGFS. `SIEMENS_FSF` lists files
from the Windows CE `\\flash` volume. It inflates compressed members and keeps
raw members unchanged.

## Memory ownership

Release each result with its matching Peare function:

- `peare_blob` with `peare_blob_free()`
- `peare_blob_array` with `peare_resource_conversion_array_free()`
- resource handles with `peare_resource_destroy()`
- resource contexts with `peare_resource_context_free()`

Do not release Peare memory with `free()`.

## Example

```c
peare_opener_handle opener = NULL;
peare_status st = peare_opener_create(&opener);
if (st != PEARE_STATUS_OK) return 1;

st = peare_opener_open_file(opener, "APP.EXE");
if (st != PEARE_STATUS_OK) {
    peare_opener_destroy(opener);
    return 1;
}

/* Browse resources here. */

peare_resource_handle resource = NULL;
peare_source_handle source = NULL;
peare_opener_handle nested = NULL;

st = peare_opener_open_resource_at(opener, 0, 0, &resource);
if (st == PEARE_STATUS_OK)
    st = peare_resource_get_source(resource, &source);
if (st == PEARE_STATUS_OK)
    st = peare_opener_create(&nested);
if (st == PEARE_STATUS_OK)
    st = peare_opener_open(nested, source);

peare_opener_destroy(nested);
peare_source_destroy(source);
peare_resource_destroy(resource);

peare_opener_destroy(opener);
```
