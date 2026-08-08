"""Thin ctypes binding over the PeareOpener C ABI, plus a normalized-tree walker.

This talks to the *built* shared library (PeareOpener.dll / libPeareOpener.so)
through the public `peare_opener_*` C ABI only — never the C++ internals. It is
what the golden-master format tests drive, and it doubles as a small reusable
Python binding for the Opener.

Nothing here is Peare-version-specific except the struct layout of
`peare_resource_context`, which mirrors src/opener/api/peare/peare_types.h.
Container-format names are parsed from that header at import time, so they stay in
sync automatically.
"""

from __future__ import annotations

import ctypes as C
import hashlib
import os
import re
from pathlib import Path

# --- locate the header, to map the container-format enum int -> readable name ---

_REPO = Path(__file__).resolve().parent.parent
_TYPES_H = _REPO / "src" / "opener" / "api" / "peare" / "peare_types.h"


def _load_container_names() -> dict[int, str]:
    """{enum value: short name}, e.g. {18: 'FAT'}. Parsed from peare_types.h so the
    mapping never drifts from the ABI.

    The enum only assigns an explicit value to the first member (UNKNOWN = 0); every
    other member is sequential. So we strip comments, extract the members in order,
    and number them from 0. (A naive comma-split miscounts members that share a line
    with an inline comment.)"""
    text = _TYPES_H.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"typedef enum peare_container_format\s*\{(.*?)\}", text, re.S)
    if not m:
        return {}
    body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)   # drop block comments
    body = re.sub(r"//[^\n]*", " ", body)                       # drop line comments
    members = re.findall(r"PEARE_CONTAINER_(\w+)", body)
    return {i: name for i, name in enumerate(members)}


CONTAINER_NAMES = _load_container_names()

# Never materialise a payload larger than this when hashing a leaf (bytes).
_MAX_HASH_BYTES = 64 * 1024 * 1024


# --- ctypes types mirroring the ABI --------------------------------------------

class _Blob(C.Structure):
    _fields_ = [("bytes", C.c_void_p), ("length", C.c_size_t)]

    def to_bytes(self) -> bytes:
        if not self.bytes or self.length == 0:
            return b""
        return C.string_at(self.bytes, self.length)


class _Context(C.Structure):
    _fields_ = [
        ("container_format", C.c_int),
        ("platform", C.c_int),
        ("source_name_utf8", _Blob),
        ("type_utf8", _Blob),
        ("identifier_utf8", _Blob),
        ("language_utf8", _Blob),
        ("codepage", C.c_uint32),
        ("data_offset", C.c_uint64),
        ("data_size", C.c_uint64),
        ("base_id", C.c_int64),
        ("resource_index", C.c_int64),
        ("is_container", C.c_int32),
    ]


_HANDLE = C.c_void_p


def load_library(path: str | os.PathLike) -> "PeareLib":
    return PeareLib(path)


class PeareLib:
    """Binds the C ABI functions of one loaded PeareOpener library."""

    def __init__(self, path: str | os.PathLike):
        path = os.fspath(path)
        # Let dependent DLLs (Qt) resolve from the library's own directory.
        if hasattr(os, "add_dll_directory"):
            try:
                os.add_dll_directory(os.path.dirname(os.path.abspath(path)))
            except OSError:
                pass
        self._lib = C.CDLL(path)
        L = self._lib
        L.peare_opener_create.argtypes = [C.POINTER(_HANDLE)]
        L.peare_opener_create.restype = C.c_int
        L.peare_opener_destroy.argtypes = [_HANDLE]
        L.peare_opener_open_file.argtypes = [_HANDLE, C.c_char_p]
        L.peare_opener_open_file.restype = C.c_int
        L.peare_opener_open.argtypes = [_HANDLE, _HANDLE]
        L.peare_opener_open.restype = C.c_int
        L.peare_source_destroy.argtypes = [_HANDLE]
        L.peare_resource_get_source.argtypes = [_HANDLE, C.POINTER(_HANDLE)]
        L.peare_resource_get_source.restype = C.c_int
        L.peare_opener_get_container_format.argtypes = [_HANDLE, C.POINTER(C.c_int)]
        L.peare_opener_get_container_format.restype = C.c_int
        L.peare_opener_get_folder_count.argtypes = [_HANDLE, C.POINTER(C.c_size_t)]
        L.peare_opener_get_folder_count.restype = C.c_int
        L.peare_opener_get_resource_count.argtypes = [_HANDLE, C.c_size_t, C.POINTER(C.c_size_t)]
        L.peare_opener_get_resource_count.restype = C.c_int
        L.peare_opener_open_resource_at.argtypes = [_HANDLE, C.c_size_t, C.c_size_t, C.POINTER(_HANDLE)]
        L.peare_opener_open_resource_at.restype = C.c_int
        L.peare_resource_destroy.argtypes = [_HANDLE]
        L.peare_resource_get_payload.argtypes = [_HANDLE, C.POINTER(_Blob)]
        L.peare_resource_get_payload.restype = C.c_int
        L.peare_resource_get_context.argtypes = [_HANDLE, C.POINTER(_Context)]
        L.peare_resource_get_context.restype = C.c_int
        L.peare_resource_context_free.argtypes = [C.POINTER(_Context)]
        L.peare_blob_free.argtypes = [C.POINTER(_Blob)]
        L.peare_status_message.argtypes = [C.c_int]
        L.peare_status_message.restype = C.c_char_p

    def status_message(self, code: int) -> str:
        msg = self._lib.peare_status_message(code)
        return msg.decode("utf-8", "replace") if msg else f"status {code}"

    def open_file(self, path: str) -> "Opener | None":
        h = _HANDLE()
        if self._lib.peare_opener_create(C.byref(h)) != 0:
            return None
        if self._lib.peare_opener_open_file(h, path.encode("utf-8")) != 0:
            self._lib.peare_opener_destroy(h)
            return None
        return Opener(self, h)

    def _open_source(self, source: _HANDLE) -> "Opener | None":
        h = _HANDLE()
        if self._lib.peare_opener_create(C.byref(h)) != 0:
            return None
        if self._lib.peare_opener_open(h, source) != 0:
            self._lib.peare_opener_destroy(h)
            return None
        return Opener(self, h)


class Opener:
    def __init__(self, lib: PeareLib, handle: _HANDLE):
        self._lib = lib
        self._L = lib._lib
        self._h = handle

    def close(self):
        if self._h:
            self._L.peare_opener_destroy(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def container_format(self) -> str:
        v = C.c_int()
        self._L.peare_opener_get_container_format(self._h, C.byref(v))
        return CONTAINER_NAMES.get(v.value, str(v.value))

    def folder_count(self) -> int:
        n = C.c_size_t()
        self._L.peare_opener_get_folder_count(self._h, C.byref(n))
        return n.value

    def resource_count(self, folder: int) -> int:
        n = C.c_size_t()
        self._L.peare_opener_get_resource_count(self._h, folder, C.byref(n))
        return n.value

    def normalized_tree(self, prefix: str = "", depth: int = 0, max_depth: int = 8) -> list[dict]:
        """Deterministic, sorted list of every resource reachable from this opener,
        recursing into nested containers. Payload bytes are summarised as sha256 so
        golden manifests stay small while still proving byte-exact extraction."""
        rows: list[dict] = []
        if depth > max_depth:
            return rows
        for folder in range(self.folder_count()):
            for idx in range(self.resource_count(folder)):
                res = _HANDLE()
                if self._L.peare_opener_open_resource_at(self._h, folder, idx, C.byref(res)) != 0:
                    continue
                try:
                    ctx = _Context()
                    if self._L.peare_resource_get_context(res, C.byref(ctx)) != 0:
                        continue
                    # Read EVERY field we need before freeing: peare_resource_context_free
                    # zeroes the struct, so touching ctx afterwards yields 0/UNKNOWN.
                    name = ctx.identifier_utf8.to_bytes().decode("utf-8", "replace")
                    is_container = bool(ctx.is_container)
                    fmt = CONTAINER_NAMES.get(ctx.container_format, str(ctx.container_format))
                    size = int(ctx.data_size)
                    self._L.peare_resource_context_free(C.byref(ctx))
                    path = f"{prefix}/{name}" if prefix else name

                    # sha256 of the payload proves byte-exact extraction. Only hash
                    # leaf files under a sanity cap — never materialise a multi-GB
                    # container (we recurse into those instead).
                    sha = ""
                    if not is_container and 0 < size <= _MAX_HASH_BYTES:
                        blob = _Blob()
                        if self._L.peare_resource_get_payload(res, C.byref(blob)) == 0:
                            data = blob.to_bytes()
                            sha = hashlib.sha256(data).hexdigest() if data else ""
                            self._L.peare_blob_free(C.byref(blob))

                    rows.append({
                        "path": path,
                        "size": size,
                        "is_container": is_container,
                        "format": fmt,
                        "sha256": sha,
                    })

                    if is_container and depth < max_depth:
                        src = _HANDLE()
                        if self._L.peare_resource_get_source(res, C.byref(src)) == 0 and src:
                            child = self._lib._open_source(src)
                            if child:
                                rows.extend(child.normalized_tree(path, depth + 1, max_depth))
                                child.close()
                            self._L.peare_source_destroy(src)
                finally:
                    self._L.peare_resource_destroy(res)
        rows.sort(key=lambda r: r["path"])
        return rows

    def snapshot(self) -> dict:
        return {"container_format": self.container_format(), "tree": self.normalized_tree()}
