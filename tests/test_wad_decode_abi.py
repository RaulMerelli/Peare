import ctypes as C
import os
from pathlib import Path
import zipfile

import pytest
import peare_opener as po


class _Blob(C.Structure):
    _fields_ = [("bytes", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]


class _BlobArray(C.Structure):
    _fields_ = [("items", C.POINTER(_Blob)), ("count", C.c_size_t)]


_Resolver = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_size_t,
                        C.c_char_p, C.c_size_t, C.POINTER(C.POINTER(C.c_uint8)),
                        C.POINTER(C.c_size_t))


@pytest.fixture(scope="module")
def decoder(peare_decoder_path):
    if peare_decoder_path.suffix.lower() == ".dll" and hasattr(os, "add_dll_directory"):
        dll_directory = os.add_dll_directory(str(peare_decoder_path.resolve().parent))
    else:
        dll_directory = None
    try:
        library = C.CDLL(str(peare_decoder_path))
    finally:
        if dll_directory:
            dll_directory.close()
    library.peare_decode_images_with_resolver.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        _Resolver, C.c_void_p, C.POINTER(_BlobArray)]
    library.peare_decode_images_with_resolver.restype = C.c_int
    library.peare_blob_array_free.argtypes = [C.POINTER(_BlobArray)]
    return library


def _decode(decoder, payload, resource_type, lumps):
    keepalive = {}

    @_Resolver
    def resolve(_user, _type_ptr, _type_length, name_ptr, name_length, output, output_length):
        name = C.string_at(name_ptr, name_length).decode("ascii").upper()
        resource_type = C.string_at(_type_ptr, _type_length).decode("ascii")
        data = lumps.get((resource_type, name))
        if data is None:
            return 4
        storage = (C.c_uint8 * len(data)).from_buffer_copy(data)
        keepalive[name] = storage
        output[0] = C.cast(storage, C.POINTER(C.c_uint8))
        output_length[0] = len(data)
        return 0

    source = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    images = _BlobArray()
    status = decoder.peare_decode_images_with_resolver(
        source, len(payload), None, resource_type.encode(), resolve, None, C.byref(images))
    if status != 0:
        return status, []
    try:
        return status, [C.string_at(images.items[i].bytes, images.items[i].length)
                        for i in range(images.count)]
    finally:
        decoder.peare_blob_array_free(C.byref(images))


def _read_resources(opener):
    resources = []
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            handle = C.c_void_p()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, C.byref(handle)) == 0
            try:
                context = po._Context()
                assert opener._L.peare_resource_get_context(handle, C.byref(context)) == 0
                resource_type = context.type_utf8.to_bytes().decode("utf-8", "replace")
                name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                opener._L.peare_resource_context_free(C.byref(context))
                blob = po._Blob()
                assert opener._L.peare_resource_get_payload(handle, C.byref(blob)) == 0
                try:
                    resources.append((resource_type, name, blob.to_bytes()))
                finally:
                    opener._L.peare_blob_free(C.byref(blob))
            finally:
                opener._L.peare_resource_destroy(handle)
    return resources


def test_local_doom_iwad_sprite_preview(peare_lib, decoder, tmp_path):
    archive = Path(os.environ.get(
        "PEARE_DOOM_IWADS_ZIP",
        Path.home() / "Downloads" / "All-Doom-IWADS-main.zip"))
    if not archive.is_file():
        pytest.skip("Local Doom IWAD archive is not available")
    with zipfile.ZipFile(archive) as source:
        member = next((name for name in source.namelist() if name.lower().endswith("/doom.wad")), None)
        if member is None:
            pytest.skip("Local archive does not contain DOOM.WAD")
        sample = tmp_path / "DOOM.WAD"
        sample.write_bytes(source.read(member))

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    try:
        folder_types = {opener.folder_type(i) for i in range(opener.folder_count())}
        resources = _read_resources(opener)
    finally:
        opener.close()

    assert folder_types == {""}
    assert any(resource_type == "WAD_MAP_HEADER" for resource_type, _, _ in resources)
    lump_payloads = {(resource_type, name.upper()): payload
                     for resource_type, name, payload in resources}
    sprite = next(((name, payload) for resource_type, name, payload in resources
                   if resource_type == "WAD_DOOM_SPRITE" and len(payload) >= 8), None)
    texturex = lump_payloads.get(("WAD_TEXTUREX", "TEXTURE1"))
    assert sprite is not None, "DOOM.WAD has no sprite entries recognized by the Opener"
    name, payload = sprite
    status, images = _decode(decoder, payload, "WAD_DOOM_SPRITE", lump_payloads)
    assert status == 0, f"Could not preview local Doom sprite {name}"
    assert images
    assert texturex is not None, "DOOM.WAD has no TEXTURE1 lump recognized by the Opener"
    status, images = _decode(decoder, texturex, "WAD_TEXTUREX", lump_payloads)
    assert status == 0, "Could not compose local Doom TEXTURE1 resources"
    assert images
