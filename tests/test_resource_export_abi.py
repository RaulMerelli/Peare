import ctypes as C
import os
from pathlib import Path
import struct

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
    library.peare_decoder_get_converted_extensions.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        _Resolver, C.c_void_p, C.POINTER(_BlobArray)]
    library.peare_decoder_get_converted_extensions.restype = C.c_int
    library.peare_decoder_convert_resource.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        C.c_char_p, _Resolver, C.c_void_p, C.POINTER(_BlobArray)]
    library.peare_decoder_convert_resource.restype = C.c_int
    library.peare_blob_array_free.argtypes = [C.POINTER(_BlobArray)]
    return library


def test_group_icon_exports_complete_multilayer_ico(peare_lib, decoder):
    sample = Path(__file__).parent / "formats" / "pe" / "fixtures" / "MerelliBattleShip.exe"
    if not sample.is_file():
        pytest.skip("Personal PE sample is not present in this checkout")
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    resources = {}
    group_payload = None
    try:
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                handle = C.c_void_p()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(handle)) == 0
                try:
                    context = po._Context()
                    assert opener._L.peare_resource_get_context(handle, C.byref(context)) == 0
                    resource_type = context.type_utf8.to_bytes().decode("utf-8", "replace")
                    blob = po._Blob()
                    assert opener._L.peare_resource_get_payload(handle, C.byref(blob)) == 0
                    try:
                        payload = blob.to_bytes()
                    finally:
                        opener._L.peare_blob_free(C.byref(blob))
                    identifier = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                    if resource_type == "RT_GROUP_ICON" and identifier == "#32512":
                        group_payload = payload
                    elif resource_type == "RT_ICON":
                        resources[(resource_type, identifier)] = payload
                    opener._L.peare_resource_context_free(C.byref(context))
                finally:
                    opener._L.peare_resource_destroy(handle)
    finally:
        opener.close()

    assert group_payload is not None
    count = struct.unpack_from("<H", group_payload, 4)[0]
    assert count > 1
    keepalive = {}

    @_Resolver
    def resolve(_user, type_ptr, type_length, name_ptr, name_length, output, output_length):
        key = (C.string_at(type_ptr, type_length).decode(),
               C.string_at(name_ptr, name_length).decode())
        data = resources.get(key)
        if data is None:
            return 4
        storage = (C.c_uint8 * len(data)).from_buffer_copy(data)
        keepalive[key] = storage
        output[0] = C.cast(storage, C.POINTER(C.c_uint8))
        output_length[0] = len(data)
        return 0

    group_buffer = (C.c_uint8 * len(group_payload)).from_buffer_copy(group_payload)
    extensions = _BlobArray()
    status = decoder.peare_decoder_get_converted_extensions(
        group_buffer, len(group_payload), b"Windows", b"RT_GROUP_ICON",
        resolve, None, C.byref(extensions))
    assert status == 0
    try:
        assert [C.string_at(extensions.items[i].bytes, extensions.items[i].length)
                for i in range(extensions.count)] == [b".ico"]
    finally:
        decoder.peare_blob_array_free(C.byref(extensions))

    files = _BlobArray()
    status = decoder.peare_decoder_convert_resource(
        group_buffer, len(group_payload), b"Windows", b"RT_GROUP_ICON", b".ico",
        resolve, None, C.byref(files))
    assert status == 0
    try:
        assert files.count == 1
        ico = C.string_at(files.items[0].bytes, files.items[0].length)
    finally:
        decoder.peare_blob_array_free(C.byref(files))

    reserved, kind, ico_count = struct.unpack_from("<HHH", ico)
    assert (reserved, kind, ico_count) == (0, 1, count)
    directory_end = 6 + ico_count * 16
    assert len(ico) > directory_end
    for index in range(ico_count):
        image_size, image_offset = struct.unpack_from("<II", ico, 6 + index * 16 + 8)
        assert image_size > 0
        assert directory_end <= image_offset < len(ico)
        assert image_offset + image_size <= len(ico)
