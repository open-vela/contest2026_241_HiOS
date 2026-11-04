#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare an ESP-SR-local archive; never modify the component cache."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

TABLES = {"sigmoid_table_5_8": 5680, "tanh_table_4_8": 3018,
          "tanh_table_3_5": 256, "sigmoid_table_4_4": 256}

def sections(tool, archive, prefix, readonly=False):
    lines = subprocess.check_output(tool + ["-h", str(archive)], text=True).splitlines()
    found = {}
    for i, line in enumerate(lines):
        fields = line.split()
        if len(fields) >= 3 and fields[1] in {prefix + n for n in TABLES}:
            name = fields[1][len(prefix):]
            if readonly and (i+1 >= len(lines) or "READONLY" not in lines[i+1]):
                raise RuntimeError("lookup table is still writable: " + name)
            found[name] = int(fields[2], 16)
    if found != TABLES:
        raise RuntimeError("unsupported ESP-SR lookup table layout: " + repr(found))

def main():
    a = argparse.ArgumentParser()
    a.add_argument("--source", required=True)
    a.add_argument("--output", required=True)
    a.add_argument("--objcopy", required=True)
    args = a.parse_args()
    source = Path(args.source).resolve(strict=True)
    output = Path(args.output).absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    objcopy = shlex.split(args.objcopy)
    objdump = objcopy[:-1] + [objcopy[-1].replace("objcopy", "objdump")]
    st = source.stat()
    identity = {"source": str(source), "size": st.st_size, "mtime": st.st_mtime_ns,
                "script": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "tool": objcopy}
    stamp = output.with_suffix(".json")
    with output.with_suffix(".lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if output.is_file() and stamp.is_file():
            try:
                if json.loads(stamp.read_text())["identity"] == identity:
                    return
            except (ValueError, KeyError):
                pass
        sections(objdump, source, ".data.")
        fd, name = tempfile.mkstemp(prefix=".sr-lib-", suffix=".a", dir=output.parent)
        os.close(fd)
        temporary = Path(name)
        try:
            cmd = objcopy[:]
            for table in TABLES:
                cmd += ["--rename-section", ".data." + table + "=.rodata." + table +
                        ",alloc,load,readonly,data,contents"]
            subprocess.run(cmd + [str(source), str(temporary)], check=True)
            sections(objdump, temporary, ".rodata.", readonly=True)
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            os.replace(temporary, output)
            stamp.write_text(json.dumps({"identity": identity, "source_sha256": digest}, indent=2))
        finally:
            temporary.unlink(missing_ok=True)

if __name__ == "__main__":
    main()
