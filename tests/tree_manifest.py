#!/usr/bin/env python3
"""Print a stable path/type/content-hash manifest without following symlinks."""
import hashlib
import json
import os
import stat
import sys


root = os.path.abspath(sys.argv[1])
entries = []
for current, directories, files in os.walk(root, topdown=True, followlinks=False):
    directories.sort()
    files.sort()
    for name in list(directories):
        path = os.path.join(current, name)
        if os.path.islink(path):
            entries.append({"path": os.path.relpath(path, root), "type": "symlink", "target": os.readlink(path)})
            directories.remove(name)
    for name in files:
        path = os.path.join(current, name)
        relative = os.path.relpath(path, root)
        info = os.lstat(path)
        if stat.S_ISLNK(info.st_mode):
            entries.append({"path": relative, "type": "symlink", "target": os.readlink(path)})
        elif stat.S_ISREG(info.st_mode):
            digest = hashlib.sha256()
            with open(path, "rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            entries.append({"path": relative, "type": "file", "sha256": digest.hexdigest()})
        else:
            entries.append({"path": relative, "type": "other"})
entries.sort(key=lambda entry: entry["path"])
print(json.dumps(entries, sort_keys=True, separators=(",", ":")))
