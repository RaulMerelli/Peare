import ctypes as C
import os
from pathlib import Path

import pytest
import peare_opener as po


class _Blob(C.Structure):
    _fields_ = [("bytes", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]


class _BlobArray(C.Structure):
    _fields_ = [("items", C.POINTER(_Blob)), ("count", C.c_size_t)]


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
    library.peare_decode_images.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        C.POINTER(_BlobArray)]
    library.peare_decode_images.restype = C.c_int
    library.peare_blob_array_free.argtypes = [C.POINTER(_BlobArray)]
    return library


def _find_adonis_resource(opener):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            handle = C.c_void_p()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, C.byref(handle)) == 0
            try:
                context = po._Context()
                assert opener._L.peare_resource_get_context(handle, C.byref(context)) == 0
                resource_type = context.type_utf8.to_bytes().decode("utf-8", "replace")
                is_container = bool(context.is_container)
                opener._L.peare_resource_context_free(C.byref(context))
                if is_container:
                    source = C.c_void_p()
                    assert opener._L.peare_resource_get_source(handle, C.byref(source)) == 0
                    child = opener._lib._open_source(source)
                    opener._L.peare_source_destroy(source)
                    if child:
                        try:
                            found = _find_adonis_resource(child)
                        finally:
                            child.close()
                        if found:
                            return found
                elif resource_type == "SIEMENS_ADONIS_DATA":
                    payload = po._Blob()
                    assert opener._L.peare_resource_get_payload(handle, C.byref(payload)) == 0
                    try:
                        return resource_type, payload.to_bytes()
                    finally:
                        opener._L.peare_blob_free(C.byref(payload))
            finally:
                opener._L.peare_resource_destroy(handle)
    return None


def test_local_siemens_adonis_firmware_decodes_embedded_images(peare_lib, decoder):
    sample = Path(os.environ.get(
        "PEARE_SIEMENS_ADONIS_SAMPLE",
        Path.home() / "Documents" / "inerente a" / "cerf" / "Immagini siemens" / "Images" / "KTP400ColorPN" / "KTP400pnColor_V12_0_1_5.img"))
    if not sample.is_file():
        pytest.skip("Local Siemens ADONIS firmware sample is not available")

    with peare_lib.open_file(str(sample)) as opener:
        found = _find_adonis_resource(opener)
    assert found is not None, "Opener did not expose the raw ADONIS firmware resource"
    resource_type, payload = found
    assert resource_type == "SIEMENS_ADONIS_DATA"

    source = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    images = _BlobArray()
    status = decoder.peare_decode_images(
        source, len(payload), None, resource_type.encode("ascii"), C.byref(images))
    assert status == 0, f"Decoder C ABI returned status {status}"
    try:
        assert images.count > 0
        assert all(images.items[index].length > 8 for index in range(images.count))
    finally:
        decoder.peare_blob_array_free(C.byref(images))
