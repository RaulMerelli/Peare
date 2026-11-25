"""Native Peare sessions and payload operations for the local MCP server."""

import ctypes as C
import os
import sys
from contextlib import ExitStack, contextmanager
from functools import cached_property
from pathlib import Path
from threading import Lock

from peare_agent import (
    HANDLE, Blob, BlobArray, ResourceContext, abi_header, bind_decoder,
    bind_opener, bind_writer, enum_names, load_library, text,
)

PTR = C.c_void_p
STR = C.c_char_p
SIZE = C.c_size_t
U32 = C.c_uint32
U64 = C.c_uint64
INT = C.c_int
READ = C.CFUNCTYPE(INT, PTR, U64, PTR, SIZE, C.POINTER(SIZE))
RELEASE = C.CFUNCTYPE(None, PTR)
RESOLVE = C.CFUNCTYPE(INT, PTR, PTR, SIZE, PTR, SIZE, C.POINTER(PTR), C.POINTER(SIZE))
VISIT_FILE = C.CFUNCTYPE(INT, PTR, STR)
VISIT_TARGET = C.CFUNCTYPE(INT, PTR, SIZE, STR, U32)
NOT_FOUND = 4
OPEN_FAILED = 6
NOT_APPLICABLE = 9
INTEGRITY_FAILED = 13
MAX_DECODE = 64 * 1024 * 1024
CHUNK = 1024 * 1024


class PeareError(Exception):
    def __init__(self, code, message, **details):
        super().__init__(message)
        self.details = {"code": code, "message": message, **details}


class FontOptions(C.Structure):
    _fields_ = [(name, U32) for name in ("scale", "padding", "foreground", "background")]


class Codepoints(C.Structure):
    _fields_ = [("items", C.POINTER(U32)), ("count", SIZE), ("glyph_index_mode", INT)]


def utf8(value):
    if value is None:
        return None
    value = str(value)
    if "\0" in value:
        raise PeareError("invalid_argument", "A path or string contains a NUL character.")
    return value.encode("utf-8")


def declare(lib, signatures):
    for name, (arguments, result) in signatures.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = arguments, result


def native_library(name, binder):
    stem = "Peare" + name.title()
    env = "PEARE_" + name.upper() + "_LIB"
    path = os.environ.get(env)
    if getattr(sys, "frozen", False):
        root = Path(sys.executable).parent / "native"
        library = stem + ".dll" if os.name == "nt" else "lib" + stem + ".so"
        path = path or str(root / library)
        os.environ.setdefault("QT_PLUGIN_PATH", str(root / "plugins"))
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    elif name == "decoder" and os.name == "nt":
        from peare_agent import ROOT
        plugins = ROOT / "build-windows" / "test-plugins"
        if (plugins / "platforms" / "qoffscreen.dll").is_file():
            os.environ.setdefault("QT_QPA_PLATFORM_PLUGIN_PATH", str(plugins / "platforms"))
            os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    lib = load_library(env, stem, path)
    binder(lib)
    return lib


class Native:
    @cached_property
    def opener(self):
        lib = native_library("opener", bind_opener)
        declare(lib, {
            "peare_opener_set_password": ([HANDLE, STR], INT),
            "peare_opener_set_tibx_private_key": ([HANDLE, PTR, SIZE], INT),
            "peare_opener_set_tibx_key_passphrase": ([HANDLE, STR], INT),
            "peare_opener_open_parts": ([HANDLE, C.POINTER(HANDLE), SIZE], INT),
            "peare_source_create_callback": (
                [U64, STR, PTR, READ, RELEASE, C.POINTER(HANDLE)], INT),
            "peare_opener_find_resource": (
                [HANDLE, STR, STR, STR, C.POINTER(HANDLE)], INT),
            "peare_resource_get_archive_path": ([HANDLE, C.POINTER(Blob)], INT),
            "peare_resource_get_converted_extensions": ([HANDLE, C.POINTER(BlobArray)], INT),
            "peare_resource_convert": ([HANDLE, STR, C.POINTER(BlobArray)], INT),
            "peare_resource_conversion_array_free": ([C.POINTER(BlobArray)], None),
            "peare_resource_read": ([HANDLE, U64, PTR, SIZE, C.POINTER(SIZE)], INT),
        })
        return lib

    @cached_property
    def decoder(self):
        lib = native_library("decoder", bind_decoder)
        hints = [PTR, SIZE, STR, STR]
        declare(lib, {
            "peare_decode_images_with_resolver": (
                hints + [RESOLVE, PTR, C.POINTER(BlobArray)], INT),
            "peare_decoder_get_converted_extensions": (
                hints + [RESOLVE, PTR, C.POINTER(BlobArray)], INT),
            "peare_decoder_convert_resource": (
                hints + [STR, RESOLVE, PTR, C.POINTER(BlobArray)], INT),
            "peare_font_render_with_codepage": (
                hints + [U32, STR, C.POINTER(FontOptions), C.POINTER(Blob)], INT),
            "peare_font_render_glyph_index_with_codepage": (
                hints + [U32, U32, C.POINTER(FontOptions), C.POINTER(Blob)], INT),
            "peare_font_get_codepoints_with_codepage": (
                hints + [U32, C.POINTER(Codepoints)], INT),
            "peare_font_codepoint_array_free": ([C.POINTER(Codepoints)], None),
            "peare_font_get_collection_faces": ([PTR, SIZE, C.POINTER(BlobArray)], INT),
            "peare_decoder_release_font_runtime": ([], None),
            "peare_blob_free": ([C.POINTER(Blob)], None),
        })
        return lib

    def close(self):
        decoder = self.__dict__.get("decoder")
        if decoder is not None:
            decoder.peare_decoder_release_font_runtime()

    @cached_property
    def writer(self):
        lib = native_library("writer", bind_writer)
        declare(lib, {
            "peare_writer_set_password": ([HANDLE, STR], INT),
            "peare_writer_visit_update_targets": ([STR, VISIT_TARGET, PTR], INT),
            "peare_writer_visit_update_files": ([STR, STR, VISIT_FILE, PTR], INT),
            "peare_writer_get_update_target_disk_offset": (
                [STR, SIZE, C.POINTER(U64)], INT),
            "peare_writer_create_file": ([HANDLE, STR, STR], INT),
            "peare_writer_replace_entry_by_id": ([HANDLE, STR, STR, C.c_int64], INT),
            "peare_writer_rename_entry_by_id": ([HANDLE, STR, STR, C.c_int64], INT),
            "peare_writer_delete_entry_by_id": ([HANDLE, STR, INT, C.c_int64], INT),
        })
        return lib

    @cached_property
    def formats(self):
        return enum_names()

    @cached_property
    def status_names(self):
        header = abi_header("opener", "peare_types.h")
        body = header.split("typedef enum peare_status {", 1)[1].split("}", 1)[0]
        return [line.split("PEARE_STATUS_", 1)[1].split()[0].rstrip(",").lower()
                for line in body.splitlines() if "PEARE_STATUS_" in line]

    def check(self, status, action):
        if status:
            message = self.opener.peare_status_message(status).decode("utf-8", "replace")
            code = self.status_names[status] if 0 <= status < len(self.status_names) else "native_error"
            raise PeareError(code, f"{action}: {message}", status=status)

    @contextmanager
    def session(self, path=None, *, parts=(), password=None, private_key=None,
                key_passphrase=None, parent_resource=None):
        lib = self.opener
        with ExitStack() as backing, ExitStack() as stack:
            handle = HANDLE()
            self.check(lib.peare_opener_create(C.byref(handle)), "Create Opener session")
            stack.callback(lib.peare_opener_destroy, handle)
            if password is not None:
                self.check(lib.peare_opener_set_password(handle, utf8(password)), "Set password")
            if private_key is not None:
                self.check(lib.peare_opener_set_tibx_private_key(
                    handle, private_key, len(private_key)), "Set private key")
            if key_passphrase is not None:
                self.check(lib.peare_opener_set_tibx_key_passphrase(
                    handle, utf8(key_passphrase)), "Set key passphrase")
            if parent_resource:
                source = HANDLE()
                self.check(lib.peare_resource_get_source(
                    parent_resource, C.byref(source)), "Open nested source")
                stack.callback(lib.peare_source_destroy, source)
                self.check(lib.peare_opener_open(handle, source), "Open nested container")
            elif parts:
                with ExitStack() as sources:
                    handles = [self.file_source(backing, sources, p) for p in parts]
                    native_parts = (HANDLE * len(handles))(*handles)
                    self.check(lib.peare_opener_open_parts(
                        handle, native_parts, len(handles)), "Open multipart container")
            else:
                self.check(lib.peare_opener_open_file(handle, utf8(path)), "Open file")
            yield handle

    def file_source(self, lifetime, sources, path):
        stream = lifetime.enter_context(path.open("rb"))
        identity = self.file_identity(os.fstat(stream.fileno()))
        lock = Lock()

        @READ
        def read(_, offset, buffer, capacity, amount):
            try:
                with lock:
                    if self.file_identity(os.fstat(stream.fileno())) != identity:
                        return INTEGRITY_FAILED
                    stream.seek(offset)
                    data = stream.read(min(capacity, CHUNK))
                if data:
                    C.memmove(buffer, data, len(data))
                amount[0] = len(data)
                return 0
            except (OSError, ValueError):
                return INTEGRITY_FAILED

        @RELEASE
        def release(_):
            pass

        source = HANDLE()
        self.check(self.opener.peare_source_create_callback(
            identity[2], utf8(path.name), None, read, release, C.byref(source)), "Open part source")
        sources.callback(self.opener.peare_source_destroy, source)
        lifetime.callback(self.keep_callbacks, read, release)
        return source

    @staticmethod
    def keep_callbacks(*callbacks):
        pass

    @staticmethod
    def file_identity(stat):
        return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns

    @contextmanager
    def resource(self, session, key):
        handle = HANDLE()
        self.check(self.opener.peare_opener_open_resource_at(
            session, *key, C.byref(handle)), "Open entry")
        try:
            yield handle
        finally:
            self.opener.peare_resource_destroy(handle)

    def blob(self, fn, *args, optional=()):
        result = Blob()
        try:
            status = fn(*args, C.byref(result))
            if status in optional:
                return b""
            self.check(status, fn.__name__)
            return result.read()
        finally:
            self.opener.peare_blob_free(C.byref(result))

    def description(self, session):
        fmt = INT()
        lib = self.opener
        self.check(lib.peare_opener_get_container_format(session, C.byref(fmt)), "Read format")
        desc = self.blob(lib.peare_opener_get_description, session)
        return {"format": self.formats.get(fmt.value, str(fmt.value)),
                "description": desc.decode("utf-8", "replace")}

    def rows(self, session):
        lib = self.opener
        folders = SIZE()
        self.check(lib.peare_opener_get_folder_count(session, C.byref(folders)), "List folders")
        for folder in range(folders.value):
            prefix = self.folder(session, folder)
            count = SIZE()
            self.check(lib.peare_opener_get_resource_count(
                session, folder, C.byref(count)), "List entries")
            for index in range(count.value):
                yield self.row(session, (folder, index), prefix)

    def folder(self, session, index):
        group = self.blob(self.opener.peare_opener_get_folder_type, session, index)
        return group.decode("utf-8", "replace").replace("\x1f", "/")

    def row(self, session, key, prefix=None):
        lib = self.opener
        if prefix is None:
            prefix = self.folder(session, key[0])
        with self.resource(session, key) as handle:
            context = ResourceContext()
            try:
                self.check(lib.peare_resource_get_context(
                    handle, C.byref(context)), "Read entry context")
                name = text(context.identifier_utf8)
                resource_type = text(context.type_utf8)
                language = text(context.language_utf8)
                language_leaf = bool(language) and prefix.split("/")[-2:] == [name, language]
                path = prefix if language_leaf else "/".join(filter(None, (prefix, name)))
                archive_path = self.blob(lib.peare_resource_get_archive_path, handle,
                                         optional=(NOT_APPLICABLE,))
                kind = "folder" if resource_type.endswith("_DIR") else (
                    "container" if context.is_container else "file")
                row = {"key": key, "path": path, "name": language if language_leaf else name,
                       "identifier": name, "kind": kind,
                       "type": resource_type, "size": context.data_size,
                       "offset": context.data_offset, "entry_id": context.base_id,
                       "browse": bool(context.is_container),
                       "language": language,
                       "platform": {1: "Windows", 2: "OS/2", 3: "Other"}.get(context.platform)}
                if archive_path:
                    row["archive_path"] = archive_path.decode("utf-8", "strict")
                return row
            finally:
                lib.peare_resource_context_free(C.byref(context))

    def chunks(self, resource, size):
        offset = 0
        buffer = C.create_string_buffer(min(CHUNK, max(1, size)))
        while offset < size:
            amount = SIZE()
            self.check(self.opener.peare_resource_read(
                resource, offset, buffer, min(len(buffer), size - offset), C.byref(amount)),
                "Read payload")
            if not amount.value:
                raise PeareError("incomplete_payload", "The resource ended before its stated size.")
            yield buffer.raw[:amount.value]
            offset += amount.value

    def payload(self, resource, size):
        if size > MAX_DECODE:
            raise PeareError("payload_too_large", "Save this payload to a file to access all its bytes.")
        return b"".join(self.chunks(resource, size))

    def resolver(self, session, language):
        if not session:
            return RESOLVE()
        cache = {}
        total = 0
        lib = self.opener

        @RESOLVE
        def resolve(_, typ, typ_len, name, name_len, data, size):
            nonlocal total
            key = C.string_at(typ, typ_len), C.string_at(name, name_len)
            handle = HANDLE()
            try:
                if key not in cache:
                    status = lib.peare_opener_find_resource(
                        session, *key, utf8(language), C.byref(handle))
                    if status:
                        return status
                    context = ResourceContext()
                    try:
                        self.check(lib.peare_resource_get_context(
                            handle, C.byref(context)), "Read related resource")
                        if context.data_size > MAX_DECODE - total:
                            return NOT_APPLICABLE
                        payload = self.payload(handle, context.data_size)
                    finally:
                        lib.peare_resource_context_free(C.byref(context))
                    total += len(payload)
                    cache[key] = C.create_string_buffer(payload), len(payload)
                buffer, length = cache[key]
                size[0] = length
                data[0] = C.addressof(buffer)
                return 0
            except PeareError as error:
                return error.details.get("status", NOT_APPLICABLE)
            except (ValueError, MemoryError):
                return NOT_APPLICABLE
            finally:
                if handle:
                    lib.peare_resource_destroy(handle)

        return resolve

    def array(self, fn, free, *args, optional=()):
        result = BlobArray()
        try:
            status = fn(*args, C.byref(result))
            if status in optional:
                return []
            self.check(status, fn.__name__)
            if sum(result.items[index].length for index in range(result.count)) > MAX_DECODE:
                raise PeareError("result_too_large", "The conversion exceeds the output size limit.")
            return [result.items[index].read() for index in range(result.count)]
        finally:
            free(C.byref(result))
