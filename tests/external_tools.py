"""Resolve optional local oracles without making CI results host-dependent."""

import os
import shutil


def enabled():
    return os.environ.get("PEARE_EXTERNAL_ORACLES", "1").lower() not in {
        "0", "false", "no"}


def find(*names):
    if not enabled():
        return None
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return None
