"""
flash_track.py - PlatformIO extra script.

After `pio run -t upload`, this flashes the bundled tracker module
(`space shell.s3m` by default) into the `xm_music` raw data partition, so the
firmware has music to play on first boot without a manual esptool step.

The partition offset is computed by parsing partitions.csv (auto-placed
offsets are accumulated) rather than hard-coded, so it stays correct if you
resize partitions.

Override the track file with the TRACK_FILE environment variable, e.g.:
    TRACK_FILE=other.s3m pio run -t upload
"""
import os
import csv

Import("env")

PROJECT_DIR = env.get("PROJECT_DIR", "")
XM_PART_LABEL = "xm_music"
TRACK_FILE = os.environ.get("TRACK_FILE", "space shell.s3m")
DEFAULT_PORT = env.get("UPLOAD_PORT", "")
UPLOAD_SPEED = env.get("UPLOAD_SPEED", "460800")


def partition_offset():
    """Return (offset, size) of the xm_music partition from partitions.csv."""
    csv_path = os.path.join(PROJECT_DIR, "partitions.csv")
    if not os.path.isfile(csv_path):
        print("[flash_track] partitions.csv not found at %s" % csv_path)
        return None, None
    offset = 0
    with open(csv_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            # Keep empty fields so fixed column positions stay aligned
            # (an auto-placed offset is a blank between two commas). Only drop
            # a single trailing empty caused by a terminating comma.
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
            if name == XM_PART_LABEL:
                return offset, size_int
            offset += size_int
    return None, None


def flash_track(*args, **kwargs):
    offset, size = partition_offset()
    if offset is None:
        print("[flash_track] xm_music partition not found; skipping track flash.")
        return
    track_path = os.path.join(PROJECT_DIR, TRACK_FILE)
    if not os.path.isfile(track_path):
        print("[flash_track] track file '%s' not found; skipping." % TRACK_FILE)
        return
    fsize = os.path.getsize(track_path)
    if size and fsize > size:
        print("[flash_track] WARNING: track (%d bytes) larger than partition (%d); "
              "increase xm_music Size in partitions.csv." % (fsize, size))
    port = env.GetProjectOption("upload_port", "") or DEFAULT_PORT
    if not port:
        print("[flash_track] no upload port; set upload_port or UPLOAD_PORT. "
              "Skipping track flash.")
        return
    # esptool needs pyserial. Use the interpreter that actually has it rather
    # than whatever python runs this SCons script (which may lack 'serial').
    import sys
    interp = sys.executable
    try:
        import serial  # noqa: F401
    except ImportError:
        for cand in ("python3", "/usr/bin/python3",
                     "/home/tvmini/miniconda/bin/python3"):
            if not cand:
                continue
            rc = os.system("%s -c 'import serial' >/dev/null 2>&1" % cand)
            if rc == 0:
                interp = cand
                break
    esptool = os.path.join(env.get("PACKAGES_DIR", ""),
                            "tool-esptoolpy", "esptool.py")
    if not os.path.isfile(esptool):
        # PACKAGES_DIR may be unset in this SCons env; try the known location.
        home = os.path.expanduser("~")
        cand = os.path.join(home, ".platformio", "packages",
                            "tool-esptoolpy", "esptool.py")
        if os.path.isfile(cand):
            esptool = cand
    if not os.path.isfile(esptool):
        esptool = "esptool.py"  # fall back to one on PATH
    cmd = "%s %s --port %s --baud %s write_flash 0x%x \"%s\"" % (
        interp, esptool, port, UPLOAD_SPEED, offset, track_path)
    print("[flash_track] %s" % cmd)
    env.Execute(cmd)


# Flash the track right after the main firmware upload.
env.AddPostAction("upload", flash_track)
# Also expose a standalone target: `pio run -t track`
env.AddCustomTarget("track", None, flash_track, "Flash tracker module to xm_music partition")
