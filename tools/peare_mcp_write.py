"""Stage, reopen, and publish Writer output through the public ABIs."""

import ctypes as C
import errno
import hashlib
import os
import shutil
import tempfile
from dataclasses import replace
from pathlib import Path

from peare_agent import WriterInput, WriterOptions, writer_formats
from peare_mcp_browse import input_file, internal_path, output_path
from peare_mcp_native import HANDLE, U64, PeareError, VISIT_FILE, VISIT_TARGET, utf8


OPERATIONS = (
    "add", "create_empty_file", "create_directory", "replace", "rename", "delete",
)
COMPRESSED_TAR = {"GZIP", "BZIP2", "ZSTD", "UNIX_COMPRESS", "XZ", "LZMA"}


def transparent_wrapper(parent_format, row, child_format):
    if parent_format == "DEB":
        name = row["name"].casefold()
        return row["type"] == "DEB_FILE" and (name == "data.tar" or name.startswith("data.tar."))
    return parent_format in COMPRESSED_TAR and child_format == "TAR"


def publish(staged, output):
    if staged.is_file() and os.name != "nt":
        os.link(staged, output)
    elif os.name == "nt":
        os.rename(staged, output)
    else:
        libc = C.CDLL(None, use_errno=True)
        rename = getattr(libc, "renameat2", None)
        if not rename:
            raise PeareError("publication_unsupported",
                             "This system cannot publish a directory without replacing it.")
        rename.argtypes = [C.c_int, C.c_char_p, C.c_int, C.c_char_p, C.c_uint]
        rename.restype = C.c_int
        if rename(-100, os.fsencode(staged), -100, os.fsencode(output), 1):
            status = C.get_errno() or errno.EIO
            raise OSError(status, os.strerror(status), str(output))


def unused(path):
    if os.path.lexists(path):
        raise PeareError("output_exists", f"Choose a new output path: {path}")
    return path


def default_output(source, directory):
    if directory:
        return source.with_name(source.name + ".edited")
    return source.with_name(source.stem + ".edited" + source.suffix)


class Writer:
    def __init__(self, native, browser):
        self.native = native
        self.browser = browser

    def check(self, status, action, handle=None):
        if not status:
            return
        lib = self.native.writer
        message = lib.peare_writer_status_message(status).decode("utf-8", "replace")
        detail = lib.peare_writer_last_error(handle) if handle else None
        if detail:
            message += ": " + detail.decode("utf-8", "replace")
        raise PeareError("writer_error", f"{action}: {message}", status=status)

    def files(self, source, password):
        parts = []

        @VISIT_FILE
        def visit(_, value):
            parts.append(Path(value.decode("utf-8")))
            return 1

        lib = self.native.writer
        self.check(lib.peare_writer_visit_update_files(
            utf8(source), utf8(password), visit, None), "Discover writable source files")
        for part in parts:
            if part.parent != source.parent or not part.is_file():
                raise PeareError("invalid_companion", "Companion files must be sibling files.")
        if not parts or parts[0] != source or len(set(parts)) != len(parts):
            raise PeareError("invalid_source_set", "The Writer returned an invalid file set.")
        return parts

    def targets(self, source):
        results = []

        @VISIT_TARGET
        def visit(_, index, name, mask):
            results.append({
                "index": index, "name": name.decode("utf-8", "replace"),
                "operations": [operation for bit, operation in enumerate(OPERATIONS)
                               if mask & (1 << bit)],
            })
            return 1

        lib = self.native.writer
        self.check(lib.peare_writer_visit_update_targets(
            utf8(source), visit, None), "Discover update targets")
        for target in results:
            offset = U64()
            status = lib.peare_writer_get_update_target_disk_offset(
                utf8(source), target["index"], C.byref(offset))
            if status != 10:
                self.check(status, "Read target disk offset")
                target["disk_offset"] = offset.value
        return results

    def formats(self, args):
        data = writer_formats(self.native.writer, args.get("format"))
        offset = args.get("offset", 0)
        limit = args.get("limit", 25)
        return {"formats": data[offset:offset + limit], "total": len(data),
                "next_offset": offset + limit if offset + limit < len(data) else None}

    def capabilities(self, args):
        location, auth = self.browser.select(args)
        location = self.browser.locate(location, auth, args.get("inside"))
        targets = self.targets(location.file)
        with self.browser.open(location, auth) as session:
            metadata = self.native.description(session)
        response = {**metadata, "source": str(location.file), "update_targets": targets}
        if location.entry:
            try:
                response["selected_entry"] = self.entry_path(location, auth)
            except PeareError as error:
                response["selected_entry_limit"] = error.details
        return response

    def entry_path(self, location, auth):
        if not location.entry and not location.prefix:
            raise PeareError("entry_required", "Select an entry from Browse or Find.")
        root = replace(location, hops=(), entry=(), prefix="")
        names = []
        disk_offset = None
        root_path = None
        part_ref = None
        for hop in location.hops:
            with self.browser.open(root, auth) as session:
                row = self.browser.entry_row(session, hop)
                parent_format = self.native.description(session)["format"]
            child = self.browser.child(root, row)
            if row["type"] == "DISK_PARTITION" and not root.hops:
                disk_offset = row["offset"]
                root_path = row["path"]
                part_ref = self.browser.public(root, row)["ref"]
            elif row["kind"] == "folder":
                names.append(row["path"])
            else:
                with self.browser.open(child, auth) as session:
                    child_format = self.native.description(session)["format"]
                if not transparent_wrapper(parent_format, row, child_format) or names or part_ref:
                    raise PeareError("nested_write_unsupported",
                                     "This entry is inside an embedded container. Export it and edit that copy.")
            root = child
        if location.entry:
            with self.browser.open(location, auth) as session:
                row = self.browser.entry_row(session, location.entry)
            if row["type"] == "DISK_PARTITION":
                raise PeareError("partition_is_not_an_entry", "Select a file or folder inside the partition.")
            names.append(row.get("archive_path", row["path"]).rstrip("/"))
            kind, entry_id = row["kind"], row["entry_id"]
        else:
            names.append(location.prefix)
            kind, entry_id = "folder", -1
        return {"entry": internal_path("/".join(names)), "disk_offset": disk_offset,
                "partition": root_path, "partition_ref": part_ref, "kind": kind,
                "entry_id": entry_id}

    def create(self, args):
        output = unused(output_path(args["output"]))
        options = WriterOptions(utf8(args["format"]), utf8(args["variant"]),
                                utf8(args.get("filesystem")))
        inputs = args.get("inputs", [])
        rows = (WriterInput * len(inputs))(*(
            WriterInput(utf8(input_file(item["source"])), utf8(internal_path(item["entry"])))
            for item in inputs))
        lib = self.native.writer
        handle = HANDLE()
        with tempfile.TemporaryDirectory(prefix=".peare-", dir=output.parent) as tmp:
            stage = Path(tmp) / "result"
            stage.mkdir()
            entry_name = Path(args.get("container_name", output.name))
            if entry_name.name != str(entry_name) or entry_name.name in ("", ".", ".."):
                raise PeareError("invalid_output", "Use a simple file name for container_name.")
            container = stage / entry_name
            self.check(lib.peare_writer_create(C.byref(handle)), "Create Writer")
            try:
                self.check(lib.peare_writer_add_files_to_ex(
                    handle, utf8(container), C.byref(options), 0, rows, len(inputs)), "Create file", handle)
            finally:
                lib.peare_writer_abort(handle)
                lib.peare_writer_destroy(handle)
            metadata = self.reopen(container)
            if args.get("container_name") or len(list(stage.iterdir())) > 1:
                publish(stage, output)
                result = output / entry_name
            else:
                publish(container, output)
                result = output
        return {**metadata, "created": str(result)}

    def reopen(self, file, auth=None):
        with self.native.session(file, **(auth or {})) as session:
            return self.native.description(session)

    def update(self, args):
        location, auth = self.browser.select(args)
        if len(location.parts) > 1:
            raise PeareError("multipart_write_unsupported",
                             "Use the primary file path for Writer-supported companion discovery.")
        if location.hops or location.entry or location.prefix:
            raise PeareError("root_required", "Select the outer file. Put entry references in changes.")
        source = location.file
        changes = [self.change(location, auth, item) for item in args["changes"]]
        files = self.files(source, auth.get("password"))
        directory = len(files) > 1
        output = unused(output_path(args["output"]) if args.get("output")
                        else default_output(source, directory))
        targets = self.targets(source)
        if not targets:
            raise PeareError("read_only", "Peare Writer reports no writable targets for this file.")
        target = self.target_for(targets, changes, args.get("target_index"))
        before = self.browser.identities(files)
        if before[:1] != location.identity[:1]:
            raise PeareError("source_changed", "The file changed before it could be copied.")
        with tempfile.TemporaryDirectory(prefix=".peare-", dir=output.parent) as tmp:
            stage = Path(tmp) / "result"
            stage.mkdir()
            for file in files:
                shutil.copyfile(file, stage / file.name)
            if self.browser.identities(files) != before:
                raise PeareError("source_changed", "The source files changed while being copied.")
            staged = stage / source.name
            staged_files = self.files(staged, auth.get("password"))
            if [p.name for p in staged_files] != [p.name for p in files]:
                raise PeareError("copy_mismatch", "The staged file set differs from the source.")
            if self.target_for(self.targets(staged), changes, target["index"]) != target:
                raise PeareError("target_changed", "The staged Writer target differs from the original target.")
            for index, change in enumerate(changes):
                if "text" in change:
                    change["source"] = Path(tmp) / f"payload-{index}"
                    change["source"].write_bytes(utf8(change["text"]))
            wad = self.reopen(staged, auth)["format"] == "WAD"
            wad_expected = self.wad_plan(staged, auth, changes) if wad else None
            self.apply(staged, target["index"], changes, auth.get("password"))
            metadata = self.reopen(staged, auth)
            validated = (self.verify_wad(staged, auth, wad_expected) if wad
                         else self.verify(staged, changes, target, auth))
            if self.browser.identities(files) != before:
                raise PeareError("source_changed", "The source files changed before publication.")
            if directory or len(list(stage.iterdir())) > 1:
                publish(stage, output)
                result = output / source.name
            else:
                publish(staged, output)
                result = output
        return {**metadata, "source": str(source), "updated": str(result), "target": target,
                "changes": [{"action": c["action"], "entry": c["entry"]} for c in changes],
                "verified": validated}

    def change(self, root, auth, item):
        change = dict(item)
        ref = change.get("entry_ref")
        if ref:
            if "entry" in change:
                raise PeareError("invalid_entry", "Use either entry or entry_ref.")
            selected, _ = self.browser.select({"ref": ref})
            if selected.file != root.file or selected.identity != root.identity:
                raise PeareError("wrong_source", "The entry reference belongs to another file.")
            change.update(self.entry_path(selected, auth))
        elif "entry" in change:
            change["entry"] = internal_path(change["entry"])
        else:
            raise PeareError("entry_required", "Supply the internal entry path or a reference.")
        action = change["action"]
        if action in ("add", "replace"):
            if ("source" in change) == ("text" in change):
                raise PeareError("content_required", "Supply either a source file or UTF-8 text.")
            if "source" in change:
                change["source"] = input_file(change["source"])
        elif "source" in change or "text" in change:
            raise PeareError("invalid_content", "Only add and replace accept source or text.")
        if action == "rename":
            if not change.get("new_entry"):
                raise PeareError("new_entry_required", "Supply the new internal path.")
            change["new_entry"] = internal_path(change["new_entry"])
        return change

    def target_for(self, targets, changes, index):
        offsets = {c["disk_offset"] for c in changes if c.get("disk_offset") is not None}
        eligible = [target for target in targets if (
            (index is None or target["index"] == index)
            and (not offsets or offsets == {target.get("disk_offset")})
            and all(c["action"] in target["operations"] for c in changes))]
        if len(eligible) != 1:
            raise PeareError("target_selection", "Select one supported Writer target for all changes.",
                             targets=targets)
        return eligible[0]

    def apply(self, path, target, changes, password):
        lib = self.native.writer
        handle = HANDLE()
        self.check(lib.peare_writer_create(C.byref(handle)), "Create Writer")
        try:
            if password is not None:
                self.check(lib.peare_writer_set_password(handle, utf8(password)), "Set password", handle)
            self.check(lib.peare_writer_update_begin_target(handle, utf8(path), target),
                       "Begin update", handle)
            for change in changes:
                self.apply_one(lib, handle, change)
            self.check(lib.peare_writer_update_finish(handle), "Commit staged changes", handle)
        finally:
            lib.peare_writer_abort(handle)
            lib.peare_writer_destroy(handle)

    def apply_one(self, lib, handle, change):
        action = change["action"]
        entry = utf8(change["entry"])
        source = utf8(change.get("source"))
        entry_id = change.get("entry_id", -1)
        if action == "add":
            status = lib.peare_writer_create_file(handle, source, entry)
        elif action == "create_empty_file":
            status = lib.peare_writer_create_empty_file(handle, entry)
        elif action == "create_directory":
            status = lib.peare_writer_create_directory(handle, entry)
        elif action == "replace":
            status = lib.peare_writer_replace_entry_by_id(handle, source, entry, entry_id)
        elif action == "rename":
            status = lib.peare_writer_rename_entry_by_id(
                handle, entry, utf8(change["new_entry"]), entry_id)
        elif action == "delete":
            status = lib.peare_writer_delete_entry_by_id(
                handle, entry, int(change.get("recursive", False)), entry_id)
        self.check(status, f"{action} {change['entry']}", handle)

    def wad_entries(self, path, auth):
        with self.native.session(path, **auth) as session:
            rows = sorted(self.native.rows(session), key=lambda row: row["entry_id"])
            return [(row["entry_id"], row["name"], self.resource_hash(session, row)) for row in rows]

    def wad_plan(self, path, auth, changes):
        entries = self.wad_entries(path, auth)
        for change in changes:
            entry = change["entry"].upper()
            action = change["action"]
            if action == "add":
                entries.append((None, entry, self.source_hash(change["source"])))
            elif action == "create_empty_file":
                entries.append((None, entry, hashlib.sha256(b"").hexdigest()))
            else:
                original_id = change.get("entry_id", -1)
                hits = [i for i, (entry_id, name, _) in enumerate(entries)
                        if (entry_id == original_id if original_id >= 0 else name == entry)]
                if len(hits) != 1:
                    raise PeareError("wad_entry_selection",
                                     "Select exactly one WAD lump by name or entry reference.")
                i = hits[0]
                identity, name, digest = entries[i]
                change["entry_id"] = i
                if action == "delete":
                    del entries[i]
                elif action == "replace":
                    entries[i] = identity, name, self.source_hash(change["source"])
                else:
                    entries[i] = identity, change["new_entry"].upper(), digest
        return [(name, digest) for _, name, digest in entries]

    def verify_wad(self, path, auth, expected):
        actual = [(name, digest) for _, name, digest in self.wad_entries(path, auth)]
        if actual != expected:
            raise PeareError("verify_failed", "The reopened WAD lump order or payloads differ.")
        return {"reopened": True, "lumps_checked": len(actual)}

    @staticmethod
    def source_hash(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    def resource_hash(self, session, row):
        digest = hashlib.sha256()
        with self.native.resource(session, row["key"]) as resource:
            for chunk in self.native.chunks(resource, row["size"]):
                digest.update(chunk)
        return digest.hexdigest()

    def verification_root(self, staged, target, auth):
        root = self.browser.root(str(staged))
        if "disk_offset" in target:
            with self.browser.open(root, auth) as session:
                partitions = [row for row in self.native.rows(session)
                              if row["type"] == "DISK_PARTITION"]
            if partitions:
                matching = [p for p in partitions if p["offset"] == target["disk_offset"]]
                if len(matching) != 1:
                    raise PeareError("verify_failed", "Cannot reopen the updated disk partition.")
                root = self.browser.child(root, matching[0])
            return root
        for _ in range(2):
            with self.browser.open(root, auth) as session:
                fmt = self.native.description(session)["format"]
                if fmt not in COMPRESSED_TAR and fmt != "DEB":
                    break
                rows = [row for row in self.native.rows(session) if row["browse"]]
            wrappers = []
            for row in rows:
                child = self.browser.child(root, row)
                with self.browser.open(child, auth) as session:
                    child_format = self.native.description(session)["format"]
                if transparent_wrapper(fmt, row, child_format):
                    wrappers.append(child)
            if len(wrappers) != 1:
                break
            root = wrappers[0]
        return root

    def verify(self, staged, changes, target, auth):
        root = self.verification_root(staged, target, auth)
        expected = {}
        for change in changes:
            action, entry = change["action"], change["entry"]
            if action == "delete":
                expected[entry] = ("absent", None)
                if change.get("recursive"):
                    for child in list(expected):
                        if child.startswith(entry + "/"):
                            del expected[child]
            elif action == "rename":
                expected[change["new_entry"]] = expected.get(entry, ("present", None))
                expected[entry] = ("absent", None)
                for child in list(expected):
                    if child.startswith(entry + "/"):
                        expected[change["new_entry"] + child[len(entry):]] = expected.pop(child)
            elif action in ("add", "replace"):
                expected[entry] = ("hash", self.source_hash(change["source"]))
            elif action == "create_empty_file":
                expected[entry] = ("hash", hashlib.sha256(b"").hexdigest())
            else:
                expected[entry] = ("present", None)
        for entry, (state, value) in expected.items():
            try:
                found = self.browser.locate(root, auth, entry)
            except PeareError as error:
                if state == "absent" and error.details["code"] == "not_found":
                    continue
                raise
            if state == "absent":
                raise PeareError("verify_failed", f"The entry is still present after the update: {entry}")
            if state == "hash":
                with self.browser.payload_entry(found, auth) as (_, row, resource):
                    digest = hashlib.sha256()
                    for chunk in self.native.chunks(resource, row["size"]):
                        digest.update(chunk)
                if digest.hexdigest() != value:
                    raise PeareError("verify_failed", f"The reopened payload differs: {entry}")
        return {"reopened": True, "paths_checked": len(expected)}
