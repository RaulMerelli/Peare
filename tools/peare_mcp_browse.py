"""Browse Peare containers by path or by a reference returned by a tool."""

import fnmatch
from collections import OrderedDict, deque
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass, replace
from pathlib import Path
from uuid import uuid4

from peare_mcp_native import PeareError


def input_file(value):
    if not value or not Path(value).is_absolute():
        raise PeareError("absolute_path_required", "Supply an absolute local file path.")
    path = Path(value).resolve(strict=True)
    if not path.is_file():
        raise PeareError("not_a_file", f"Expected a regular file: {path}")
    return path


def output_path(value):
    if not value or not Path(value).is_absolute():
        raise PeareError("absolute_path_required", "Supply an absolute local output path.")
    path = Path(value)
    if path.name in ("", ".", "..") or ":" in path.name:
        raise PeareError("invalid_output", "Supply a new file or directory name.")
    return path.parent.resolve(strict=True) / path.name


def internal_path(value):
    value = value.replace("\\", "/").rstrip("/")
    if not value or any(part in ("", ".", "..") for part in value.split("/")) or "\0" in value:
        raise PeareError("invalid_entry_path", "Use a path relative to the container, with '/' separators.")
    return value


@dataclass(frozen=True)
class Location:
    file: Path
    parts: tuple
    identity: tuple
    hops: tuple = ()
    prefix: str = ""
    entry: tuple = ()


class Browser:
    def __init__(self, native):
        self.native = native
        self.references = OrderedDict()
        self.credentials = OrderedDict()

    def remember(self, location):
        ref = uuid4().hex
        self.references[ref] = location
        if len(self.references) > 8192:
            self.references.popitem(last=False)
        return ref

    def identities(self, paths):
        return tuple(self.native.file_identity(path.stat()) for path in paths)

    def root(self, path, parts=()):
        file = input_file(path)
        files = tuple(input_file(part) for part in parts)
        if files and (files[0] != file or len(set(files)) != len(files)):
            raise PeareError("invalid_parts", "List each part once, starting with the file being opened.")
        return Location(file, files, self.identities(files or (file,)))

    def select(self, args):
        ref = args.get("ref")
        if ref:
            if args.get("path") or args.get("parts"):
                raise PeareError("invalid_selection", "Use either a reference or a file path with optional parts.")
            location = self.references.get(ref)
            if not location:
                raise PeareError("expired_reference", "Open the file again to obtain a fresh reference.")
            self.references.move_to_end(ref)
        else:
            location = self.root(args.get("path"), args.get("parts", ()))
        if self.identities(location.parts or (location.file,)) != location.identity:
            raise PeareError("source_changed", "The file changed. Open it again before using this reference.")
        supplied = self.auth(args)
        if supplied:
            self.credentials[(location.file, location.parts, location.identity)] = supplied
            self.credentials.move_to_end((location.file, location.parts, location.identity))
            if len(self.credentials) > 32:
                self.credentials.popitem(last=False)
        return location, self.credentials.get((location.file, location.parts, location.identity), {})

    @staticmethod
    def auth(args):
        result = {key: args[key] for key in ("password", "key_passphrase") if key in args}
        if "private_key_file" in args:
            key = input_file(args["private_key_file"])
            if key.stat().st_size > 1024 * 1024:
                raise PeareError("key_too_large", "The private key file exceeds 1 MiB.")
            result["private_key"] = key.read_bytes()
        return result

    @contextmanager
    def open(self, location, auth):
        with ExitStack() as stack:
            session = stack.enter_context(self.native.session(
                location.file, parts=location.parts, **auth))
            for hop in location.hops:
                handle = stack.enter_context(self.native.resource(session, hop))
                session = stack.enter_context(self.native.session(parent_resource=handle, **auth))
            yield session

    @staticmethod
    def child(location, row):
        return replace(location, prefix="", entry=(), hops=location.hops + (row["key"],))

    def entry_row(self, session, key):
        return self.native.row(session, key)

    def directory(self, location, auth):
        if not location.entry:
            return location
        with self.open(location, auth) as session:
            row = self.entry_row(session, location.entry)
        if row["browse"]:
            return self.child(location, row)
        if row["kind"] == "folder":
            return replace(location, prefix=row["path"], entry=())
        raise PeareError("not_a_container", "This entry is a file. Read it or extract its payload.")

    def locate(self, location, auth, inside, *, case_insensitive=False):
        if not inside:
            return location
        location = self.directory(location, auth)
        desired = "/".join(filter(None, (location.prefix, internal_path(inside))))
        compare = str.casefold if case_insensitive else str
        for _ in range(32):
            with self.open(location, auth) as session:
                rows = list(self.native.rows(session))
            needle = compare(desired)
            matches = [row for row in rows if compare(row["path"]) == needle]
            if case_insensitive:
                exact = [row for row in matches if row["path"] == desired]
                if exact:
                    matches = exact
            if len(matches) == 1:
                return replace(location, entry=matches[0]["key"], prefix="")
            if matches:
                raise PeareError("ambiguous_entry", "Several entries share this path. Use an entry reference.",
                                 entries=[self.public(location, row) for row in matches[:20]])
            if any(compare(row["path"]).startswith(needle + "/") for row in rows):
                return replace(location, prefix=desired)
            parents = [row for row in rows if row["browse"] and
                       needle.startswith(compare(row["path"]) + "/")]
            if not parents:
                raise PeareError("not_found", f"No entry matches: {inside}")
            parents.sort(key=lambda row: len(row["path"]), reverse=True)
            if len(parents) > 1 and len(parents[0]["path"]) == len(parents[1]["path"]):
                raise PeareError("ambiguous_entry", "Several nested containers match. Browse by reference.",
                                 entries=[self.public(location, row) for row in parents[:20]])
            parent = parents[0]
            desired = desired[len(parent["path"]) + 1:]
            location = self.child(location, parent)
        raise PeareError("nesting_limit", "Browse this path in smaller steps.")

    def public(self, location, row):
        if row["key"] is None:
            target = replace(location, prefix=row["path"], entry=())
        else:
            target = replace(location, prefix="", entry=row["key"])
        return {**{key: row[key] for key in (
            "name", "path", "kind", "type", "size", "offset", "language", "platform", "identifier")
            if row.get(key) is not None}, "ref": self.remember(target)}

    def level(self, location, auth):
        prefix = location.prefix + "/" if location.prefix else ""
        specific = []
        synthetic = {}
        with self.open(location, auth) as session:
            metadata = self.native.description(session)
            for row in self.native.rows(session):
                if not row["path"].startswith(prefix):
                    continue
                relative = row["path"][len(prefix):]
                name, separator, _ = relative.partition("/")
                if not name:
                    continue
                if separator:
                    synthetic.setdefault(name, {
                        "key": None, "path": prefix + name, "name": name, "kind": "folder",
                        "browse": False,
                    })
                else:
                    specific.append(row)
        for row in specific:
            if row["kind"] == "folder":
                synthetic.pop(row["name"], None)
        rows = specific + list(synthetic.values())
        rows.sort(key=lambda row: (row["kind"] == "file", row["name"].casefold(),
                                   row["name"], row["key"] or ()))
        return metadata, rows

    def browse(self, args):
        location, auth = self.select(args)
        location = self.locate(location, auth, args.get("inside"))
        directory = self.directory(location, auth)
        metadata, rows = self.level(directory, auth)
        offset = args.get("offset", 0)
        limit = args.get("limit", 60)
        return {**metadata, "source": str(directory.file), "folder": directory.prefix,
                "ref": self.remember(directory), "total": len(rows),
                "entries": [self.public(directory, row) for row in rows[offset:offset + limit]],
                "next_offset": offset + limit if offset + limit < len(rows) else None}

    def find(self, args):
        location, auth = self.select(args)
        location = self.locate(location, auth, args.get("inside"))
        pending = deque([(self.directory(location, auth), "", 0)])
        pattern = args["name"].casefold()
        limit = args.get("limit", 30)
        max_folders = args.get("max_folders", 250)
        max_depth = args.get("depth", 16)
        matches, errors = [], []
        visited = 0
        truncated = False
        while pending and visited < max_folders and len(matches) < limit:
            current, path, depth = pending.popleft()
            visited += 1
            try:
                _, rows = self.level(current, auth)
            except PeareError as error:
                if visited == 1:
                    raise
                errors.append({"path": path, **error.details})
                continue
            for row in rows:
                full_path = "/".join(filter(None, (path, row["name"])))
                if any(fnmatch.fnmatchcase(name.casefold(), pattern)
                       for name in {row["name"], row.get("identifier", row["name"])}):
                    if len(matches) == limit:
                        truncated = True
                        break
                    matches.append({**self.public(current, row), "matched_path": full_path})
                descend = row["kind"] == "folder" or (row["browse"] and (
                    args.get("nested", False) or row["type"] == "DISK_PARTITION"))
                if descend and depth < max_depth:
                    child = self.child(current, row) if row["browse"] else replace(
                        current, prefix=row["path"], entry=())
                    pending.append((child, full_path, depth + 1))
                elif descend:
                    truncated = True
        return {"matches": matches, "visited_folders": visited,
                "complete": not (truncated or pending or errors),
                "failed_folders": len(errors), "errors": errors[:12]}

    @contextmanager
    def payload_entry(self, location, auth):
        if not location.entry:
            raise PeareError("entry_required", "Select a file with an entry reference or its internal path.")
        with self.open(location, auth) as session:
            row = self.entry_row(session, location.entry)
            if row["kind"] == "folder":
                raise PeareError("not_a_payload", "Browse the folder or export its contents.")
            with self.native.resource(session, row["key"]) as handle:
                yield session, row, handle
