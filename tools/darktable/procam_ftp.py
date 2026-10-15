#!/usr/bin/env python3
"""Fetches Pro Cam's photos from the 3DS over FTP for the darktable import (procam_import.lua).

It downloads the shots into the photo library and deletes them on the 3DS once darktable has them.
It needs only the Python standard library. It also runs on its own:

    python procam_ftp.py fetch  --host <3ds-ip> --dest D:\\Photos [--layout "{Y}\\{Y}-{m}-{d}"] [--verify EXE]
    python procam_ftp.py delete --host <3ds-ip> IMG_0042.TXT IMG_0042.DNG ...
    python procam_ftp.py list   --host <3ds-ip>

Shots in the photo folder, names in upper case:
    IMG_nnnn.DNG + .JPG + .TXT   a photo, a merge or an HDR shot (JPG only or RAW only lack one of them)
    IMG_nnnn.PNG + .YUV + .TXT   a photo without raw capture
    IMG_nnnn/                    a bracket or a 3D pair: one picture per frame and INFO.TXT

fetch takes every IMG_nnnn shot with all its files, root files and folder together. The 3DS's file time
picks the day folder. In darktable a shot is one group: the root DNG leads, else the JPG, else the PNG, else
a 3D pair's SBS.PNG, else the folder's first picture. A shot without a picture is still copied and deleted,
just not shown. Other names stay on the 3DS.

The 3DS's Wi-Fi can be slow and drop out:
- Every FTP step reconnects and retries, with longer pauses each time. A connection without data for
  --timeout seconds counts as stalled.
- A download that makes progress never runs out of retries. Any other step gives up after the pauses.
- Downloads go to <dest>/.3ds-incoming/*.part first and resume where they stopped, also in the next run.
- A finished file must have the size the 3DS listed, and the same CRC-32 when the server offers XCRC.
- When the connection is gone for good, fetch reports ERROR and keeps the partial downloads.
- delete takes the files in the given order. The Lua side puts each shot's main file last, so a half
  deleted shot is still recognised next time. A file that is already gone counts as deleted.

Output, one tab-separated line per event:
    COUNT       shots  bytes                       shots to fetch and their size
    FILE        index  name                        fetching shot number index, counted from 1
    BYTES       shot done  shot total  run done  run total  bytes/s
    RETRY       step  reason  seconds              connection trouble, trying again after a pause
    SHOT        new|dup  path  raw  a|b|c  x|y     a shot is ready: the file to import ("" when it has no
                                                   picture), 1 for a raw shot, its remote files in delete
                                                   order, the other local pictures of its group
    SKIP        name  reason                       left on the 3DS
    FAIL        name  reason                       left on the 3DS, the next run tries again
    DELETED     name
    NOTDELETED  name  reason
    ERROR       message                            the connection is gone, nothing more follows
"dup" means an identical file is already in the library. The shot is still reported, so that it gets
imported and deleted.
"""
import argparse
import collections
import datetime
import ftplib
import os
import re
import socket
import subprocess
import time
import zlib

FTP_PORT = 5021
FTP_USER = "procam"
FTP_PASSWORD = "darktable"  # the server takes any login
STALL_SECONDS = 30
# Pauses between attempts that made no progress, about four and a half minutes in all.
RETRY_PAUSES = [2, 4, 8, 15, 30, 30, 60, 60, 60]
# Before the first connection, a failure most likely means the FTP server is off: say so soon.
FIRST_CONNECT_TRIES = 3

INCOMING_FOLDER = ".3ds-incoming"
PART_SUFFIX = ".part"
PART_NAME_PIECES = 3  # name, size, file time
DOWNLOAD_BLOCK_BYTES = 64 * 1024
CRC_BLOCK_BYTES = 1024 * 1024
# A CRC mismatch downloads the file again. Several in a row are not the link's fault.
DOWNLOAD_CHECKS = 3
PROGRESS_INTERVAL_SECONDS = 0.5
SPEED_WINDOW_SECONDS = 5
VERIFY_TIMEOUT_SECONDS = 120
# The 3DS numbers shots from IMG_0001 again once its card is empty, so names get a number: IMG_0001_2.
NAME_NUMBER_LIMIT = 1000

# An ls -l line: permissions, links, owner, group, size, month, day, time or year, name.
LIST_COLUMNS = 9
LIST_SIZE_COLUMN = 4
LIST_NAME_COLUMN = 8

MDTM_TIME_FORMAT = "%Y%m%d%H%M%S"
MDTM_TIME_LENGTH = 14

# Windows socket errors that mean the 3DS cannot be reached.
WSA_TIMED_OUT = 10060
WSA_HOST_UNREACHABLE = 10065
WSA_NETWORK_UNREACHABLE = 10051
UNREACHABLE_ERRORS = (WSA_TIMED_OUT, WSA_HOST_UNREACHABLE, WSA_NETWORK_UNREACHABLE)

SHOT_NAME = re.compile(r"^IMG_\d{4,}$")
# A 3D pair's side by side picture leads its folder's group when the shot has no root picture.
SIDE_BY_SIDE_PICTURE = "SBS.PNG"
PICTURE_EXTENSIONS = ("DNG", "JPG", "PNG")

# Worth another try: network errors and timeouts, a connection closed in the middle of a reply, 4xx replies
# and garbled replies.
TRANSIENT_ERRORS = (OSError, EOFError, ftplib.error_temp, ftplib.error_reply, ftplib.error_proto)

# What fetch takes of one shot. downloads: [(remote path, size)]. deletions: remote paths in delete order,
# the folder first and the main file last.
Shot = collections.namedtuple("Shot", "name main downloads deletions is_raw")


def report(*fields):
    """Prints one output line for the Lua side."""
    cleaned = []
    for field in fields:
        text = str(field)
        for separator in ("\t", "\r", "\n"):
            text = text.replace(separator, " ")
        cleaned.append(text)
    print("\t".join(cleaned), flush=True)


class LinkDown(Exception):
    """The 3DS stayed unreachable through every retry."""


def read_features(ftp):
    """The command names of the server's FEAT reply, in upper case."""
    try:
        lines = ftp.sendcmd("FEAT").splitlines()
    except ftplib.error_perm:
        return set()
    # The first and the last line frame the list.
    feature_lines = lines[1:-1]
    return {line.strip().split(" ")[0].upper() for line in feature_lines}


class Link:
    """One FTP control connection that opens again whenever it breaks."""

    def __init__(self, host, port, stall_seconds, pauses):
        self.host = host
        self.port = port
        self.stall_seconds = stall_seconds
        self.pauses = pauses
        self.ftp = None
        self.features = set()
        self.reached = False

    def open(self):
        ftp = ftplib.FTP()
        ftp.connect(self.host, self.port, timeout=self.stall_seconds)
        ftp.login(FTP_USER, FTP_PASSWORD)
        ftp.voidcmd("TYPE I")
        self.features = read_features(ftp)
        self.ftp = ftp
        self.reached = True

    def drop(self):
        if self.ftp:
            try:
                self.ftp.close()
            except OSError:
                pass
        self.ftp = None

    def close(self):
        if self.ftp:
            try:
                self.ftp.quit()
            except (OSError, EOFError, ftplib.Error):
                pass
        self.drop()

    def run(self, step_name, step, progress=None):
        """Runs step(ftp) and reconnects and retries after transient errors. progress() returns a number that
        grows while work gets done (bytes on disk): an attempt that moved it resets the retry count.
        Permanent errors (5xx) go to the caller."""
        failures = 0
        last_progress = progress() if progress else None
        while True:
            try:
                if not self.ftp:
                    self.open()
                return step(self.ftp)
            except TRANSIENT_ERRORS as error:
                self.drop()
                current_progress = progress() if progress else None
                if current_progress is not None and current_progress > last_progress:
                    failures = 0
                    last_progress = current_progress
                allowed_failures = len(self.pauses) if self.reached else FIRST_CONNECT_TRIES
                if failures >= allowed_failures:
                    raise LinkDown("%s: %s" % (step_name, self.describe(error)))
                pause = self.pauses[min(failures, len(self.pauses) - 1)]
                report("RETRY", step_name, self.describe(error), pause)
                time.sleep(pause)
                failures += 1

    def describe(self, error):
        """A short reason for the user."""
        if isinstance(error, socket.timeout):
            return "no data for %g s" % self.stall_seconds
        if isinstance(error, EOFError):
            return "the 3DS closed the connection"
        if isinstance(error, ConnectionRefusedError):
            return "nothing answers on the FTP port: is Pro Cam's FTP server on?"
        if isinstance(error, ConnectionResetError):
            return "connection reset"
        if isinstance(error, OSError) and getattr(error, "winerror", None) in UNREACHABLE_ERRORS:
            return "the 3DS is not reachable (Wi-Fi off or another address?)"
        return str(error) or type(error).__name__


def list_folder(link, folder=""):
    """{name: (size, is folder)} of the photo folder or one of its subfolders."""
    command = "LIST " + folder if folder else "LIST"
    place = folder + "/" if folder else "the 3DS"

    def step(ftp):
        lines = []
        ftp.retrlines(command, lines.append)
        return lines

    entries = {}
    for line in link.run("listing " + place, step):
        columns = line.split(None, LIST_COLUMNS - 1)
        if len(columns) == LIST_COLUMNS:
            size = int(columns[LIST_SIZE_COLUMN])
            is_folder = columns[0].startswith("d")
            entries[columns[LIST_NAME_COLUMN]] = (size, is_folder)
    return entries


def extension_of(name):
    """The extension in upper case without the dot, or "" when there is none."""
    if "." not in name:
        return ""
    return name.rsplit(".", 1)[-1].upper()


def remote_time(link, name):
    """The file's time on the 3DS, or None. The 3DS keeps local time and MDTM hands it out as it is."""
    try:
        reply = link.run("MDTM " + name, lambda ftp: ftp.sendcmd("MDTM " + name))
        stamp = reply.split()[1][:MDTM_TIME_LENGTH]
        return datetime.datetime.strptime(stamp, MDTM_TIME_FORMAT)
    except (ftplib.error_perm, ValueError, IndexError):
        return None


class Progress:
    """BYTES lines, at most twice a second: the current shot's bytes, the whole run's and the speed over the
    last few seconds. Only bytes from the network count for the speed: a resumed part is on disk already."""

    def __init__(self, run_total):
        self.run_total = run_total
        self.run_base = 0  # the bytes of the shots before this one
        self.shot_total = 0
        self.shot_base = 0  # the bytes of this shot's files before the current one
        self.last_shown = 0.0
        self.received = 0
        self.samples = [(time.monotonic(), 0)]

    def start_shot(self, shot_total):
        self.run_base += self.shot_total
        self.shot_total = shot_total
        self.shot_base = 0

    def add_received(self, byte_count):
        self.received += byte_count

    def show(self, done_in_file, force=False):
        now = time.monotonic()
        if not force and now - self.last_shown < PROGRESS_INTERVAL_SECONDS:
            return
        self.last_shown = now
        self.samples.append((now, self.received))
        while len(self.samples) > 2 and now - self.samples[1][0] > SPEED_WINDOW_SECONDS:
            self.samples.pop(0)
        oldest_time, oldest_received = self.samples[0]
        elapsed = now - oldest_time
        rate = (self.received - oldest_received) / elapsed if elapsed > 0 else 0
        shot_done = self.shot_base + done_in_file
        report("BYTES", shot_done, self.shot_total, self.run_base + shot_done, self.run_total, int(rate))


def file_crc32(path):
    crc = 0
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(CRC_BLOCK_BYTES), b""):
            crc = zlib.crc32(block, crc)
    return crc


def read_file(path):
    with open(path, "rb") as source:
        return source.read()


def part_path(incoming, remote_name, size, stamp):
    """The partial download's path. Size and file time are in the name, so that a new IMG_0001 never resumes
    the part of an old one."""
    flat_name = remote_name.replace("/", "__")
    return os.path.join(incoming, "%s.%d.%s%s" % (flat_name, size, stamp, PART_SUFFIX))


def download(link, name, size, part, progress):
    """Downloads the remote file into part, resuming what is there, and checks its size and CRC."""

    def bytes_on_disk():
        if os.path.exists(part):
            return os.path.getsize(part)
        return 0

    def step(ftp):
        if bytes_on_disk() == size:
            return
        # Without REST the server cannot resume: start over.
        resume_at = bytes_on_disk() if "REST" in link.features else 0
        mode = "ab" if resume_at else "wb"
        with open(part, mode) as part_file:

            def write_block(data):
                part_file.write(data)
                progress.add_received(len(data))
                progress.show(part_file.tell())

            resume_offset = resume_at or None
            ftp.retrbinary("RETR " + name, write_block, blocksize=DOWNLOAD_BLOCK_BYTES, rest=resume_offset)

    for _ in range(DOWNLOAD_CHECKS):
        if bytes_on_disk() > size:
            os.remove(part)
        link.run("downloading " + name, step, bytes_on_disk)
        progress.show(bytes_on_disk(), True)
        if bytes_on_disk() != size:
            raise RuntimeError("%s: got %d bytes of %d" % (name, bytes_on_disk(), size))
        if "XCRC" not in link.features:
            return
        reply = link.run("checking " + name, lambda ftp: ftp.sendcmd("XCRC " + name))
        remote_crc = int(reply.split()[1], 16)
        if remote_crc == file_crc32(part):
            return
        report("RETRY", "checking " + name, "CRC differs from the 3DS's, downloading it again", 0)
        os.remove(part)
    raise RuntimeError("%s: the download keeps coming out different from the file on the 3DS" % name)


def decode_problem(verify_tool, path):
    """"" when darktable's raw loader (darktable-rs-identify) reads the file, else what it said."""
    if not verify_tool:
        return ""
    no_window = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        result = subprocess.run([verify_tool, path], capture_output=True, text=True, errors="replace",
                                timeout=VERIFY_TIMEOUT_SECONDS, creationflags=no_window)
    except (OSError, subprocess.TimeoutExpired) as error:
        return "could not run the check: %s" % error
    for line in (result.stdout + result.stderr).splitlines():
        if "ERROR" in line:
            return line.strip()
    if result.returncode != 0:
        return "check exited with %d" % result.returncode
    return ""


def has_same_content(path, data):
    if os.path.getsize(path) != len(data):
        return False
    return read_file(path) == data


def numbered_name(name, number):
    """IMG_0042.DNG for number 1, else IMG_0042_2.DNG, IMG_0042_3.DNG ..."""
    if number == 1:
        return name
    stem, dot, extension = name.rpartition(".")
    return "%s_%d%s%s" % (stem, number, dot, extension)


def place_file(folder, name, data):
    """Where a downloaded file goes: (path, "dup") when an identical copy is already there, else (the first
    free name, "new")."""
    for number in range(1, NAME_NUMBER_LIMIT):
        candidate = os.path.join(folder, numbered_name(name, number))
        if not os.path.exists(candidate):
            return candidate, "new"
        if has_same_content(candidate, data):
            return candidate, "dup"
    raise RuntimeError("no free name for " + name)


def differs(path, data):
    """True when a file is at path and its content is not data."""
    return os.path.exists(path) and not has_same_content(path, data)


def place_folder(day_folder, shot_name, files):
    """The local folder for a shot's folder files: the first one that is free or holds no different files."""
    for number in range(1, NAME_NUMBER_LIMIT):
        folder_name = shot_name if number == 1 else "%s_%d" % (shot_name, number)
        candidate = os.path.join(day_folder, folder_name)
        if not os.path.exists(candidate):
            return candidate
        conflicts = any(differs(os.path.join(candidate, name), data) for name, data in files.items())
        if not conflicts:
            return candidate
    raise RuntimeError("no free folder name for " + shot_name)


def plan_shots(link, entries):
    """One Shot per IMG_nnnn, from its root files and its folder. Other names are reported as SKIP."""
    root_files = {}
    folders = set()
    for name, (_, is_folder) in entries.items():
        if is_folder:
            folders.add(name)
        else:
            stem = name.rsplit(".", 1)[0]
            root_files.setdefault(stem, []).append(name)
    shots = []
    for shot_name in sorted(set(root_files) | folders):
        files = sorted(root_files.get(shot_name, []))
        has_folder = shot_name in folders
        if not SHOT_NAME.match(shot_name):
            skipped = files + ([shot_name + "/"] if has_folder else [])
            for name in skipped:
                report("SKIP", name, "not a Pro Cam shot")
            continue
        by_extension = {extension_of(name): name for name in files}
        main = by_extension.get("DNG") or by_extension.get("JPG") or by_extension.get("PNG")
        inside = list_folder(link, shot_name) if has_folder else {}
        folder_has_files = any(not is_folder for _, is_folder in inside.values())
        if not files and not folder_has_files:
            report("SKIP", shot_name + "/", "empty folder")
            continue
        is_raw = bool(main) and extension_of(main) == "DNG"
        downloads = [(name, entries[name][0]) for name in files]
        for name, (size, is_folder) in sorted(inside.items()):
            if not is_folder:
                downloads.append((shot_name + "/" + name, size))
        deletions = [shot_name + "/"] if has_folder else []
        deletions += [name for name in files if name != main]
        if main:
            deletions.append(main)
        shots.append(Shot(shot_name, main, downloads, deletions, is_raw))
    return shots


def download_shot(link, incoming, shot, stamp, progress):
    """Downloads the shot's files into parts. Returns {remote path: part path}."""
    progress.start_shot(sum(size for _, size in shot.downloads))
    parts = {}
    for remote, size in shot.downloads:
        parts[remote] = part_path(incoming, remote, size, stamp)
        download(link, remote, size, parts[remote], progress)
        progress.shot_base += size
    return parts


def read_parts(parts):
    """{remote path: content}."""
    return {remote: read_file(part) for remote, part in parts.items()}


def day_folder_name(layout, when):
    year = "%04d" % when.year
    month = "%02d" % when.month
    day = "%02d" % when.day
    return layout.format(Y=year, m=month, d=day)


def best_render(pictures):
    """The picture that leads a folder's group: a 3D pair's side by side picture, else the first by name."""
    if SIDE_BY_SIDE_PICTURE in pictures:
        return SIDE_BY_SIDE_PICTURE
    return sorted(pictures)[0]


def remove_files(paths):
    for path in paths:
        os.remove(path)


class LibraryWriter:
    """Writes a shot's files into the library and keeps a list of them."""

    def __init__(self, when):
        self.when = when
        self.written = []

    def write(self, path, data):
        # Through a temporary file, so that a half-written file never has the final name.
        temporary = path + PART_SUFFIX
        with open(temporary, "wb") as output:
            output.write(data)
        os.replace(temporary, path)
        if self.when:
            timestamp = self.when.timestamp()
            os.utime(path, (timestamp, timestamp))
        self.written.append(path)

    def write_unless_same(self, path, data):
        if os.path.exists(path) and has_same_content(path, data):
            return
        self.write(path, data)


def check_written_dngs(verify_tool, written, parts):
    """Every DNG written must decode. Else the shot leaves the library again and stays on the 3DS."""
    for path in written:
        if not path.upper().endswith(".DNG"):
            continue
        problem = decode_problem(verify_tool, path)
        if not problem:
            continue
        remove_files(written)
        # The parts passed the size and CRC checks: the file on the card is like this, so do not resume them.
        remove_files(parts.values())
        raise RuntimeError("darktable could not read %s: %s" % (os.path.basename(path), problem))


def fetch_shot(args, link, incoming, shot, progress):
    """Downloads one shot and puts it in the library. Returns (status, the file to import first or "" when the
    shot has no picture, the other pictures to import into its group)."""
    shot_time = remote_time(link, shot.main or shot.downloads[0][0])
    when = shot_time or datetime.datetime.now()
    stamp = shot_time.strftime(MDTM_TIME_FORMAT) if shot_time else "0"
    parts = download_shot(link, incoming, shot, stamp, progress)
    contents = read_parts(parts)
    day_folder = os.path.join(args.dest, day_folder_name(args.layout, when))
    os.makedirs(day_folder, exist_ok=True)
    writer = LibraryWriter(when)
    root_contents = {}
    folder_contents = {}
    for remote, data in contents.items():
        if "/" in remote:
            folder_contents[remote.split("/", 1)[1]] = data
        else:
            root_contents[remote] = data

    status = "dup"
    lead = None
    pictures = []
    base = os.path.join(day_folder, shot.name)
    if shot.main:
        main_data = contents[shot.main]
        lead, status = place_file(day_folder, shot.main, main_data)
        if status == "new":
            writer.write(lead, main_data)
        # The other root files take the main file's name: IMG_0042_2.JPG next to IMG_0042_2.DNG.
        base = os.path.splitext(lead)[0]
        for remote, data in root_contents.items():
            if remote == shot.main:
                continue
            companion = base + os.path.splitext(remote)[1]
            writer.write_unless_same(companion, data)
            # A RAW+JPG shot's JPG is the in-camera render: it joins the DNG's group.
            if extension_of(remote) == "JPG":
                pictures.append(companion)
    else:
        # Root files without a picture beside them, such as a lone TXT.
        for remote, data in root_contents.items():
            path, file_status = place_file(day_folder, remote, data)
            if file_status == "new":
                writer.write(path, data)

    if folder_contents:
        folder = base if shot.main else place_folder(day_folder, shot.name, folder_contents)
        os.makedirs(folder, exist_ok=True)
        for name, data in folder_contents.items():
            writer.write_unless_same(os.path.join(folder, name), data)
        shown = [name for name in folder_contents if extension_of(name) in PICTURE_EXTENSIONS]
        if not lead and shown:
            leader = best_render(shown)
            lead = os.path.join(folder, leader)
            shown.remove(leader)
        pictures += [os.path.join(folder, name) for name in sorted(shown)]
    if writer.written:
        status = "new"
    check_written_dngs(args.verify, writer.written, parts)
    remove_files(parts.values())
    return status, lead or "", pictures


def remove_stale_parts(incoming, shots):
    """Removes the parts of files that are no longer on the 3DS: they would never be resumed."""
    wanted = set()
    for shot in shots:
        for remote, size in shot.downloads:
            wanted.add((remote.replace("/", "__"), str(size)))
    for file_name in os.listdir(incoming):
        if not file_name.endswith(PART_SUFFIX):
            continue
        pieces = file_name[:-len(PART_SUFFIX)].rsplit(".", PART_NAME_PIECES - 1)
        is_wanted = len(pieces) == PART_NAME_PIECES and (pieces[0], pieces[1]) in wanted
        if not is_wanted:
            os.remove(os.path.join(incoming, file_name))


def count_parts(incoming):
    return len([name for name in os.listdir(incoming) if name.endswith(PART_SUFFIX)])


def make_link(args):
    pauses = RETRY_PAUSES
    if args.pauses:
        pauses = [float(pause) for pause in args.pauses.split(",")]
    return Link(args.host, args.port, args.timeout, pauses)


def fetch(args):
    args.dest = os.path.abspath(args.dest)
    incoming = os.path.join(args.dest, INCOMING_FOLDER)
    os.makedirs(incoming, exist_ok=True)
    link = make_link(args)
    try:
        shots = plan_shots(link, list_folder(link))
        run_total = sum(size for shot in shots for _, size in shot.downloads)
        report("COUNT", len(shots), run_total)
        progress = Progress(run_total)
        for index, shot in enumerate(shots, 1):
            label = shot.main or shot.name + "/"
            report("FILE", index, label)
            try:
                status, lead, pictures = fetch_shot(args, link, incoming, shot, progress)
            except (ftplib.error_perm, OSError, RuntimeError) as error:
                report("FAIL", label, link.describe(error))
                continue
            raw_flag = 1 if shot.is_raw else 0
            report("SHOT", status, lead, raw_flag, "|".join(shot.deletions), "|".join(pictures))
        remove_stale_parts(incoming, shots)
    except LinkDown as error:
        what = "lost the connection to the 3DS" if link.reached else "could not reach the 3DS"
        message = "%s (%s)" % (what, error)
        kept = count_parts(incoming)
        if kept:
            message += "; %d partial download(s) kept, the next import resumes them" % kept
        report("ERROR", message)
    finally:
        link.close()
        # rmdir removes the folder only when no parts are left in it.
        try:
            os.rmdir(incoming)
        except OSError:
            pass


def delete_remote(link, name):
    """A name with a trailing slash is a shot folder: RMD removes it with its files."""
    if name.endswith("/"):
        folder = name[:-1]
        link.run("deleting " + name, lambda ftp: ftp.rmd(folder))
    else:
        link.run("deleting " + name, lambda ftp: ftp.delete(name))


def delete(args):
    link = make_link(args)
    try:
        for index, name in enumerate(args.files):
            try:
                delete_remote(link, name)
                report("DELETED", name)
            except ftplib.error_perm as error:
                # Already gone: an earlier attempt worked and its reply was lost.
                if "No such" in str(error):
                    report("DELETED", name)
                else:
                    report("NOTDELETED", name, link.describe(error))
            except LinkDown as error:
                for rest in args.files[index:]:
                    report("NOTDELETED", rest, "connection lost")
                message = "lost the connection to the 3DS (%s); the next import deletes the rest" % error
                report("ERROR", message)
                return
    finally:
        link.close()


def list_photo_folder(args):
    link = make_link(args)
    try:
        for name, (size, is_folder) in sorted(list_folder(link).items()):
            report("DIR" if is_folder else "FILE", name, size)
        report("FEATURES", " ".join(sorted(link.features)))
    except LinkDown as error:
        report("ERROR", str(error))
    finally:
        link.close()


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("fetch", "delete", "list"):
        command = commands.add_parser(name)
        command.add_argument("--host", required=True)
        command.add_argument("--port", type=int, default=FTP_PORT)
        command.add_argument("--timeout", type=float, default=STALL_SECONDS,
                             help="seconds without data before a retry")
        command.add_argument("--pauses", help="comma-separated waits between attempts that made no progress")
        if name == "fetch":
            command.add_argument("--dest", required=True, help="photo library root")
            command.add_argument("--layout", default=os.path.join("{Y}", "{Y}-{m}-{d}"),
                                 help="day folder below --dest")
            command.add_argument("--verify",
                                 help="darktable-rs-identify.exe: check that every DNG decodes")
        if name == "delete":
            command.add_argument("files", nargs="+")
    return parser.parse_args()


def main():
    args = parse_arguments()
    if args.command == "fetch":
        fetch(args)
    elif args.command == "delete":
        delete(args)
    else:
        list_photo_folder(args)


if __name__ == "__main__":
    main()
