#!/usr/bin/env python3
"""
upload_track.py - Flash a tracker module (S3M/XM/etc.) into a raw data partition
on the ESP32, by partition *label* (not a hard-coded offset).

Usage:
    python3 upload_track.py --label xm_music  --file "space shell.s3m" [--port /dev/ttyACM0] [--baud 460800]
    python3 upload_track.py --label xm_music2 --file "im_back.xm"

The partition offset/size are read from the project's partitions.csv (auto-placed
offsets are accumulated), so it stays correct if you resize partitions.

If --port is omitted it auto-detects the first /dev/ttyACM* or /dev/ttyUSB* device.
"""
import os
import sys
import glob
import argparse

try:
    import serial  # noqa: F401
except ImportError:
    pass


def find_esptool():
    """Locate esptool.py (prefer the one bundled with PlatformIO)."""
    home = os.path.expanduser("~")
    candidates = [
        os.path.join(home, ".platformio", "packages", "tool-esptoolpy", "esptool.py"),
        os.path.join(os.environ.get("PLATFORMIO_CORE_DIR", ""), "packages",
                     "tool-esptoolpy", "esptool.py"),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return "esptool.py"  # last resort: on PATH


def find_interp_with_serial():
    """Return a python interpreter that has pyserial importable."""
    try:
        import serial  # noqa: F401
        return sys.executable
    except ImportError:
        pass
    for cand in ("python3", "/usr/bin/python3",
                "/home/tvmini/miniconda/bin/python3"):
        if not cand:
            continue
        if os.system("%s -c 'import serial' >/dev/null 2>&1" % cand) == 0:
            return cand
    return sys.executable


def partition_offset(csv_path, label):
    """Return (offset, size) of `label` from partitions.csv. Auto-placed offsets
    (blank column) are accumulated from preceding partitions."""
    if not os.path.isfile(csv_path):
        print("[upload_track] partitions.csv not found at %s" % csv_path)
        return None, None
    offset = 0
    with open(csv_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            row = [c.strip() for c in line.split(",")]
            if row and row[-1] == "":
                row.pop()
            if len(row) < 4:
                continue
            name, ptype, subtype, poffset = row[0], row[1], row[2], row[3]
            size = row[4] if len(row) > 4 else "0"
            if poffset:
                offset = int(poffset, 0)
            size_int = int(size, 0) if size else 0
            if name == label:
                return offset, size_int
            offset += size_int
    return None, None


def auto_port():
    for pat in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        devs = sorted(glob.glob(pat))
        if devs:
            return devs[0]
    return None


def main():
    ap = argparse.ArgumentParser(description="Flash a tracker file to a partition by label.")
    ap.add_argument("--label", required=True,
                   help="Partition label, e.g. xm_music or xm_music2")
    ap.add_argument("--file", required=True,
                   help="Path to the tracker file (S3M/XM/IT/MOD)")
    ap.add_argument("--port", default=None, help="Serial port (auto-detected if omitted)")
    ap.add_argument("--baud", default="460800", help="Upload baud rate")
    ap.add_argument("--csv", default=None,
                   help="Path to partitions.csv (defaults to ../partitions.csv)")
    ap.add_argument("--project-dir", default=None,
                   help="Project dir containing partitions.csv (defaults to script dir)")
    args = ap.parse_args()

    project_dir = args.project_dir or os.path.dirname(os.path.abspath(__file__))
    csv_path = args.csv or os.path.join(project_dir, "partitions.csv")

    offset, size = partition_offset(csv_path, args.label)
    if offset is None:
        print("[upload_track] partition '%s' not found in %s" % (args.label, csv_path))
        sys.exit(1)

    track_path = os.path.abspath(args.file)
    if not os.path.isfile(track_path):
        print("[upload_track] track file '%s' not found" % track_path)
        sys.exit(1)

    fsize = os.path.getsize(track_path)
    if size and fsize > size:
        print("[upload_track] WARNING: track (%d bytes) larger than partition '%s' "
              "(%d). Increase its Size in partitions.csv or the flash will overflow."
              % (fsize, args.label, size))

    port = args.port or auto_port()
    if not port:
        print("[upload_track] no serial port found; pass --port /dev/ttyACM0")
        sys.exit(1)

    interp = find_interp_with_serial()
    esptool = find_esptool()
    cmd = '%s "%s" --port %s --baud %s --before default_reset --after no_reset write_flash 0x%x "%s"' % (
        interp, esptool, port, args.baud, offset, os.path.abspath(track_path))
    print("[upload_track] %s" % cmd)
    rc = os.system(cmd)
    sys.exit(rc >> 8 if rc > 255 else rc)


if __name__ == "__main__":
    main()
