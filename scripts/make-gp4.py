#!/usr/bin/env python3
"""Generate a GP4 project file for a staged PS4 application.

The create-gp4 tool of the SDK only takes a list of files, which does not
work for the directories that hold the Kodi resources.  This walks the
staging directory and writes the file list and the directory tree.
"""
import os
import sys
import datetime

stage = os.path.abspath(sys.argv[1])
out_file = sys.argv[2]
content_id = sys.argv[3]

files = []
dirs = {}


def walk(path, rel=""):
    entries = sorted(os.listdir(path))
    for name in entries:
        full = os.path.join(path, name)
        target = f"{rel}/{name}" if rel else name
        if os.path.isdir(full):
            dirs.setdefault(rel, []).append(name)
            walk(full, target)
        else:
            files.append((target, os.path.relpath(full, stage)))


walk(stage)

stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
lines = [
    '<?xml version="1.0"?>',
    '<psproject xmlns:xsd="http://www.w3.org/2001/XMLSchema" '
    'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" fmt="gp4" version="1000">',
    "\t<volume>",
    "\t\t<volume_type>pkg_ps4_app</volume_type>",
    "\t\t<volume_id>PS4VOLUME</volume_id>",
    f"\t\t<volume_ts>{stamp}</volume_ts>",
    f'\t\t<package content_id="{content_id}" '
    'passcode="00000000000000000000000000000000" storage_type="digital50" app_type="full" />',
    '\t\t<chunk_info chunk_count="1" scenario_count="1">',
    "\t\t\t<chunks>",
    '\t\t\t\t<chunk id="0" layer_no="0" label="Chunk #0" />',
    "\t\t\t</chunks>",
    '\t\t\t<scenarios default_id="0">',
    '\t\t\t\t<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>',
    "\t\t\t</scenarios>",
    "\t\t</chunk_info>",
    "\t</volume>",
    '\t<files img_no="0">',
]
for target, orig in files:
    lines.append(f'\t\t<file targ_path="{target}" orig_path="{orig}" />')
lines.append("\t</files>")
lines.append("\t<rootdir>")


def emit_dir(rel, indent):
    for name in dirs.get(rel, []):
        child = f"{rel}/{name}" if rel else name
        if child in dirs:
            lines.append(f'{indent}<dir targ_name="{name}">')
            emit_dir(child, indent + "\t")
            lines.append(f"{indent}</dir>")
        else:
            lines.append(f'{indent}<dir targ_name="{name}" />')


emit_dir("", "\t")
lines.append("\t</rootdir>")
lines.append("</psproject>")

with open(out_file, "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"{out_file}: {len(files)} files, {len(dirs)} directories")
