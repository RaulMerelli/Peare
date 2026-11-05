#!/usr/bin/env python3
"""Machine-readable CLI for Peare's public C APIs."""

import argparse
import base64
import ctypes as C
import hashlib
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HANDLE = C.c_void_p


class Blob(C.Structure):
    _fields_ = [("bytes", C.c_void_p), ("length", C.c_size_t)]

    def read(self):
        return C.string_at(self.bytes, self.length) if self.bytes and self.length else b""


class BlobArray(C.Structure):
    _fields_ = [("items", C.POINTER(Blob)), ("count", C.c_size_t)]


class ResourceContext(C.Structure):
    _fields_ = [
        ("container_format", C.c_int), ("platform", C.c_int),
        ("source_name_utf8", Blob), ("type_utf8", Blob),
        ("identifier_utf8", Blob), ("language_utf8", Blob),
        ("codepage", C.c_uint32), ("data_offset", C.c_uint64),
        ("data_size", C.c_uint64), ("base_id", C.c_int64),
        ("resource_index", C.c_int64), ("is_container", C.c_int32),
    ]


class WriterFormat(C.Structure):
    _fields_ = [("id", C.c_char_p), ("display_name", C.c_char_p),
                ("extension", C.c_char_p)]


class WriterVariant(C.Structure):
    _fields_ = [("id", C.c_char_p), ("display_name", C.c_char_p)]


class WriterInput(C.Structure):
    _fields_ = [("source_path_utf8", C.c_char_p), ("entry_name_utf8", C.c_char_p)]


class WriterOptions(C.Structure):
    _fields_ = [("format_id_utf8", C.c_char_p), ("variant_id_utf8", C.c_char_p),
                ("filesystem_id_utf8", C.c_char_p)]


class ValueBuffer(C.Structure):
    _fields_ = [("data", C.c_void_p), ("length", C.c_size_t)]


class ValueUnion(C.Union):
    _fields_ = [("boolean_value", C.c_int), ("signed_value", C.c_int64),
                ("unsigned_value", C.c_uint64), ("buffer", ValueBuffer)]


class PeareValue(C.Structure):
    _fields_ = [("type", C.c_int), ("value", ValueUnion)]


def text(blob):
    return blob.read().decode("utf-8", "replace")


def resource_path(prefix, folder_type, name):
    segments = [part for part in folder_type.split("\x1f") if part]
    segments.append(name)
    return "/".join(([prefix] if prefix else []) + segments)


def load_library(env_name, stem, cli_path=None):
    candidates = [cli_path] if cli_path else []
    if not cli_path and os.environ.get(env_name):
        candidates.append(os.environ[env_name])
    names = [f"{stem}.dll", f"lib{stem}.so", f"lib{stem}.dylib"]
    for build in ("build-windows/Release", "build-windows", "build/Release", "build"):
        candidates.extend(str(ROOT / build / name) for name in names)
    path = next((Path(value) for value in candidates if value and Path(value).is_file()), None)
    if path is None:
        option = {"PeareOpener": "--opener-lib", "PeareDecoder": "--decoder-lib",
                  "PeareWriter": "--writer-lib"}.get(stem, "--library")
        raise RuntimeError(f"Cannot find {stem}; build Peare or pass {option}")
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(path.resolve().parent))
    return C.CDLL(str(path))


def bind_opener(lib):
    signatures = {
        "peare_opener_create": ([C.POINTER(HANDLE)], C.c_int),
        "peare_opener_destroy": ([HANDLE], None),
        "peare_opener_open_file": ([HANDLE, C.c_char_p], C.c_int),
        "peare_opener_open": ([HANDLE, HANDLE], C.c_int),
        "peare_opener_get_container_format": ([HANDLE, C.POINTER(C.c_int)], C.c_int),
        "peare_opener_get_description": ([HANDLE, C.POINTER(Blob)], C.c_int),
        "peare_opener_get_folder_count": ([HANDLE, C.POINTER(C.c_size_t)], C.c_int),
        "peare_opener_get_folder_type": ([HANDLE, C.c_size_t, C.POINTER(Blob)], C.c_int),
        "peare_opener_get_resource_count": ([HANDLE, C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
        "peare_opener_open_resource_at": ([HANDLE, C.c_size_t, C.c_size_t, C.POINTER(HANDLE)], C.c_int),
        "peare_resource_destroy": ([HANDLE], None),
        "peare_resource_get_payload": ([HANDLE, C.POINTER(Blob)], C.c_int),
        "peare_resource_get_source": ([HANDLE, C.POINTER(HANDLE)], C.c_int),
        "peare_resource_get_context": ([HANDLE, C.POINTER(ResourceContext)], C.c_int),
        "peare_resource_context_free": ([C.POINTER(ResourceContext)], None),
        "peare_blob_free": ([C.POINTER(Blob)], None),
        "peare_status_message": ([C.c_int], C.c_char_p),
        "peare_opener_close": ([HANDLE], C.c_int),
        "peare_source_destroy": ([HANDLE], None),
    }
    for name, (args, result) in signatures.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, result


def enum_names():
    header = (ROOT / "src/opener/api/peare/peare_types.h").read_text(encoding="utf-8")
    match = re.search(r"typedef enum peare_container_format\s*\{(.*?)\}", header, re.S)
    if not match:
        return {}
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match.group(1), flags=re.S)
    return {i: name for i, name in enumerate(re.findall(r"PEARE_CONTAINER_(\w+)", body))}


class OpenerSession:
    def __init__(self, lib, handle):
        self.lib, self.handle = lib, handle

    @classmethod
    def open_file(cls, lib, path):
        handle = HANDLE()
        status = lib.peare_opener_create(C.byref(handle))
        if status:
            raise RuntimeError(f"Opener create failed: {status}")
        status = lib.peare_opener_open_file(handle, str(path).encode("utf-8"))
        if status:
            lib.peare_opener_destroy(handle)
            raise RuntimeError(f"Open failed: {status}: {lib.peare_status_message(status).decode()}")
        return cls(lib, handle)

    def close(self):
        if self.handle:
            self.lib.peare_opener_destroy(self.handle)
            self.handle = None

    def child(self, resource):
        source = HANDLE()
        status = self.lib.peare_resource_get_source(resource, C.byref(source))
        if status:
            return None
        handle = HANDLE()
        status = self.lib.peare_opener_create(C.byref(handle))
        if status == 0:
            status = self.lib.peare_opener_open(handle, source)
        self.lib.peare_source_destroy(source)
        if status:
            if handle:
                self.lib.peare_opener_destroy(handle)
            return None
        return OpenerSession(self.lib, handle)

    def description(self):
        blob = Blob()
        if self.lib.peare_opener_get_description(self.handle, C.byref(blob)):
            return ""
        try:
            return text(blob)
        finally:
            self.lib.peare_blob_free(C.byref(blob))

    def walk(self, names, prefix="", depth=0, max_depth=8, hash_limit=0):
        if depth > max_depth:
            return []
        rows = []
        folder_count = C.c_size_t()
        self.lib.peare_opener_get_folder_count(self.handle, C.byref(folder_count))
        for folder in range(folder_count.value):
            folder_blob = Blob()
            folder_type = ""
            if self.lib.peare_opener_get_folder_type(self.handle, folder, C.byref(folder_blob)) == 0:
                folder_type = text(folder_blob)
                self.lib.peare_blob_free(C.byref(folder_blob))
            count = C.c_size_t()
            self.lib.peare_opener_get_resource_count(self.handle, folder, C.byref(count))
            for index in range(count.value):
                resource = HANDLE()
                if self.lib.peare_opener_open_resource_at(
                        self.handle, folder, index, C.byref(resource)) != 0:
                    continue
                context = ResourceContext()
                try:
                    if self.lib.peare_resource_get_context(resource, C.byref(context)) != 0:
                        continue
                    name = text(context.identifier_utf8)
                    path = resource_path(prefix, folder_type, name)
                    row = {
                        "path": path, "name": name,
                        "type": text(context.type_utf8),
                        "folder": folder_type,
                        "language": text(context.language_utf8),
                        "size": int(context.data_size),
                        "offset": int(context.data_offset),
                        "format": names.get(context.container_format, str(context.container_format)),
                        "is_container": bool(context.is_container),
                    }
                    self.lib.peare_resource_context_free(C.byref(context))
                    if hash_limit and not row["is_container"] and row["size"] <= hash_limit:
                        blob = Blob()
                        if self.lib.peare_resource_get_payload(resource, C.byref(blob)) == 0:
                            row["sha256"] = hashlib.sha256(blob.read()).hexdigest()
                            self.lib.peare_blob_free(C.byref(blob))
                    rows.append(row)
                    if row["is_container"] and depth < max_depth:
                        child = self.child(resource)
                        if child:
                            try:
                                rows.extend(child.walk(names, path, depth + 1, max_depth, hash_limit))
                            finally:
                                child.close()
                finally:
                    if context.identifier_utf8.bytes or context.source_name_utf8.bytes:
                        self.lib.peare_resource_context_free(C.byref(context))
                    self.lib.peare_resource_destroy(resource)
        return rows

    def find_payload(self, target, max_bytes, prefix="", depth=0, max_depth=32):
        if depth > max_depth:
            return None
        folder_count = C.c_size_t()
        self.lib.peare_opener_get_folder_count(self.handle, C.byref(folder_count))
        for folder in range(folder_count.value):
            count = C.c_size_t()
            self.lib.peare_opener_get_resource_count(self.handle, folder, C.byref(count))
            for index in range(count.value):
                resource = HANDLE()
                if self.lib.peare_opener_open_resource_at(
                        self.handle, folder, index, C.byref(resource)) != 0:
                    continue
                context = ResourceContext()
                try:
                    if self.lib.peare_resource_get_context(resource, C.byref(context)) != 0:
                        continue
                    name = text(context.identifier_utf8)
                    folder_blob = Blob()
                    folder_type = ""
                    if self.lib.peare_opener_get_folder_type(
                            self.handle, folder, C.byref(folder_blob)) == 0:
                        folder_type = text(folder_blob)
                        self.lib.peare_blob_free(C.byref(folder_blob))
                    path = resource_path(prefix, folder_type, name)
                    size, is_container = int(context.data_size), bool(context.is_container)
                    self.lib.peare_resource_context_free(C.byref(context))
                    if path == target:
                        if size > max_bytes:
                            raise RuntimeError(f"Payload exceeds --max-bytes ({max_bytes})")
                        blob = Blob()
                        status = self.lib.peare_resource_get_payload(resource, C.byref(blob))
                        if status:
                            raise RuntimeError(f"Payload read failed: {status}")
                        try:
                            return blob.read()
                        finally:
                            self.lib.peare_blob_free(C.byref(blob))
                    if is_container:
                        child = self.child(resource)
                        if child:
                            try:
                                found = child.find_payload(
                                    target, max_bytes, path, depth + 1, max_depth)
                                if found is not None:
                                    return found
                            finally:
                                child.close()
                finally:
                    if context.identifier_utf8.bytes or context.source_name_utf8.bytes:
                        self.lib.peare_resource_context_free(C.byref(context))
                    self.lib.peare_resource_destroy(resource)
        return None


def bind_decoder(lib):
    for name in ("peare_decode_images", "peare_decode_texts"):
        fn = getattr(lib, name)
        fn.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.c_char_p, C.POINTER(BlobArray)]
        fn.restype = C.c_int
    lib.peare_blob_array_free.argtypes = [C.POINTER(BlobArray)]
    lib.peare_blob_array_free.restype = None
    lib.peare_get_info.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p,
                                   C.c_char_p, C.c_int, C.POINTER(PeareValue)]
    lib.peare_get_info.restype = C.c_int
    lib.peare_value_free.argtypes = [C.POINTER(PeareValue)]
    lib.peare_value_free.restype = None


def decoder_info_ids():
    header = (ROOT / "src/opener/api/peare/peare_types.h").read_text(encoding="utf-8")
    match = re.search(r"typedef enum peare_info_id\s*\{(.*?)\}", header, re.S)
    if not match:
        return []
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match.group(1), flags=re.S)
    return list(enumerate(re.findall(r"PEARE_INFO_(\w+)", body)))


def value_json(value):
    if value.type == 1:
        return bool(value.value.boolean_value)
    if value.type == 2:
        return int(value.value.signed_value)
    if value.type == 3:
        return int(value.value.unsigned_value)
    if value.type == 4:
        return C.string_at(value.value.buffer.data,
                           value.value.buffer.length).decode("utf-8", "replace")
    if value.type == 5:
        raw = C.string_at(value.value.buffer.data, value.value.buffer.length)
        return {"base64": base64.b64encode(raw).decode("ascii"), "length": len(raw)}
    return None


def decoder_info(lib, payload, platform, resource_type):
    storage = C.create_string_buffer(payload)
    pointer = C.cast(storage, C.c_void_p)
    platform_bytes = platform.encode("utf-8") if platform else None
    type_bytes = resource_type.encode("utf-8") if resource_type else None
    fields = {}
    for info_id, name in decoder_info_ids():
        if info_id == 0:
            continue
        value = PeareValue()
        status = lib.peare_get_info(pointer, len(payload), platform_bytes,
                                    type_bytes, info_id, C.byref(value))
        if status == 0:
            fields[name] = value_json(value)
        lib.peare_value_free(C.byref(value))
    return fields


def bind_writer(lib):
    signatures = {
        "peare_writer_create": ([C.POINTER(HANDLE)], C.c_int),
        "peare_writer_destroy": ([HANDLE], None),
        "peare_writer_create_format_count": ([], C.c_size_t),
        "peare_writer_get_create_format": ([C.c_size_t, C.POINTER(WriterFormat)], C.c_int),
        "peare_writer_create_variant_count": ([C.c_char_p], C.c_size_t),
        "peare_writer_get_create_variant": ([C.c_char_p, C.c_size_t, C.POINTER(WriterVariant)], C.c_int),
        "peare_writer_create_filesystem_count": ([C.c_char_p], C.c_size_t),
        "peare_writer_get_create_filesystem": ([C.c_char_p, C.c_size_t, C.POINTER(WriterVariant)], C.c_int),
        "peare_writer_add_files_to_ex": ([HANDLE, C.c_char_p, C.POINTER(WriterOptions), C.c_size_t,
                                             C.POINTER(WriterInput), C.c_size_t], C.c_int),
        "peare_writer_update_target_count": ([C.c_char_p], C.c_size_t),
        "peare_writer_get_update_target_name": ([C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t], C.c_int),
        "peare_writer_get_update_capabilities": ([C.c_char_p, C.c_size_t, C.POINTER(C.c_uint32)], C.c_int),
        "peare_writer_update_begin_target": ([HANDLE, C.c_char_p, C.c_size_t], C.c_int),
        "peare_writer_add_file": ([HANDLE, C.c_char_p, C.c_char_p], C.c_int),
        "peare_writer_create_file": ([HANDLE, C.c_char_p, C.c_char_p], C.c_int),
        "peare_writer_create_empty_file": ([HANDLE, C.c_char_p], C.c_int),
        "peare_writer_create_directory": ([HANDLE, C.c_char_p], C.c_int),
        "peare_writer_replace_file": ([HANDLE, C.c_char_p, C.c_char_p], C.c_int),
        "peare_writer_rename_entry": ([HANDLE, C.c_char_p, C.c_char_p], C.c_int),
        "peare_writer_delete_entry": ([HANDLE, C.c_char_p, C.c_int], C.c_int),
        "peare_writer_update_finish": ([HANDLE], C.c_int),
        "peare_writer_abort": ([HANDLE], None),
        "peare_writer_last_error": ([HANDLE], C.c_char_p),
        "peare_writer_status_message": ([C.c_int], C.c_char_p),
    }
    for name, (args, result) in signatures.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, result


def writer_check(lib, status, handle=None):
    if status == 0:
        return
    detail = lib.peare_writer_status_message(status).decode("utf-8", "replace")
    if handle:
        last = lib.peare_writer_last_error(handle)
        if last:
            detail += ": " + last.decode("utf-8", "replace")
    raise RuntimeError(f"Writer error {status}: {detail}")


def writer_formats(lib, selected=None):
    formats = []
    for index in range(lib.peare_writer_create_format_count()):
        item = WriterFormat()
        writer_check(lib, lib.peare_writer_get_create_format(index, C.byref(item)))
        format_id = item.id.decode()
        if selected and selected != format_id:
            continue
        variants = []
        for variant_index in range(lib.peare_writer_create_variant_count(item.id)):
            variant = WriterVariant()
            writer_check(lib, lib.peare_writer_get_create_variant(item.id, variant_index,
                                                                  C.byref(variant)))
            variants.append({"id": variant.id.decode(), "name": variant.display_name.decode()})
        filesystems = []
        for fs_index in range(lib.peare_writer_create_filesystem_count(item.id)):
            filesystem = WriterVariant()
            writer_check(lib, lib.peare_writer_get_create_filesystem(item.id, fs_index,
                                                                      C.byref(filesystem)))
            filesystems.append({"id": filesystem.id.decode(), "name": filesystem.display_name.decode()})
        formats.append({"id": format_id, "name": item.display_name.decode(),
                         "extension": item.extension.decode(), "variants": variants,
                         "filesystems": filesystems})
    return formats


def writer_targets(lib, path):
    path_bytes = str(path).encode("utf-8")
    targets = []
    for index in range(lib.peare_writer_update_target_count(path_bytes)):
        name = C.create_string_buffer(4096)
        writer_check(lib, lib.peare_writer_get_update_target_name(
            path_bytes, index, name, len(name)))
        capabilities = C.c_uint32()
        writer_check(lib, lib.peare_writer_get_update_capabilities(
            path_bytes, index, C.byref(capabilities)))
        names = ((1, "create_file"), (2, "create_empty_file"), (4, "create_directory"),
                 (8, "update_file"), (16, "rename_entry"), (32, "delete_entry"))
        targets.append({"index": index, "name": name.value.decode("utf-8", "replace"),
                        "capabilities": capabilities.value,
                        "operations": [label for bit, label in names if capabilities.value & bit]})
    return targets


def parse_input(value):
    source, separator, entry = value.partition("=")
    if not separator or not source or not entry:
        raise ValueError("Input must use SOURCE=ENTRY form")
    source_path = Path(source).resolve()
    if not source_path.is_file():
        raise ValueError(f"Input file does not exist: {source_path}")
    return str(source_path).encode("utf-8"), entry.encode("utf-8")


def writer_create(lib, args):
    target = Path(args.output)
    if target.exists() and not args.force:
        raise RuntimeError("Output exists; pass --force to replace it")
    if not args.input:
        raise RuntimeError("At least one --input SOURCE=ENTRY is required")
    writer = HANDLE()
    writer_check(lib, lib.peare_writer_create(C.byref(writer)))
    inputs = (WriterInput * len(args.input))(*(WriterInput(*parse_input(v)) for v in args.input))
    options = WriterOptions(args.format.encode(), args.variant.encode(),
                            args.filesystem.encode() if args.filesystem else None)
    try:
        writer_check(lib, lib.peare_writer_add_files_to_ex(
            writer, str(target).encode("utf-8"), C.byref(options), 0, inputs, len(inputs)), writer)
    except Exception:
        lib.peare_writer_abort(writer)
        raise
    finally:
        lib.peare_writer_destroy(writer)
    return {"created": str(target)}


def writer_update(lib, args):
    if not args.confirm_copy:
        raise RuntimeError("Use a disposable copy, then pass --confirm-copy to authorize updates")
    target = Path(args.target)
    path_bytes = str(target).encode("utf-8")
    if args.action == "add" and not args.input:
        raise RuntimeError("The add action requires --input SOURCE=ENTRY")
    if args.action in ("new-file", "mkdir", "replace", "rename", "delete") and not args.entry:
        raise RuntimeError(f"The {args.action} action requires --entry")
    if args.action == "replace" and not args.source:
        raise RuntimeError("The replace action requires --source")
    if args.action == "rename" and not args.new_name:
        raise RuntimeError("The rename action requires --new-name")
    writer = HANDLE()
    writer_check(lib, lib.peare_writer_create(C.byref(writer)))
    try:
        writer_check(lib, lib.peare_writer_update_begin_target(
            writer, path_bytes, args.target_index), writer)
        status = 0
        entry = args.entry.encode("utf-8") if getattr(args, "entry", None) else None
        if args.action == "add":
            source, destination = parse_input(args.input)
            status = lib.peare_writer_add_file(writer, source, destination)
        elif args.action == "new-file":
            status = lib.peare_writer_create_empty_file(writer, entry)
        elif args.action == "mkdir":
            status = lib.peare_writer_create_directory(writer, entry)
        elif args.action == "replace":
            status = lib.peare_writer_replace_file(writer, str(Path(args.source).resolve()).encode(), entry)
        elif args.action == "rename":
            status = lib.peare_writer_rename_entry(writer, entry, args.new_name.encode("utf-8"))
        elif args.action == "delete":
            status = lib.peare_writer_delete_entry(writer, entry, int(args.recursive))
        writer_check(lib, status, writer)
        writer_check(lib, lib.peare_writer_update_finish(writer), writer)
    except Exception:
        lib.peare_writer_abort(writer)
        raise
    finally:
        lib.peare_writer_destroy(writer)
    return {"updated": str(target), "target_index": args.target_index, "action": args.action}


def emit(value):
    print(json.dumps(value, ensure_ascii=False, indent=2))


def main(argv=None):
    parser = argparse.ArgumentParser(prog="peare-agent")
    sub = parser.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser("inspect", help="List a file and nested resources as JSON")
    inspect.add_argument("path")
    inspect.add_argument("--max-depth", type=int, default=8)
    inspect.add_argument("--hash-limit", type=int, default=0)
    inspect.add_argument("--opener-lib")
    extract = sub.add_parser("extract", help="Extract one resource by its displayed path")
    extract.add_argument("path")
    extract.add_argument("resource_path")
    extract.add_argument("output")
    extract.add_argument("--max-bytes", type=int, default=256 * 1024 * 1024)
    extract.add_argument("--force", action="store_true")
    extract.add_argument("--opener-lib")
    decode = sub.add_parser("decode", help="Decode or query payload data through PeareDecoder")
    decode.add_argument("path")
    decode.add_argument("--kind", choices=("images", "texts", "info"), required=True)
    decode.add_argument("--output-dir")
    decode.add_argument("--platform")
    decode.add_argument("--type")
    decode.add_argument("--force", action="store_true")
    decode.add_argument("--decoder-lib")
    writer = sub.add_parser("writer", help="Query or update formats through PeareWriter")
    writer.add_argument("--writer-lib")
    writer_sub = writer.add_subparsers(dest="writer_command", required=True)
    formats = writer_sub.add_parser("formats")
    formats.add_argument("--format")
    targets = writer_sub.add_parser("targets")
    targets.add_argument("path")
    create = writer_sub.add_parser("create")
    create.add_argument("output")
    create.add_argument("--format", required=True)
    create.add_argument("--variant", required=True)
    create.add_argument("--filesystem")
    create.add_argument("--input", action="append", default=[])
    create.add_argument("--force", action="store_true")
    action = writer_sub.add_parser("mutate")
    action.add_argument("target")
    action.add_argument("--target-index", type=int, default=0)
    action.add_argument("--confirm-copy", action="store_true")
    action.add_argument("action", choices=("add", "new-file", "mkdir", "replace", "rename", "delete"))
    action.add_argument("--input")
    action.add_argument("--entry")
    action.add_argument("--source")
    action.add_argument("--new-name")
    action.add_argument("--recursive", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.command == "inspect":
            lib = load_library("PEARE_OPENER_LIB", "PeareOpener", args.opener_lib)
            bind_opener(lib)
            session = OpenerSession.open_file(lib, args.path)
            try:
                value = C.c_int()
                lib.peare_opener_get_container_format(session.handle, C.byref(value))
                emit({"format": enum_names().get(value.value, str(value.value)),
                      "description": session.description(),
                      "resources": session.walk(enum_names(), max_depth=args.max_depth,
                                                hash_limit=args.hash_limit)})
            finally:
                session.close()
        elif args.command == "extract":
            output = Path(args.output)
            if output.exists() and not args.force:
                raise RuntimeError("Output exists; pass --force to replace it")
            lib = load_library("PEARE_OPENER_LIB", "PeareOpener", args.opener_lib)
            bind_opener(lib)
            session = OpenerSession.open_file(lib, args.path)
            try:
                data = session.find_payload(args.resource_path, args.max_bytes)
            finally:
                session.close()
            if data is None:
                raise RuntimeError("Resource path was not found")
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
            emit({"extracted": str(output), "size": len(data),
                  "sha256": hashlib.sha256(data).hexdigest()})
        elif args.command == "decode":
            lib = load_library("PEARE_DECODER_LIB", "PeareDecoder", args.decoder_lib)
            bind_decoder(lib)
            payload = Path(args.path).read_bytes()
            if args.kind == "info":
                emit({"fields": decoder_info(lib, payload, args.platform, args.type)})
                return 0
            if not args.output_dir:
                raise RuntimeError("--output-dir is required for image and text decoding")
            buffer = C.create_string_buffer(payload)
            results = BlobArray()
            fn = getattr(lib, "peare_decode_" + args.kind)
            status = fn(buffer, len(payload),
                         args.platform.encode("utf-8") if args.platform else None,
                         args.type.encode("utf-8") if args.type else None, C.byref(results))
            if status:
                raise RuntimeError(f"Decoder error {status}")
            output_dir = Path(args.output_dir)
            output_dir.mkdir(parents=True, exist_ok=True)
            suffix = ".png" if args.kind == "images" else ".txt"
            targets = [output_dir / f"{Path(args.path).stem}-{index + 1}{suffix}"
                       for index in range(results.count)]
            existing = [str(path) for path in targets if path.exists()]
            if existing and not args.force:
                raise RuntimeError("Output exists: " + ", ".join(existing) +
                                   "; pass --force to replace it")
            paths = []
            try:
                for index, target in enumerate(targets):
                    target.write_bytes(results.items[index].read())
                    paths.append(str(target))
            finally:
                lib.peare_blob_array_free(C.byref(results))
            emit({"decoded": paths})
        else:
            lib = load_library("PEARE_WRITER_LIB", "PeareWriter", args.writer_lib)
            bind_writer(lib)
            if args.writer_command == "formats":
                emit(writer_formats(lib, args.format))
            elif args.writer_command == "targets":
                emit(writer_targets(lib, args.path))
            elif args.writer_command == "create":
                emit(writer_create(lib, args))
            else:
                emit(writer_update(lib, args))
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
