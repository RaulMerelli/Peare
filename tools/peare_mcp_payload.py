"""Read, decode, convert, and export Peare payloads."""

import base64
import ctypes as C
import hashlib
import tempfile
from contextlib import contextmanager
from pathlib import Path, PureWindowsPath

from peare_agent import Blob, decoder_info
from peare_mcp_browse import output_path
from peare_mcp_native import (
    MAX_DECODE, NOT_APPLICABLE, Codepoints, FontOptions, PeareError, utf8,
)
from peare_mcp_write import publish, unused


def image_block(data):
    return {"type": "image", "data": base64.b64encode(data).decode("ascii"), "mimeType": "image/png"}


def safe_name(name):
    if (not name or name in (".", "..") or name.rstrip(" .") != name
            or any(ord(char) < 32 or char in '/\\:<>|"*?' for char in name)
            or PureWindowsPath(name).is_reserved()):
        raise PeareError("unsafe_export_name", f"Cannot use the entry as a local file name: {name!r}")
    return name


class Payload:
    def __init__(self, native, browser):
        self.native = native
        self.browser = browser

    def locate(self, args):
        location, auth = self.browser.select(args)
        return self.browser.locate(location, auth, args.get("inside")), auth

    @contextmanager
    def source(self, args):
        location, auth = self.locate(args)
        if location.entry:
            with self.browser.payload_entry(location, auth) as (session, row, resource):
                payload = self.native.payload(resource, row["size"])
                yield payload, row, session, resource
        elif not location.hops and not location.prefix and not location.parts:
            with location.file.open("rb") as stream:
                payload = stream.read(MAX_DECODE + 1)
            if len(payload) > MAX_DECODE:
                raise PeareError("payload_too_large", "Select an entry or a smaller standalone file.")
            yield payload, {"name": location.file.name, "size": len(payload)}, None, None
        else:
            raise PeareError("entry_required", "Select an entry or supply a standalone file.")

    def read(self, args):
        mode = args.get("as", "auto")
        limit = args.get("limit", 12000)
        offset = args.get("offset", 0)
        with self.source(args) as (payload, row, session, resource):
            result = {"name": row["name"], "size": len(payload),
                      "sha256": hashlib.sha256(payload).hexdigest()}
            if mode == "hex":
                data = payload[offset:offset + limit]
                return {**result, "hex": data.hex(" "), "offset": offset,
                        "next_offset": offset + len(data) if offset + len(data) < len(payload) else None}
            platform = args.get("platform", row.get("platform"))
            typ = args.get("resource_type", row.get("type"))
            lib = self.native.decoder
            hints = payload, len(payload), utf8(platform), utf8(typ)
            resolver = self.native.resolver(session, row.get("language"))
            if mode == "info":
                result["fields"] = decoder_info(lib, payload, platform, typ)
                result["conversions"] = self.extensions(hints, resolver, resource)
                return result
            if mode in ("auto", "image"):
                pngs = self.images(hints, resolver, typ)
                if pngs:
                    index = args.get("item", 0)
                    if index >= len(pngs):
                        raise PeareError("item_out_of_range", f"This resource has {len(pngs)} images.")
                    if len(pngs[index]) > 4 * 1024 * 1024:
                        raise PeareError("image_too_large", "Convert the image to a local output file.")
                    return {**result, "images": len(pngs), "item": index}, [image_block(pngs[index])]
            if mode in ("auto", "text"):
                rows = self.native.array(lib.peare_decode_texts, lib.peare_blob_array_free,
                                         *hints, optional=(NOT_APPLICABLE,))
                if rows or not payload:
                    joined = "\n".join(item.decode("utf-8", "replace") for item in rows)
                    value = joined[offset:offset + limit]
                    return {**result, "text": value, "offset": offset,
                            "next_offset": offset + len(value)
                            if offset + len(value) < len(joined) else None}
            raise PeareError("decode_not_applicable",
                             "Peare has no preview for this payload. Use info, hex, or extract.")

    def images(self, hints, resolver, typ):
        lib = self.native.decoder
        group_extension = {"RT_GROUP_ICON": b".ico", "RT_GROUP_CURSOR": b".cur"}.get(typ)
        if resolver and group_extension:
            files = self.native.array(lib.peare_decoder_convert_resource,
                                      lib.peare_blob_array_free, *hints, group_extension,
                                      resolver, None, optional=(NOT_APPLICABLE,))
            return [png for data in files for png in self.images((data, len(data), None, None),
                                                                 None, None)]
        if resolver:
            return self.native.array(lib.peare_decode_images_with_resolver,
                                     lib.peare_blob_array_free, *hints, resolver, None,
                                     optional=(NOT_APPLICABLE,))
        return self.native.array(lib.peare_decode_images, lib.peare_blob_array_free,
                                 *hints, optional=(NOT_APPLICABLE,))

    def extensions(self, hints, resolver, resource):
        lib = self.native.decoder
        extensions = self.native.array(lib.peare_decoder_get_converted_extensions,
                                       lib.peare_blob_array_free, *hints, resolver, None,
                                       optional=(NOT_APPLICABLE,))
        if resource:
            opener = self.native.opener
            extensions += self.native.array(opener.peare_resource_get_converted_extensions,
                                            opener.peare_resource_conversion_array_free,
                                            resource, optional=(NOT_APPLICABLE,))
        return sorted(set(item.decode("utf-8") for item in extensions))

    def convert(self, args):
        output = unused(output_path(args["output"]))
        extension = args["extension"].lower()
        extension = "." + extension.lstrip(".")
        with self.source(args) as (payload, row, session, resource):
            lib = self.native.decoder
            opener = self.native.opener if resource else None
            source_extensions = self.native.array(
                opener.peare_resource_get_converted_extensions, opener.peare_resource_conversion_array_free,
                resource, optional=(NOT_APPLICABLE,)) if opener else []
            if utf8(extension) in source_extensions:
                files = self.native.array(opener.peare_resource_convert,
                                          opener.peare_resource_conversion_array_free,
                                          resource, utf8(extension))
            else:
                resolver = self.native.resolver(session, row.get("language"))
                files = self.native.array(lib.peare_decoder_convert_resource,
                                          lib.peare_blob_array_free, payload, len(payload),
                                          utf8(args.get("platform", row.get("platform"))),
                                          utf8(args.get("resource_type", row.get("type"))),
                                          utf8(extension), resolver, None)
        if not files:
            raise PeareError("conversion_empty", "The conversion returned no files.")
        if len(files) == 1:
            self.write_bytes(output, files[0])
            paths = [str(output)]
        else:
            with tempfile.TemporaryDirectory(prefix=".peare-", dir=output.parent) as tmp:
                stage = Path(tmp) / "result"
                stage.mkdir()
                names = [safe_name(f"{Path(row['name']).stem}-{i + 1}{extension}")
                         for i in range(len(files))]
                for name, data in zip(names, files):
                    (stage / name).write_bytes(data)
                publish(stage, output)
            paths = [str(output / name) for name in names]
        return {"converted": paths}

    @staticmethod
    def write_bytes(output, data):
        with tempfile.TemporaryDirectory(prefix=".peare-", dir=output.parent) as tmp:
            staged = Path(tmp) / "payload"
            staged.write_bytes(data)
            publish(staged, output)

    def copy(self, resource, row, path):
        digest = hashlib.sha256()
        with path.open("xb") as stream:
            for block in self.native.chunks(resource, row["size"]):
                stream.write(block)
                digest.update(block)
        return digest.hexdigest()

    def extract(self, args):
        location, auth = self.locate(args)
        output = unused(output_path(args["output"]))
        with tempfile.TemporaryDirectory(prefix=".peare-", dir=output.parent) as tmp:
            staged = Path(tmp) / "result"
            if args.get("tree"):
                directory = self.browser.directory(location, auth)
                staged.mkdir()
                count = self.copy_tree(directory, auth, staged)
                result = {"files": count}
            else:
                with self.browser.payload_entry(location, auth) as (_, row, resource):
                    digest = self.copy(resource, row, staged)
                    result = {"size": row["size"], "sha256": digest}
            publish(staged, output)
        return {"extracted": str(output), **result}

    def copy_tree(self, location, auth, output, depth=0):
        if depth >= 64:
            raise PeareError("nesting_limit", "Export a smaller subtree.")
        _, rows = self.browser.level(location, auth)
        names = set()
        count = 0
        with self.browser.open(location, auth) as session:
            for row in rows:
                name = safe_name(row["name"])
                if name.casefold() in names:
                    raise PeareError("duplicate_export_name", f"Export this entry by reference: {name}")
                names.add(name.casefold())
                target = output / name
                if row["kind"] == "folder":
                    target.mkdir()
                    child = self.browser.directory(
                        self.browser.references[self.browser.public(location, row)["ref"]], auth)
                    count += self.copy_tree(child, auth, target, depth + 1)
                else:
                    with self.native.resource(session, row["key"]) as resource:
                        self.copy(resource, row, target)
                    count += 1
        return count

    def font(self, args):
        with self.source(args) as (payload, row, _, _):
            lib = self.native.decoder
            platform, typ = row.get("platform"), row.get("type")
            mode = args.get("mode", "render")
            codepage = args.get("codepage", 0)
            if mode == "faces" or "face" in args:
                faces = self.native.array(lib.peare_font_get_collection_faces,
                                          lib.peare_blob_array_free, payload, len(payload))
                if mode == "faces":
                    offset = args.get("offset", 0)
                    end = min(offset + args.get("limit", 25), len(faces))
                    return {"faces": [
                        {"index": i, "name": decoder_info(lib, faces[i], None, None).get("FONT_FACE_NAME")}
                        for i in range(offset, end)], "total": len(faces),
                        "next_offset": end if end < len(faces) else None}
                index = args["face"]
                if index >= len(faces):
                    raise PeareError("face_out_of_range", f"This collection has {len(faces)} faces.")
                payload = faces[index]
                platform = typ = None
            hints = payload, len(payload), utf8(platform), utf8(typ)
            if mode == "characters":
                cp = Codepoints()
                try:
                    self.native.check(lib.peare_font_get_codepoints_with_codepage(
                        *hints, codepage, C.byref(cp)), "List font characters")
                    offset = args.get("offset", 0)
                    end = min(offset + args.get("limit", 100), cp.count)
                    return {"characters": [cp.items[i] for i in range(offset, end)],
                            "glyph_indices": bool(cp.glyph_index_mode), "total": cp.count,
                            "next_offset": end if end < cp.count else None}
                finally:
                    lib.peare_font_codepoint_array_free(C.byref(cp))
            style = FontOptions(args.get("scale", 2), 4, 0x101010FF, 0xFFFFFFFF)
            png = Blob()
            try:
                if "glyph_index" in args:
                    status = lib.peare_font_render_glyph_index_with_codepage(
                        *hints, codepage, args["glyph_index"], C.byref(style), C.byref(png))
                else:
                    status = lib.peare_font_render_with_codepage(
                        *hints, codepage,
                        utf8(args.get("text", "The quick brown fox jumps over the lazy dog.")),
                        C.byref(style), C.byref(png))
                self.native.check(status, "Render font")
                if png.length > 4 * 1024 * 1024:
                    raise PeareError("image_too_large", "Use a smaller scale or shorter text.")
                return {"name": row["name"]}, [image_block(png.read())]
            finally:
                lib.peare_blob_free(C.byref(png))
