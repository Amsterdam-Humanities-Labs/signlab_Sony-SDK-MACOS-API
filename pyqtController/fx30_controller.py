#!/usr/bin/env python3
"""FX30 Multi-Camera Controller — PyQt6 frontend for fx30MultiRecord.

Manages the fx30MultiRecord server process, shows live camera status,
provides capture / format / sync controls, tracks expected clip filenames
per capture, and verifies synced files on the studio drive per date.
"""

import json
import math
import os
import re
import signal
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import date, datetime
from pathlib import Path

from PyQt6.QtCore import QPointF, QRect, QSize, Qt, QThread, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QFontMetrics, QIcon, QPainter, QPen
from PyQt6.QtWidgets import (
    QApplication, QFrame, QGridLayout, QGroupBox, QHBoxLayout,
    QLabel, QMainWindow, QMessageBox, QPushButton, QScrollArea, QSizePolicy,
    QTableWidget, QTableWidgetItem, QTextEdit, QVBoxLayout, QWidget,
)

APP_DIR = Path(__file__).resolve().parent
CONFIG_PATH = APP_DIR / "config.json"
LOG_DIR = APP_DIR / "capture_logs"


def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)


# ---------------------------------------------------------------- HTTP layer

def api_request(base_url, path, method="GET", body=None, timeout=5):
    """The server requires a JSON body on every POST (bare POST -> HTTP 400)."""
    url = base_url + path
    data = None
    headers = {}
    if method == "POST":
        data = json.dumps(body if body is not None else {}).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def studio_page_already_open(url):
    """True if any Chrome tab already shows the studio page (avoids opening a
    duplicate tab on every app start). Never launches Chrome itself."""
    if not subprocess.run(["pgrep", "-x", "Google Chrome"],
                          capture_output=True).stdout:
        return False
    fragment = url.split("/")[-1].split("?")[0] or url
    script = (
        'tell application "Google Chrome"\n'
        '  repeat with w in windows\n'
        '    repeat with t in tabs of w\n'
        f'      if URL of t contains "{fragment}" then return "found"\n'
        '    end repeat\n'
        '  end repeat\n'
        'end tell\n'
        'return "not-found"')
    try:
        out = subprocess.run(["osascript", "-e", script],
                             capture_output=True, text=True, timeout=10)
        return "found" in out.stdout and "not-found" not in out.stdout
    except Exception:
        return False


def count_usb_cameras():
    """FX30s visible on the USB bus, regardless of SDK connection state."""
    try:
        out = subprocess.run(["ioreg", "-p", "IOUSB"],
                             capture_output=True, text=True, timeout=5)
        return out.stdout.count("ILME-FX30")
    except Exception:
        return -1  # unknown


class StatusPoller(QThread):
    """Polls /api/status and the USB bus off the UI thread."""
    status_received = pyqtSignal(dict, int)
    poll_failed = pyqtSignal(str, int)

    def __init__(self, base_url, interval_ms):
        super().__init__()
        self.base_url = base_url
        self.interval_s = interval_ms / 1000.0
        self._running = True

    def run(self):
        while self._running:
            usb = count_usb_cameras()
            try:
                status = api_request(self.base_url, "/api/status", timeout=4)
                self.status_received.emit(status, usb)
            except Exception as e:
                self.poll_failed.emit(str(e), usb)
            self.msleep(int(self.interval_s * 1000))

    def stop(self):
        self._running = False


# ------------------------------------------------------------- capture log

class CaptureLog:
    """Per-date JSON log of triggered captures and their expected filenames.

    capture_logs/YYYY-MM-DD.json:
    [{"time": iso, "clips": {"<serial>": "B20260610_4681.MP4", ...}}, ...]
    """

    def __init__(self):
        LOG_DIR.mkdir(exist_ok=True)

    def _path(self, day):
        return LOG_DIR / f"{day}.json"

    def load(self, day):
        p = self._path(day)
        if not p.exists():
            return []
        try:
            return json.loads(p.read_text())
        except json.JSONDecodeError:
            return []

    def append(self, day, clips):
        entries = self.load(day)
        if entries and entries[-1]["clips"] == clips:
            return  # same capture already logged (e.g. duplicate detection)
        entries.append({"time": datetime.now().isoformat(timespec="seconds"),
                        "clips": clips})
        self._path(day).write_text(json.dumps(entries, indent=2))

    def expected_files(self, day):
        files = []
        for entry in self.load(day):
            files.extend(entry["clips"].values())
        return files


# ------------------------------------------------------------ verification

def list_remote_mp4s(cfg, day):
    """List .MP4 basenames in the remote date folder via rclone (the remote is
    the source of truth; the FUSE mount's dir cache can be hours stale)."""
    remote = f"{cfg['rclone_remote_root']}/{day}"
    out = subprocess.run(
        [cfg["rclone_binary"], "lsf", "-R", "--files-only", remote],
        capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        stderr = out.stderr.strip().splitlines()
        # missing date folder on the remote = simply no files yet
        if "directory not found" in out.stderr:
            return set(), remote
        raise RuntimeError(stderr[-1] if stderr else f"rclone exit {out.returncode}")
    present = {Path(line).name for line in out.stdout.splitlines()
               if line.upper().endswith(".MP4")}
    return present, remote


def list_local_mp4s(cfg):
    """All .MP4 basenames anywhere under the staging dir (inbox, per-date
    folders, any nested raw/<position>/ layout). Scanned in one walk rather
    than per date, so clips still sitting in the inbox — or filed under a
    different date's folder — still count as held locally."""
    root = Path(cfg["staging_dir"])
    if not root.is_dir():
        return set()
    return {p.name for p in root.rglob("*") if p.suffix.upper() == ".MP4"}


def verify_date(cfg, day, expected_files):
    """Compare expected clip filenames against .MP4 files on the remote drive
    (recursive, so the raw/<position>/ subfolder layout doesn't matter).
    Falls back to scanning the FUSE mount if rclone fails."""
    try:
        present, location = list_remote_mp4s(cfg, day)
    except Exception as e:
        date_dir = Path(cfg["studio_root"]) / str(day)
        location = f"{date_dir} (mount fallback — rclone failed: {e})"
        present = set()
        if date_dir.is_dir():
            present = {p.name for p in date_dir.rglob("*")
                       if p.suffix.upper() == ".MP4"}
    expected = set(expected_files)
    return {
        "date_dir": str(location),
        "expected": sorted(expected),
        "present_count": len(present),
        "matched": sorted(expected & present),
        "missing": sorted(expected - present),
        "untracked": sorted(present - expected),
    }


CAM_PREFIXES = ["L", "R", "M", "A", "B"]


class DateCountThread(QThread):
    """List the most recent date folders on the research drive and count unique
    .MP4 clips per camera prefix. Only clips whose FILENAME date matches the
    folder date are counted — misplaced files (handled by an external cleanup
    script) are ignored. Reads the remote directly because the FUSE mount's
    directory cache is up to 2 hours stale."""
    row_ready = pyqtSignal(str, dict, int)
    finished_all = pyqtSignal(int)

    def __init__(self, cfg, limit=None):
        super().__init__()
        self.cfg = cfg
        self.limit = limit
        self._running = True

    def run(self):
        try:
            out = subprocess.run(
                [self.cfg["rclone_binary"], "lsf", "--dirs-only",
                 self.cfg["rclone_remote_root"]],
                capture_output=True, text=True, timeout=120)
            dates = sorted(
                (d.rstrip("/") for d in out.stdout.splitlines()
                 if re.fullmatch(r"\d{4}-\d{2}-\d{2}/?", d)),
                reverse=True)
        except Exception:
            self.finished_all.emit(0)
            return
        if self.limit:
            dates = dates[:self.limit]
        scanned = 0
        for day in dates:
            if not self._running:
                break
            try:
                present, _ = list_remote_mp4s(self.cfg, day)
            except Exception:
                continue
            counts = {}
            matching = {f for f in present if clip_date(f) == day}
            for f in matching:
                prefix = f[:1].upper()
                counts[prefix] = counts.get(prefix, 0) + 1
            if self._running:
                self.row_ready.emit(day, counts, len(matching))
                scanned += 1
        self.finished_all.emit(scanned)

    def stop(self):
        self._running = False


CLIP_RE = re.compile(r"^[A-Z](\d{4})(\d{2})(\d{2})_\d+\.MP4$", re.IGNORECASE)


def clip_date(filename):
    """B20260610_4681.MP4 -> '2026-06-10', or None if the name doesn't match."""
    m = CLIP_RE.match(filename)
    return f"{m.group(1)}-{m.group(2)}-{m.group(3)}" if m else None


def sort_inbox(cfg):
    """Move downloaded clips from the flat staging inbox into per-recording-date
    folders (staging/{date}/raw/), using the date encoded in the clip name.
    Returns (per-date move counts, leftover files that could not be sorted)."""
    inbox = Path(cfg["staging_dir"]) / "inbox"
    moved = {}
    leftover = []
    if not inbox.is_dir():
        return moved, leftover
    for f in sorted(inbox.iterdir()):
        if not f.is_file() or f.name.startswith("."):
            continue
        day = clip_date(f.name)
        if day is None:
            leftover.append(f.name)
            continue
        dest_dir = Path(cfg["staging_dir"]) / day / cfg["download_subdir"]
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / f.name
        if dest.exists():
            if dest.stat().st_size == f.stat().st_size:
                f.unlink()  # identical clip already sorted on a previous sync
            else:
                leftover.append(f.name)  # size mismatch: keep both, flag it
            continue
        f.rename(dest)
        moved[day] = moved.get(day, 0) + 1
    return moved, leftover


def resort_date_folders(cfg):
    """Safety net: if a staging date folder contains clips whose filename date
    doesn't match the folder (e.g. left over from an older sync version), move
    them to the right date folder. Identical already-sorted copies are dropped."""
    moved = {}
    for day in staged_dates(cfg):
        raw = Path(cfg["staging_dir"]) / day / cfg["download_subdir"]
        if not raw.is_dir():
            continue
        for f in sorted(raw.iterdir()):
            if not f.is_file():
                continue
            actual = clip_date(f.name)
            if actual is None or actual == day:
                continue
            dest_dir = Path(cfg["staging_dir"]) / actual / cfg["download_subdir"]
            dest_dir.mkdir(parents=True, exist_ok=True)
            dest = dest_dir / f.name
            if dest.exists() and dest.stat().st_size == f.stat().st_size:
                f.unlink()
            elif not dest.exists():
                f.rename(dest)
            moved[actual] = moved.get(actual, 0) + 1
    return moved


def staged_dates(cfg):
    """All per-date folders currently present in staging."""
    root = Path(cfg["staging_dir"])
    if not root.is_dir():
        return []
    return sorted(d.name for d in root.iterdir()
                  if d.is_dir() and re.fullmatch(r"\d{4}-\d{2}-\d{2}", d.name))


class CaptureCheckThread(QThread):
    """Gather where today's clips are: on the remote drive and/or on local disk
    (staging inbox + sorted date folder), so capture entries can be ticked off."""
    result = pyqtSignal(str, object, object)  # day, remote-set-or-None, local-set

    def __init__(self, cfg, day):
        super().__init__()
        self.cfg = cfg
        self.day = day

    def run(self):
        local = set()
        staging = Path(self.cfg["staging_dir"])
        for sub in (staging / "inbox",
                    staging / self.day / self.cfg["download_subdir"]):
            if sub.is_dir():
                local |= {p.name for p in sub.iterdir()
                          if p.suffix.upper() == ".MP4"}
        try:
            present, _ = list_remote_mp4s(self.cfg, self.day)
            self.result.emit(self.day, present, local)
        except Exception:
            self.result.emit(self.day, None, local)


class UploadThread(QThread):
    """rclone copy: every staged date folder -> its matching remote date folder.
    Already-uploaded clips are skipped by rclone, so re-runs are cheap."""
    progress = pyqtSignal(str)
    finished_ok = pyqtSignal(list)
    failed = pyqtSignal(str)

    def __init__(self, cfg, days):
        super().__init__()
        self.cfg = cfg
        self.days = days

    def run(self):
        done = []
        for day in self.days:
            sub = self.cfg["download_subdir"]
            src = str(Path(self.cfg["staging_dir"]) / day / sub)
            dst = f"{self.cfg['rclone_remote_root']}/{day}/{sub}"
            self.progress.emit(f"{day} uploaden -> onderzoeksschijf…")
            try:
                out = subprocess.run(
                    [self.cfg["rclone_binary"], "copy", src, dst,
                     "--transfers", "2", "--stats-one-line", "--stats", "30s"],
                    capture_output=True, text=True, timeout=6 * 3600)
            except Exception as e:
                self.failed.emit(f"{day}: {e}")
                return
            if out.returncode != 0:
                stderr = out.stderr.strip().splitlines()
                self.failed.emit(f"{day}: " + (stderr[-1] if stderr else f"rclone exit {out.returncode}"))
                return
            done.append(day)
        self.finished_ok.emit(done)


# ------------------------------------------------------------- format steps

# The visible stages of "Slot 1 formatteren", in the order they actually occur.
# Note the camera mode switch back to Remote happens at the END of the listing
# (the server reconnects before /api/list-files reports done), so comparing the
# file lists comes AFTER the cameras are already back in recording mode.
FORMAT_STEPS = [
    "Camera's omschakelen naar overdrachtsmodus",
    "Bestanden op de camera's oplijsten",
    "Camera's terugschakelen naar opnamemodus",
    "Bestanden vergelijken met schijf en lokale opslag",
    "Ontbrekende bestanden synchroniseren",
    "Slot 1 formatteren",
    "Klaar",
]
(S_MODE_OUT, S_LIST, S_MODE_BACK, S_COMPARE,
 S_SYNC, S_FORMAT, S_DONE) = range(len(FORMAT_STEPS))


def dutch_list_status(text):
    """The C++ server reports listing progress in English; this window is Dutch."""
    if text.startswith("Disconnecting cameras from Remote mode"):
        return "Camera's loskoppelen van de opnamemodus…"
    if text.startswith("Scanning for cameras in ContentsTransfer mode"):
        return "Camera's zoeken in overdrachtsmodus…"
    if text.startswith("Listing files on "):
        return f"Bestanden oplijsten op {text[len('Listing files on '):].rstrip('. ')}…"
    m = re.match(r"Listing complete\. (\d+) file\(s\)", text)
    if m:
        if "Reconnecting" in text:
            return f"{m.group(1)} bestanden gevonden — camera's terugschakelen…"
        return f"{m.group(1)} bestanden op de camera's gevonden."
    if text.startswith("Error: No cameras found"):
        return "Fout: geen camera's gevonden."
    if text.startswith("Error: Could not connect"):
        return "Fout: kon geen verbinding maken in overdrachtsmodus."
    return text


class StepFlow(QWidget):
    """Horizontal step indicator: numbered circles joined by connectors, with a
    caption under each. Aimed at non-technical users — shape and colour carry
    the state, so it reads at a glance without parsing any text."""

    PENDING, ACTIVE, DONE, FAILED, SKIPPED = range(5)

    COLORS = {
        PENDING: QColor("#c3c9d2"),
        ACTIVE:  QColor("#1565c0"),
        DONE:    QColor("#2e7d32"),
        FAILED:  QColor("#c62828"),
        SKIPPED: QColor("#b6bcc5"),
    }
    GLYPHS = {DONE: "✓", FAILED: "✗", SKIPPED: "–"}

    R = 22            # circle radius
    TOP = 16          # margin above the circles
    GAP = 14          # circle -> caption gap

    def __init__(self, steps, parent=None):
        super().__init__(parent)
        self.steps = list(steps)
        self.states = [self.PENDING] * len(self.steps)
        self._phase = 0.0
        self._anim = QTimer(self)
        self._anim.timeout.connect(self._tick)
        pol = QSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)
        pol.setHeightForWidth(True)
        self.setSizePolicy(pol)

    # ------------------------------------------------------------ geometry
    # Captions wrap, so the height needed depends on the width the layout hands
    # us: height-for-width keeps the last line from being clipped on a narrow
    # window without padding out a wide one.

    def _caption_font(self, bold=False):
        return QFont("", 11, QFont.Weight.Bold if bold else QFont.Weight.Normal)

    def _caption_height(self, slot_w):
        """Tallest wrapped caption at this column width, measured in the bold
        face so a step does not grow (and clip) the moment it becomes active."""
        fm = QFontMetrics(self._caption_font(bold=True))
        tallest = 0
        for label in self.steps:
            r = fm.boundingRect(
                QRect(0, 0, max(int(slot_w) - 12, 40), 4000),
                Qt.AlignmentFlag.AlignHCenter | Qt.TextFlag.TextWordWrap, label)
            tallest = max(tallest, r.height())
        return tallest + 8

    def hasHeightForWidth(self):
        return True

    def heightForWidth(self, width):
        slot = max(width, 1) / max(len(self.steps), 1)
        return self.TOP + 2 * self.R + self.GAP + self._caption_height(slot)

    def sizeHint(self):
        return QSize(1100, self.heightForWidth(1100))

    def minimumSizeHint(self):
        return QSize(620, self.heightForWidth(620))

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.setMinimumHeight(self.heightForWidth(self.width()))

    # ---------------------------------------------------------- state API

    def reset(self):
        self.states = [self.PENDING] * len(self.steps)
        self._sync_anim()
        self.update()

    def set_state(self, index, state):
        if 0 <= index < len(self.states):
            self.states[index] = state
            self._sync_anim()
            self.update()

    def advance_to(self, index):
        """Activate `index`, resolving every earlier unfinished step as done.
        Any other step still marked ACTIVE is also resolved, so exactly one
        circle is ever live — the sync step sits after "vergelijken" in the row
        but hands control back to it, which would otherwise light up both.
        Steps already SKIPPED or FAILED keep their state."""
        for i, state in enumerate(self.states):
            if state not in (self.PENDING, self.ACTIVE):
                continue
            if i < index or (state == self.ACTIVE and i != index):
                self.states[i] = self.DONE
        self.set_state(index, self.ACTIVE)

    def finish_all(self):
        for i, s in enumerate(self.states):
            if s in (self.PENDING, self.ACTIVE):
                self.states[i] = self.DONE
        self._sync_anim()
        self.update()

    def fail_current(self):
        """Mark the in-progress step failed; anything still pending stays pending."""
        for i, s in enumerate(self.states):
            if s == self.ACTIVE:
                self.states[i] = self.FAILED
        self._sync_anim()
        self.update()

    # ------------------------------------------------------------ drawing

    def _sync_anim(self):
        if self.ACTIVE in self.states:
            if not self._anim.isActive():
                self._anim.start(40)
        else:
            self._anim.stop()

    def _tick(self):
        self._phase = (self._phase + 0.13) % (2 * math.pi)
        self.update()

    def paintEvent(self, _event):
        n = len(self.steps)
        if not n:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        slot = self.width() / n
        cy = self.TOP + self.R
        cxs = [slot * (i + 0.5) for i in range(n)]

        # Connectors are green only up to how far we have actually got: a
        # SKIPPED step must not colour the link to a step we have not reached.
        reached = [i for i, s in enumerate(self.states)
                   if s in (self.ACTIVE, self.DONE, self.FAILED)]
        frontier = max(reached) if reached else -1
        for i in range(n - 1):
            behind = i < frontier and self.states[i] in (self.DONE, self.SKIPPED)
            pen = QPen(self.COLORS[self.DONE] if behind else self.COLORS[self.PENDING], 4)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            p.setPen(pen)
            p.drawLine(int(cxs[i] + self.R + 7), int(cy),
                       int(cxs[i + 1] - self.R - 7), int(cy))

        num_font = QFont("", 15, QFont.Weight.Bold)
        for i, (label, state) in enumerate(zip(self.steps, self.states)):
            cx, color = cxs[i], self.COLORS[state]

            # breathing halo marks the step that is running right now
            if state == self.ACTIVE:
                halo = QColor(color)
                halo.setAlpha(55)
                grow = self.R + 5 + 6 * (1 + math.sin(self._phase)) / 2
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(halo)
                p.drawEllipse(QPointF(cx, cy), grow, grow)

            filled = state in (self.ACTIVE, self.DONE, self.FAILED)
            p.setPen(Qt.PenStyle.NoPen if filled else QPen(color, 3))
            p.setBrush(color if filled else Qt.BrushStyle.NoBrush)
            p.drawEllipse(QPointF(cx, cy), float(self.R), float(self.R))

            p.setFont(num_font)
            p.setPen(QColor("white") if filled else color)
            p.drawText(QRect(int(cx - self.R), int(cy - self.R), 2 * self.R, 2 * self.R),
                       Qt.AlignmentFlag.AlignCenter,
                       self.GLYPHS.get(state, str(i + 1)))

            p.setFont(self._caption_font(bold=state in (self.ACTIVE, self.FAILED)))
            if state == self.PENDING:
                p.setPen(QColor("#98a0aa"))
            elif state == self.SKIPPED:
                p.setPen(self.COLORS[self.SKIPPED])
            elif state == self.FAILED:
                p.setPen(self.COLORS[self.FAILED])
            else:
                p.setPen(QColor("#2b2f36"))
            p.drawText(
                QRect(int(cx - slot / 2 + 6), int(cy + self.R + self.GAP),
                      int(slot - 12), self._caption_height(slot)),
                Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop
                | Qt.TextFlag.TextWordWrap,
                label)
        p.end()


class FormatThread(QThread):
    """POST /api/format off the GUI thread. The server formats every camera
    synchronously inside the request handler, so calling it inline would freeze
    the window — and the step indicator — for the whole format."""
    done = pyqtSignal(object)
    failed = pyqtSignal(str)

    def __init__(self, cfg):
        super().__init__()
        self.cfg = cfg

    def run(self):
        try:
            resp = api_request(self.cfg["api_url"], "/api/format", "POST", timeout=300)
        except Exception as e:
            self.failed.emit(str(e))
            return
        self.done.emit(resp)


# -------------------------------------------------------------- camera card

class CameraCard(QFrame):
    def __init__(self):
        super().__init__()
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setMinimumWidth(190)
        layout = QVBoxLayout(self)
        layout.setSpacing(2)
        self.title = QLabel("—")
        self.title.setFont(QFont("", 12, QFont.Weight.Bold))
        self.rec = QLabel("")
        self.rec.setFont(QFont("", 11, QFont.Weight.Bold))
        self.lines = [QLabel("") for _ in range(6)]
        layout.addWidget(self.title)
        layout.addWidget(self.rec)
        for l in self.lines:
            layout.addWidget(l)

    def update_cam(self, c):
        serial = c["model"].split("(")[-1].rstrip(")")
        prefix = (c.get("clipName") or "?")[:1]
        self.title.setText(f"[{prefix}] …{serial[-4:]}")
        if not c["connected"]:
            self.rec.setText("NIET VERBONDEN")
            self.rec.setStyleSheet("color: gray;")
            self.setStyleSheet("background-color: #eeeeee;")
        elif c["recording"]:
            self.rec.setText("● OPN")
            self.rec.setStyleSheet("color: red;")
            self.setStyleSheet("background-color: #ffecec;")
        else:
            self.rec.setText("inactief")
            self.rec.setStyleSheet("color: green;")
            self.setStyleSheet("")
        heat = {0: "", 1: " ⚠ BIJNA OVERVERHIT", 2: " 🔥 OVERVERHIT"}[c.get("heatState", 0)]
        self.lines[0].setText(f"Batterij: {c['battery']}%{heat}")
        self.lines[1].setText(f"{c['iso']}  {c['shutterSpeed']}  {c['fNumber']}")
        self.lines[2].setText(f"WB: {c['whiteBalance']} {c['colorTemp']}K")
        self.lines[3].setText(f"Slot1: {c['mediaSlot1Min']} min")
        self.lines[4].setText(f"{c['movieFormat']} {c['frameRate']}")
        self.lines[5].setText(f"Volgende clip: {c['clipName']}")

    def clear_cam(self):
        self.title.setText("—")
        self.rec.setText("geen camera")
        self.rec.setStyleSheet("color: gray;")
        for l in self.lines:
            l.setText("")
        self.setStyleSheet("background-color: #eeeeee;")


# --------------------------------------------------------------- main window

class MainWindow(QMainWindow):
    NUM_SLOTS = 5

    def __init__(self, cfg):
        super().__init__()
        self.cfg = cfg
        self.capture_log = CaptureLog()
        self.server_proc = None
        self.last_status = None
        self.sync_in_progress = False
        self.format_check_pending = False
        self.format_sync_attempted = False
        self.format_after_sync = False
        self.format_flow_active = False
        self.listing_was_active = False
        self.drive_files_today = None
        self.local_files_today = None
        self.chrome_launched = False
        self.server_autostart_attempted = False
        self.was_recording = False
        self.was_downloading = False
        self.idle_clips = {}
        self.setWindowTitle("FX30 Multi-Camera Bediening")
        self._build_ui()

        self.poller = StatusPoller(cfg["api_url"], cfg["poll_interval_ms"])
        self.poller.status_received.connect(self.on_status)
        self.poller.poll_failed.connect(self.on_poll_failed)
        self.poller.start()

        self.refresh_capture_table()
        self.scan_date_counts()
        self.check_captures()

        # keep capture sync-status fresh even when no app-driven event fires
        # (e.g. uploads triggered elsewhere, or clips arriving on the drive)
        self.capture_timer = QTimer(self)
        self.capture_timer.timeout.connect(self.check_captures)
        self.capture_timer.start(60_000)

        # While only SOME of the expected cameras are connected, keep polling
        # the server to scan for the rest automatically (every 5 s) so the user
        # doesn't have to press "Rescan" once the missing cameras come online.
        self.auto_rescan_timer = QTimer(self)
        self.auto_rescan_timer.setInterval(5000)
        self.auto_rescan_timer.timeout.connect(self._auto_rescan)

        # Startup catch-up: a download that finished while the app was NOT
        # running leaves clips stranded in the inbox (never sorted/uploaded).
        # Recover them now. Delayed so the first status poll arrives first
        # (so we can tell whether a transfer is currently in progress).
        QTimer.singleShot(3000, self.catch_up_inbox)

        # Open the recording page fullscreen on the extended monitor. Try right
        # away; if the external display hasn't woken yet, retry a few times
        # (non-blocking) instead of making the user wait.
        self._display_attempts = 0
        self.open_recording_display()

        # Keep the QR-scherm status indicator in the server bar current, also
        # when the kiosk window is opened/closed outside this app.
        self.display_status_timer = QTimer(self)
        self.display_status_timer.timeout.connect(self._update_display_status)
        self.display_status_timer.start(5000)
        self._update_display_status()

    # ---------------------------------------------------------------- UI

    def _build_ui(self):
        central = QWidget()
        root = QVBoxLayout(central)

        # Server bar
        server_bar = QHBoxLayout()
        self.server_label = QLabel("Server: …")
        self.server_label.setFont(QFont("", 12, QFont.Weight.Bold))
        self.btn_restart = QPushButton("Sony SDK herstarten (hard)")
        self.btn_restart.clicked.connect(self.restart_server)
        self.btn_reset = QPushButton("USB-reset (zacht)")
        self.btn_reset.clicked.connect(lambda: self.post_simple("/api/reset"))
        self.btn_scan = QPushButton("Opnieuw scannen")
        self.btn_scan.clicked.connect(lambda: self.post_simple("/api/scan"))
        self.btn_chrome = QPushButton("🌐 Studiopagina openen")
        self.btn_chrome.clicked.connect(self.open_studio_page)
        self.display_status_label = QLabel("QR-scherm: …")
        self.btn_display = QPushButton("🖥 QR-scherm openen")
        self.btn_display.clicked.connect(self.open_display_manually)
        server_bar.addWidget(self.server_label)
        server_bar.addStretch()
        server_bar.addWidget(self.display_status_label)
        server_bar.addWidget(self.btn_display)
        server_bar.addWidget(self.btn_chrome)
        server_bar.addWidget(self.btn_scan)
        server_bar.addWidget(self.btn_reset)
        server_bar.addWidget(self.btn_restart)
        root.addLayout(server_bar)

        self.activity_label = QLabel("")
        root.addWidget(self.activity_label)

        # Format progress — hidden until "Slot 1 formatteren" is pressed
        self.format_box = QGroupBox("Formatteren — voortgang")
        fmt_layout = QVBoxLayout(self.format_box)
        self.format_flow = StepFlow(FORMAT_STEPS)
        self.format_detail = QLabel("")
        self.format_detail.setWordWrap(True)
        self.format_detail.setStyleSheet("color: #555; padding-left: 4px;")
        fmt_layout.addWidget(self.format_flow)
        fmt_layout.addWidget(self.format_detail)
        self.format_box.setVisible(False)
        root.addWidget(self.format_box)

        # Camera cards
        cams_box = QGroupBox("Camera's")
        cams_layout = QHBoxLayout(cams_box)
        self.cards = [CameraCard() for _ in range(self.NUM_SLOTS)]
        for card in self.cards:
            cams_layout.addWidget(card)
        root.addWidget(cams_box)

        # Capture controls
        ctrl = QHBoxLayout()
        self.btn_start = QPushButton("▶ Opname starten")
        self.btn_start.setStyleSheet("font-weight: bold; color: white; background-color: #c62828; padding: 8px;")
        self.btn_start.clicked.connect(self.start_capture)
        self.btn_stop = QPushButton("■ Opname stoppen")
        self.btn_stop.setStyleSheet("font-weight: bold; padding: 8px;")
        self.btn_stop.clicked.connect(self.stop_capture)
        self.btn_format = QPushButton("Slot 1 formatteren (alle camera's)")
        self.btn_format.clicked.connect(self.format_media)
        self.btn_sync = QPushButton("⇣ Videobestanden synchroniseren")
        self.btn_sync.clicked.connect(self.sync_files)
        ctrl.addWidget(self.btn_start)
        ctrl.addWidget(self.btn_stop)
        ctrl.addStretch()
        ctrl.addWidget(self.btn_format)
        ctrl.addWidget(self.btn_sync)
        root.addLayout(ctrl)

        # Capture log + verification
        bottom = QHBoxLayout()

        log_box = QGroupBox("Gestarte opnames (vandaag)")
        log_layout = QVBoxLayout(log_box)
        self.capture_table = QTableWidget(0, 3)
        self.capture_table.setHorizontalHeaderLabels(["Tijd", "Op schijf", "Verwachte bestanden"])
        self.capture_table.horizontalHeader().setStretchLastSection(True)
        self.capture_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        log_layout.addWidget(self.capture_table)
        bottom.addWidget(log_box, 1)

        verify_box = QGroupBox("Gesynchroniseerde bestanden op schijf verifiëren")
        verify_layout = QVBoxLayout(verify_box)
        self.verify_output = QTextEdit()
        self.verify_output.setReadOnly(True)
        verify_layout.addWidget(self.verify_output, 1)

        cols = ["Datum"] + CAM_PREFIXES + ["overig", "Totaal"]
        self.date_table = QTableWidget(0, len(cols))
        self.date_table.setHorizontalHeaderLabels(cols)
        self.date_table.horizontalHeader().setStretchLastSection(True)
        self.date_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        verify_layout.addWidget(self.date_table, 2)
        bottom.addWidget(verify_box, 1)

        root.addLayout(bottom)
        self.setCentralWidget(central)
        self.resize(1150, 760)

        # Big centered overlay box for camera-connection guidance, with its own
        # action buttons (it covers the toolbar, so actions must live inside it)
        self.banner = QFrame(central)
        self.banner.setVisible(False)
        banner_layout = QVBoxLayout(self.banner)
        banner_layout.setContentsMargins(30, 30, 30, 24)
        banner_layout.setSpacing(18)
        self.banner_label = QLabel("")
        self.banner_label.setWordWrap(True)
        self.banner_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.banner_label.setFont(QFont("", 20, QFont.Weight.Bold))
        self.banner_label.setStyleSheet("background: transparent; border: none;")
        banner_layout.addWidget(self.banner_label)
        self.banner_btn_row = QHBoxLayout()
        self.banner_btn_row.setSpacing(12)
        banner_layout.addLayout(self.banner_btn_row)
        self._banner_state = None

    def _position_banner(self):
        if not self.centralWidget():
            return
        cw = self.centralWidget()
        w = int(cw.width() * 0.7)
        self.banner.setFixedWidth(w)
        h = max(self.banner.sizeHint().height(), 170)
        self.banner.setFixedHeight(h)
        self.banner.move((cw.width() - w) // 2, (cw.height() - h) // 2)
        self.banner.raise_()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, "banner"):
            self._position_banner()

    # ----------------------------------------------------------- status

    STYLE_ERR = ("background-color: rgba(198, 40, 40, 235); color: white; "
                 "padding: 30px; border-radius: 14px; border: 3px solid white;")
    STYLE_WARN = ("background-color: rgba(239, 108, 0, 235); color: white; "
                  "padding: 30px; border-radius: 14px; border: 3px solid white;")

    BANNER_BTN_STYLE = ("QPushButton { background-color: white; color: #222; "
                        "font-weight: bold; font-size: 15px; padding: 10px 22px; "
                        "border-radius: 8px; border: none; } "
                        "QPushButton:hover { background-color: #eee; }")

    def show_banner(self, text, style, buttons=None):
        state = (text, style, tuple(lbl for lbl, _ in (buttons or [])))
        if state == self._banner_state and self.banner.isVisible():
            return  # unchanged: don't rebuild (buttons must survive clicks)
        self._banner_state = state
        self.banner_label.setText(text)
        self.banner.setStyleSheet(f"QFrame {{ {style} }}")
        while self.banner_btn_row.count():
            item = self.banner_btn_row.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        if buttons:
            self.banner_btn_row.addStretch()
            for lbl, cb in buttons:
                b = QPushButton(lbl)
                b.setStyleSheet(self.BANNER_BTN_STYLE)
                b.setCursor(Qt.CursorShape.PointingHandCursor)
                b.clicked.connect(cb)
                self.banner_btn_row.addWidget(b)
            self.banner_btn_row.addStretch()
        self.banner.setVisible(True)
        self._position_banner()

    def update_banner(self, connected, usb, scanning):
        """Plain-language guidance for camera connection trouble."""
        # Auto-rescan only while we're waiting for MORE cameras to join a
        # partially-connected rig (handled in the connected < usb branch below).
        partial = (not scanning) and usb > 0 and 0 < connected < usb
        self._set_auto_rescan(partial)
        if scanning:
            if connected == 0 and usb == 0:
                self.show_banner("⏳ Wachten tot camera's\nworden AANGEZET…",
                                 self.STYLE_WARN)
            else:
                self.show_banner("🔄 Verbinden met camera's…\ndit kan even duren.",
                                 self.STYLE_WARN)
            return
        if usb == 0:
            self.show_banner(
                "📷 Alle camera's staan UIT\n\n"
                "Zet de camera's AAN —\nze verbinden automatisch.",
                self.STYLE_WARN,
                [("Nu opnieuw scannen", lambda: self.post_simple("/api/scan"))])
        elif connected == 0:
            self.show_banner(
                f"⚠ {usb if usb > 0 else 'De'} camera('s) REAGEREN NIET\n\n"
                "Zet ze uit en weer aan:\nschakel ALLE camera's UIT en dan weer AAN.",
                self.STYLE_ERR,
                [("USB-reset (zacht)", lambda: self.post_simple("/api/reset")),
                 ("Sony SDK herstarten", self.restart_server_now)])
        elif usb > 0 and connected < usb:
            self.show_banner(
                f"⚠ Slechts {connected} van {usb} camera's verbonden\n\n"
                "Automatisch opnieuw scannen elke 5 s…\nAls ze blijven ontbreken, "
                "zet de niet-reagerende camera's uit en weer aan.",
                self.STYLE_WARN,
                [("Nu opnieuw scannen", lambda: self.post_simple("/api/scan")),
                 ("USB-reset (zacht)", lambda: self.post_simple("/api/reset"))])
        else:
            self.hide_banner()

    # Chrome kiosk for the recording display (opened on the extended monitor).
    CHROME_BIN = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    KIOSK_PROFILE = os.path.expanduser("~/.signcollect-kiosk-chrome")

    def _display_target_screen(self):
        """The extended monitor to show the recording page on: the screen whose
        name matches display_screen and that is NOT the primary. A mirrored
        display never appears as its own screen, so a standalone match is always
        the extended one. Returns None until such a screen is available."""
        want = self.cfg.get("display_screen", "PHL").lower()
        app = QApplication.instance()
        primary = app.primaryScreen()
        for s in app.screens():
            if s is not primary and want in (s.name() or "").lower():
                return s
        return None

    def _kiosk_open(self):
        """True if the QR/recording kiosk window is currently running."""
        return subprocess.run(["pgrep", "-f", self.KIOSK_PROFILE],
                              capture_output=True).returncode == 0

    def _launch_kiosk(self, screen):
        """Start the Chrome kiosk with the recording page on the given screen."""
        g = screen.geometry()
        subprocess.Popen([
            self.CHROME_BIN,
            f"--user-data-dir={self.KIOSK_PROFILE}",
            "--no-first-run", "--no-default-browser-check",
            "--disable-session-crashed-bubble", "--disable-infobars",
            f"--app={self.cfg['display_url']}",
            f"--window-position={g.x()},{g.y()}",
            f"--window-size={g.width()},{g.height()}",
            "--start-fullscreen",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        QTimer.singleShot(2000, self._update_display_status)

    def open_recording_display(self):
        """Open the recording page fullscreen on the extended monitor via a
        dedicated Chrome kiosk profile. Idempotent: does nothing if that window
        is already running. If the monitor isn't up yet, retry a few times."""
        if not self.cfg.get("display_url"):
            return
        if self._kiosk_open():
            return  # already open
        screen = self._display_target_screen()
        if screen is None:
            self._display_attempts += 1
            if self._display_attempts <= 6:  # ~30 s of 5 s retries, non-blocking
                QTimer.singleShot(5000, self.open_recording_display)
            else:
                self.notify("QR-scherm: uitgebreide monitor niet gevonden — "
                            "overgeslagen. Gebruik de knop '🖥 QR-scherm openen' "
                            "zodra de monitor aan staat.")
            return
        self._launch_kiosk(screen)

    def open_display_manually(self):
        """Button handler: open the QR/recording kiosk now, with clear feedback
        instead of the silent retries of the automatic startup path."""
        if not self.cfg.get("display_url"):
            self.notify("QR-scherm: geen display_url ingesteld in config.json.")
            return
        if self._kiosk_open():
            self.notify("QR-scherm is al open op de externe monitor.")
            return
        screen = self._display_target_screen()
        if screen is None:
            self.notify(f"QR-scherm: externe monitor "
                        f"({self.cfg.get('display_screen', 'PHL')}) niet gevonden. "
                        "Controleer of die aan staat en probeer opnieuw.")
            return
        self._launch_kiosk(screen)
        self.notify("QR-scherm wordt geopend op de externe monitor.")

    def _update_display_status(self):
        """Refresh the QR-scherm indicator + button in the server bar."""
        if self._kiosk_open():
            self.display_status_label.setText("QR-scherm: ● open")
            self.display_status_label.setStyleSheet("color: green;")
            self.btn_display.setEnabled(False)
        else:
            self.display_status_label.setText("QR-scherm: ○ niet open")
            self.display_status_label.setStyleSheet("color: #c62828;")
            self.btn_display.setEnabled(True)

    def _set_auto_rescan(self, active):
        """Start/stop the 5-second background rescan used while waiting for the
        remaining cameras of a partially-connected rig."""
        if active:
            if not self.auto_rescan_timer.isActive():
                self.auto_rescan_timer.start()
        elif self.auto_rescan_timer.isActive():
            self.auto_rescan_timer.stop()

    def _auto_rescan(self):
        """Fire a /api/scan on the interval, but skip if the server is already
        busy (a scan/download/listing is running) to avoid stacking requests."""
        st = self.last_status or {}
        if st.get("scanning") or st.get("downloading") or st.get("listing"):
            return
        self.post_simple("/api/scan", quiet=True)

    def hide_banner(self):
        self.banner.setVisible(False)
        self._banner_state = None

    def on_status(self, status, usb=-1):
        self.last_status = status
        self.server_label.setText("Server: ● online")
        self.server_label.setStyleSheet("color: green;")
        cams = status.get("cameras", [])
        for i, card in enumerate(self.cards):
            if i < len(cams):
                card.update_cam(cams[i])
            else:
                card.clear_cam()

        activity = []
        if status.get("scanning"):
            activity.append(f"Scannen: {status.get('scanStatus', '')}")
        if status.get("downloading"):
            activity.append(f"Downloaden: {status.get('downloadStatus', '')}")
        elif self.sync_in_progress:
            # camera download just finished -> upload staging to the remote
            self.sync_in_progress = False
            activity.append(f"Download voltooid: {status.get('downloadStatus', '')}")
            self.start_upload()
        elif self.was_downloading:
            # a download triggered by another client (web app) just finished:
            # run the rest of the pipeline (sort -> upload -> verify) so the
            # clips reach the research drive without any manual step
            self.notify("Download (extern) voltooid — sorteren en uploaden "
                        "naar de onderzoeksschijf…")
            self.start_upload()
        self.was_downloading = bool(status.get("downloading"))
        if status.get("listing"):
            self.listing_was_active = True
            activity.append(f"Oplijsten: {status.get('listStatus', '')}")
            self._track_format_listing(status.get("listStatus", ""))
        elif self.listing_was_active:
            self.listing_was_active = False
            activity.append(f"Oplijsten voltooid: {status.get('listStatus', '')}")
            if self.format_check_pending:
                self.format_check_pending = False
                self.finish_format_check()
        self.activity_label.setText("   |   ".join(activity))

        # Captures are logged by the C++ server itself at /api/start time (exact
        # clip names, any client). We just refresh the table when recording ends.
        rec_now = any(c["recording"] for c in cams)
        if self.was_recording and not rec_now:
            self.refresh_capture_table()
        elif rec_now and not self.was_recording:
            self.refresh_capture_table()
        self.was_recording = rec_now

        connected = sum(1 for c in cams if c["connected"])
        if status.get("downloading") or status.get("listing"):
            self.hide_banner()  # cameras intentionally disconnected
        else:
            self.update_banner(connected, usb, bool(status.get("scanning")))

        # First time the FULL rig is up (all expected cameras connected AND
        # physically on USB): open the studio recording page. The USB check
        # guards against the server briefly reporting stale "connected"
        # cameras after they have been switched off.
        expected = self.cfg.get("expected_cameras", 5)
        all_up = connected >= expected and (usb < 0 or usb >= expected)
        if all_up and not self.chrome_launched and self.cfg.get("studio_url"):
            self.chrome_launched = True
            if studio_page_already_open(self.cfg["studio_url"]):
                self.notify(f"Alle {connected} camera's verbonden — studiopagina is "
                            "al open in Chrome.")
            else:
                subprocess.Popen(["open", "-a", "Google Chrome", self.cfg["studio_url"]],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.notify(f"Alle {connected} camera's verbonden — studiopagina openen in Chrome.")

        busy = (status.get("scanning") or status.get("downloading")
                or status.get("listing"))
        recording = any(c["recording"] for c in cams)
        self.btn_start.setEnabled(not busy and bool(cams) and not recording)
        self.btn_stop.setEnabled(not status.get("downloading") and recording)
        self.btn_format.setEnabled(not busy and bool(cams))
        self.btn_sync.setEnabled(not busy)
        self.btn_scan.setEnabled(not busy)
        self.btn_reset.setEnabled(not busy)

    def on_poll_failed(self, err, usb=-1):
        self.last_status = None
        self.server_label.setText("Server: ○ onbereikbaar")
        self.server_label.setStyleSheet("color: red;")
        self.activity_label.setText(err)
        if not self.server_autostart_attempted:
            # e.g. fresh boot via the LaunchAgent: bring the server up ourselves
            self.server_autostart_attempted = True
            if not subprocess.run(["pgrep", "-f", "fx30MultiRecord"],
                                  capture_output=True).stdout:
                self._launch_server()
                self.show_banner("🔄 Cameraserver starten…", self.STYLE_WARN)
                return
        self.show_banner(
            "⚠ De cameraserver draait niet",
            self.STYLE_ERR,
            [("Cameraserver starten", self._launch_server)])
        for card in self.cards:
            card.clear_cam()
        for b in (self.btn_start, self.btn_stop, self.btn_format,
                  self.btn_sync, self.btn_scan, self.btn_reset):
            b.setEnabled(False)
        self.btn_restart.setEnabled(True)

    # ---------------------------------------------------------- actions

    def post_simple(self, path, body=None, quiet=False):
        """POST to the server. When quiet=True, failures/busy responses are
        swallowed silently (used by the background auto-rescan so it never
        spams dialogs)."""
        try:
            resp = api_request(self.cfg["api_url"], path, "POST", body, timeout=15)
        except Exception as e:
            if not quiet:
                QMessageBox.warning(self, "Verzoek mislukt", f"POST {path}\n{e}")
            return None
        if isinstance(resp, dict) and resp.get("error"):
            if not quiet:
                QMessageBox.warning(self, "Server bezig", resp["error"])
            return None
        return resp

    def start_capture(self):
        """Capture logging happens in on_status (idle->recording transition),
        which also covers captures started by the web app or dashboard."""
        if not self.last_status or not self.last_status.get("cameras"):
            QMessageBox.warning(self, "Geen camera's", "Geen camera's verbonden.")
            return
        resp = self.post_simple("/api/start")
        if resp is None:
            return
        if resp.get("failed"):
            # All-or-nothing: a capture with a missing camera angle is useless.
            stop = self.post_simple("/api/stop")
            stopped = stop.get("ok", 0) if stop else 0
            self.notify(f"⚠ Opname AFGEBROKEN: {resp['failed']} camera('s) konden niet "
                        f"starten — de andere {stopped} direct gestopt. "
                        "Controleer de camera's en probeer opnieuw.")

    def stop_capture(self):
        self.post_simple("/api/stop")

    # ------------------------------------------------- format step indicator

    def format_step(self, index, detail=None):
        """Advance the visible step flow (no-op when no format run is active)."""
        if not self.format_flow_active:
            return
        self.format_flow.advance_to(index)
        if detail is not None:
            self.format_detail.setText(detail)

    def format_step_failed(self, detail):
        if not self.format_flow_active:
            return
        self.format_flow.fail_current()
        self.format_detail.setText(detail)
        self.format_flow_active = False

    def _track_format_listing(self, list_status):
        """Map the server's English listing status onto steps 1-3."""
        if not self.format_flow_active:
            return
        if list_status.startswith("Error"):
            self.format_step_failed(dutch_list_status(list_status))
            return
        if list_status.startswith("Listing files on "):
            self.format_flow.advance_to(S_LIST)
        elif "Reconnecting" in list_status:
            self.format_flow.advance_to(S_MODE_BACK)
        elif ("ContentsTransfer" in list_status
                or list_status.startswith("Disconnecting")):
            self.format_flow.advance_to(S_MODE_OUT)
        self.format_detail.setText(dutch_list_status(list_status))

    def format_media(self):
        """Safe format, fully automatic after one confirmation: list camera
        clips, verify each one is held either on the research drive or in local
        staging, auto-sync anything held in neither, then format. Progress is
        reported in the activity line and verify panel — no further dialogs."""
        answer = QMessageBox.question(
            self, "Media formatteren",
            "De app zal:\n"
            "1. Alle clips op de camera's oplijsten (~1 min, camera's kort losgekoppeld)\n"
            "2. Controleren of elke clip op de onderzoeksschijf óf in lokale staging staat\n"
            "3. Clips die op geen van beide staan automatisch synchroniseren\n"
            "4. Slot 1 op alle camera's WISSEN zodra alles geverifieerd is\n\n"
            "Doorgaan?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        self.format_flow_active = True
        self.format_flow.reset()
        self.format_flow.set_state(S_SYNC, StepFlow.SKIPPED)  # only if clips are missing
        self.format_box.setVisible(True)
        self.format_step(S_MODE_OUT, "Camera's loskoppelen van de opnamemodus…")
        resp = self.post_simple("/api/list-files")
        if resp is not None:
            self.format_check_pending = True
            self.format_sync_attempted = False
            self.activity_label.setText("Formatteercontrole: bestanden op camera's oplijsten…")
        else:
            self.format_step_failed("Oplijsten kon niet gestart worden — "
                                    "is de cameraserver bezig?")

    def finish_format_check(self):
        """Runs after /api/list-files completes (and again after an auto-sync):
        a camera clip counts as safe if it is on the remote drive OR still held
        in local staging; auto-sync anything in neither, format when everything
        is verified."""
        self.format_step(S_COMPARE, "Cameralijst ophalen…")
        try:
            resp = api_request(self.cfg["api_url"], "/api/files", timeout=10)
            files = resp.get("files") or {}
            assert files.get("cameras")
        except Exception as e:
            self.format_step_failed(f"Kon de camerabestanden niet oplijsten ({e}).")
            self.notify(f"Formatteercontrole mislukt: kon camerabestanden niet oplijsten ({e})")
            return

        cam_clips = sorted({f for names in files["cameras"].values() for f in names
                            if f.upper().endswith(".MP4")})
        by_date = {}
        undated = []
        for f in cam_clips:
            day = clip_date(f)
            (by_date.setdefault(day, []) if day else undated).append(f)

        self.verify_output.setPlainText(
            f"Formatteercontrole: {len(cam_clips)} cameraclips vergelijken met "
            "de schijf en lokale staging…")
        QApplication.processEvents()

        self.format_step(S_COMPARE, f"{len(cam_clips)} cameraclips vergelijken met de "
                                    "onderzoeksschijf en de lokale opslag…")
        QApplication.processEvents()

        local = list_local_mp4s(self.cfg)
        missing = []
        on_drive = 0
        local_only = []
        for day, clips in sorted(by_date.items()):
            try:
                present, _ = list_remote_mp4s(self.cfg, day)
            except Exception as e:
                self.format_step_failed(f"Kon de schijfbestanden voor {day} niet oplijsten ({e}).")
                self.notify(f"Formatteercontrole mislukt: kon schijfbestanden voor {day} niet oplijsten ({e})")
                return
            for f in clips:
                if f in present:
                    on_drive += 1
                elif f in local:
                    local_only.append(f)   # nog niet geüpload, maar wel lokaal veilig
                else:
                    missing.append(f)

        report = [f"Cameraclips: {len(cam_clips)} over {len(by_date)} datum(s)",
                  f"Op onderzoeksschijf: {on_drive}"]
        if local_only:
            report.append(f"Alleen in lokale staging (telt als veilig): {len(local_only)}")
        if undated:
            report.append(f"Overgeslagen (geen datum in naam): {len(undated)}")

        if missing:
            report.append(f"Ontbreekt op schijf EN lokaal: {len(missing)}")
            report.extend(f"   {f}" for f in missing[:40])
            if len(missing) > 40:
                report.append(f"   … en nog {len(missing) - 40} meer")
            if self.format_sync_attempted:
                report.append("❌ Nog steeds ontbrekend na synchronisatie — formatteren GEANNULEERD.")
                self.verify_output.setPlainText("\n".join(report))
                self.format_step_failed(
                    f"{len(missing)} clip(s) ontbreken nog na synchronisatie — "
                    "formatteren geannuleerd. Er is niets gewist.")
                self.notify(f"Formatteren geannuleerd: {len(missing)} clip(s) ontbreken nog na synchronisatie.")
                return
            report.append("→ Ontbrekende clips worden nu automatisch gesynchroniseerd; formatteren volgt na verificatie.")
            self.verify_output.setPlainText("\n".join(report))
            self.format_flow.set_state(S_SYNC, StepFlow.PENDING)  # it is needed after all
            self.format_step(S_SYNC, f"{len(missing)} clip(s) staan nog nergens veilig — "
                                     "eerst downloaden en uploaden. Dit kan lang duren.")
            self.notify(f"{len(missing)} clip(s) staan nog niet op de schijf — automatisch synchroniseren, "
                        "formatteren volgt.")
            self.format_sync_attempted = True
            self.start_sync(format_after=True)
            return

        report.append("✅ Elke cameraclip staat op de onderzoeksschijf of in lokale "
                      "staging. Formatteren…")
        self.verify_output.setPlainText("\n".join(report))
        summary = f"{on_drive} op de onderzoeksschijf"
        if local_only:
            summary += f", {len(local_only)} lokaal in staging"
        self.format_step(S_FORMAT, f"Alle {len(cam_clips)} clips staan veilig "
                                   f"({summary}). Slot 1 wordt nu gewist…")
        self.do_format(len(cam_clips), len(local_only))

    def do_format(self, verified_count, local_only_count=0):
        """Format slot 1 on a worker thread — /api/format blocks until every
        camera is done, which would otherwise freeze the window mid-progress."""
        self.format_thread = FormatThread(self.cfg)
        self.format_thread.done.connect(
            lambda resp: self.on_format_done(resp, verified_count, local_only_count))
        self.format_thread.failed.connect(self.on_format_failed)
        self.format_thread.start()

    def on_format_done(self, resp, verified_count, local_only_count):
        if isinstance(resp, dict) and resp.get("error"):
            self.on_format_failed(resp["error"])
            return
        ok, failed = resp.get("ok", 0), resp.get("failed", 0)
        extra = (f", waarvan {local_only_count} nog alleen lokaal"
                 if local_only_count else "")
        if failed:
            self.format_step_failed(
                f"{ok} camera('s) geformatteerd, {failed} mislukt.")
        else:
            self.format_step(S_DONE, f"Klaar — slot 1 gewist op {ok} camera('s). "
                                     f"{verified_count} clips waren geverifieerd{extra}.")
            self.format_flow.finish_all()
            self.format_flow_active = False
        self.notify(f"Formatteren voltooid ({verified_count} clips geverifieerd{extra}): "
                    f"{ok} camera('s) ok, {failed} mislukt.")

    def on_format_failed(self, err):
        self.format_step_failed(f"Formatteren mislukt: {err}")
        self.notify(f"Formatteren mislukt: {err}")

    def open_studio_page(self):
        """Manual fallback for the automatic Chrome launch."""
        url = self.cfg.get("studio_url")
        if not url:
            self.notify("Geen studio_url ingesteld in config.json.")
            return
        self.chrome_launched = True  # don't auto-open a second tab later
        if studio_page_already_open(url):
            subprocess.Popen(["open", "-a", "Google Chrome"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.notify("Studiopagina is al open — Chrome naar voren gehaald.")
        else:
            subprocess.Popen(["open", "-a", "Google Chrome", url],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.notify("Studiopagina openen in Chrome.")

    def notify(self, msg):
        """Non-modal notification: activity line + status bar."""
        self.activity_label.setText(msg)
        self.statusBar().showMessage(msg, 30000)

    def sync_files(self):
        """Three-step sync: cameras -> staging inbox (USB), sort clips into
        per-recording-date folders by filename date, then upload each date
        folder to its matching remote folder. The download deliberately does
        NOT go through the FUSE mount: the mount reports the personal WebDAV
        quota (full) as free space, which makes the Sony SDK reject every file."""
        inbox = str(Path(self.cfg["staging_dir"]) / "inbox")
        answer = QMessageBox.question(
            self, "Videobestanden synchroniseren",
            f"1. Alle clips van alle camera's downloaden naar:\n{inbox}\n"
            "2. Clips sorteren in datummappen op basis van hun bestandsnaamdatum\n"
            f"3. Elke datum uploaden naar {self.cfg['rclone_remote_root']}/{{datum}}/{self.cfg['download_subdir']}\n\n"
            "Camera's kunnen tijdens stap 1 niet opnemen. Doorgaan?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        self.start_sync()

    def start_sync(self, format_after=False):
        """Kick off download -> sort -> upload without user interaction."""
        inbox = str(Path(self.cfg["staging_dir"]) / "inbox")
        resp = self.post_simple("/api/download", {"path": inbox})
        if resp is not None:
            self.sync_in_progress = True
            self.format_after_sync = format_after
        elif format_after:
            self.format_step_failed("Kon de synchronisatie-download niet starten — "
                                    "formatteren geannuleerd. Er is niets gewist.")
            self.notify("Formatteren geannuleerd: kon de synchronisatie-download niet starten.")

    def catch_up_inbox(self):
        """Recover clips downloaded but never sorted/uploaded — e.g. a download
        that finished while this app was not running. Safe to call at startup."""
        if self.last_status and (self.last_status.get("downloading")
                                 or self.last_status.get("listing")):
            return  # a transfer is active; its completion handler will sort+upload
        inbox = Path(self.cfg["staging_dir"]) / "inbox"
        try:
            has_clips = inbox.is_dir() and any(
                p.suffix.upper() == ".MP4" for p in inbox.iterdir())
        except OSError:
            has_clips = False
        if has_clips:
            self.notify("Niet-gesynchroniseerde clips gevonden in de inbox van een "
                        "vorige sessie — sorteren en uploaden naar de onderzoeksschijf…")
            self.start_upload()

    def start_upload(self):
        if hasattr(self, "upload_thread") and self.upload_thread.isRunning():
            return  # an upload is already running; don't start a second
        moved, leftover = sort_inbox(self.cfg)
        resort_date_folders(self.cfg)
        if leftover:
            self.notify("Niet-gesorteerde bestanden in de staging-inbox (geen datum in "
                        "naam of grootteconflict): " + ", ".join(leftover[:10]))
        days = staged_dates(self.cfg)
        if not days:
            self.activity_label.setText("Niets om te uploaden (staging is leeg).")
            return
        summary = ", ".join(f"{d}: {n}" for d, n in sorted(moved.items())) or "geen nieuwe clips"
        self.activity_label.setText(f"Gesorteerd ({summary}). {len(days)} datummap(pen) uploaden…")
        self.check_captures()  # show "downloaded" status right away, not after upload
        self.btn_sync.setEnabled(False)
        self.upload_thread = UploadThread(self.cfg, days)
        self.upload_thread.progress.connect(self.activity_label.setText)
        self.upload_thread.finished_ok.connect(self.on_upload_done)
        self.upload_thread.failed.connect(self.on_upload_failed)
        self.upload_thread.start()

    def on_upload_done(self, days):
        self.notify(f"Upload voltooid: {', '.join(days)}")
        self.btn_sync.setEnabled(True)
        self.scan_date_counts()
        self.check_captures()
        if self.format_after_sync:
            self.format_after_sync = False
            self.finish_format_check()  # re-verify, then format
        else:
            self.run_verify()

    def on_upload_failed(self, err):
        self.btn_sync.setEnabled(True)
        if self.format_after_sync:
            self.format_after_sync = False
            self.format_step_failed(f"Upload mislukt: {err}. Formatteren geannuleerd — er is "
                                    "niets gewist; de bestanden staan veilig in staging.")
            self.notify(f"Formatteren geannuleerd — upload mislukt: {err}. Bestanden staan "
                        "veilig in staging; los het probleem op en druk opnieuw op Formatteren.")
        else:
            self.notify(f"Upload MISLUKT: {err}. Bestanden staan veilig in staging; druk "
                        "opnieuw op Synchroniseren om het te herproberen (reeds geüploade clips worden overgeslagen).")

    # ------------------------------------------------------ log / verify

    def refresh_capture_table(self, present=None, local=None):
        """present: files on the research drive; local: files on local disk
        (staging). None = reuse the last check's result."""
        if present is not None:
            self.drive_files_today = present
        if local is not None:
            self.local_files_today = local
        present = self.drive_files_today
        local = self.local_files_today or set()
        entries = self.capture_log.load(str(date.today()))
        self.capture_table.setRowCount(len(entries))
        for row, e in enumerate(entries):
            clips = sorted(e["clips"].values())
            self.capture_table.setItem(row, 0, QTableWidgetItem(e["time"].split("T")[-1]))
            n = len(clips)
            on_drive = sum(1 for c in clips if present and c in present)
            on_disk = sum(1 for c in clips if c in local or (present and c in present))
            if present is None and not local:
                mark = QTableWidgetItem("…")
                mark.setForeground(QColor("gray"))
            elif on_drive == n:
                mark = QTableWidgetItem("✓ gesynchroniseerd")
                mark.setForeground(QColor("#2e7d32"))
            elif on_disk == n:
                uploading = (hasattr(self, "upload_thread")
                             and self.upload_thread.isRunning())
                mark = QTableWidgetItem("⬇ gedownload — synchroniseren…" if uploading
                                        else "⬇ gedownload — synchronisatie in wachtrij")
                mark.setForeground(QColor("#1565c0"))
            else:
                mark = QTableWidgetItem(f"✗ {max(on_disk, on_drive)}/{n}")
                mark.setForeground(QColor("red"))
            mark.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self.capture_table.setItem(row, 1, mark)
            self.capture_table.setItem(row, 2, QTableWidgetItem(", ".join(clips)))
        self.capture_table.resizeColumnToContents(0)
        self.capture_table.resizeColumnToContents(1)

    def check_captures(self):
        """Verify today's capture entries against the remote date folder."""
        if hasattr(self, "capture_check") and self.capture_check.isRunning():
            return
        self.capture_check = CaptureCheckThread(self.cfg, str(date.today()))
        self.capture_check.result.connect(self.on_capture_check)
        self.capture_check.start()

    def on_capture_check(self, day, present, local):
        self.refresh_capture_table(present, local)
        if present is None:
            return
        # report synced files to the server so the web app sees the same status
        expected = set(self.capture_log.expected_files(day))
        synced = sorted(expected & present)
        if synced:
            try:
                api_request(self.cfg["api_url"], "/api/captures/synced", "POST",
                            {"date": day, "files": synced}, timeout=10)
            except Exception:
                pass  # server may be busy; next check will retry

    def run_verify(self):
        day = str(date.today())
        expected = self.capture_log.expected_files(day)
        self.verify_output.setPlainText("Externe bestanden oplijsten…")
        QApplication.processEvents()
        result = verify_date(self.cfg, day, expected)
        lines = [
            f"Locatie: {result['date_dir']}",
            f"Verwacht (gevolgde opnames): {len(result['expected'])}",
            f".MP4-bestanden op schijf:    {result['present_count']}",
            f"Overeenkomend:               {len(result['matched'])}",
            "",
        ]
        if result["missing"]:
            lines.append(f"❌ ONTBREEKT ({len(result['missing'])}):")
            lines.extend(f"   {f}" for f in result["missing"])
        elif result["expected"]:
            lines.append("✅ Alle gevolgde opnames staan op de schijf.")
        else:
            lines.append("Geen gevolgde opnames voor deze datum (opnamelog is leeg).")
        if result["untracked"]:
            lines.append("")
            lines.append(f"Op schijf maar niet in opnamelog ({len(result['untracked'])}):")
            lines.extend(f"   {f}" for f in result["untracked"][:30])
            if len(result["untracked"]) > 30:
                lines.append(f"   … en nog {len(result['untracked']) - 30} meer")
        self.verify_output.setPlainText("\n".join(lines))

    def scan_date_counts(self):
        """Count MP4s per camera prefix for the last 5 recording-date folders
        on the mount (mount listing only — does not enumerate the whole remote)."""
        if hasattr(self, "count_thread") and self.count_thread.isRunning():
            return
        self.date_table.setRowCount(0)
        self.count_thread = DateCountThread(self.cfg, limit=5)
        self.count_thread.row_ready.connect(self.on_date_row)
        self.count_thread.finished_all.connect(self.on_date_scan_done)
        self.count_thread.start()

    def on_date_row(self, day, counts, total):
        row = self.date_table.rowCount()
        self.date_table.insertRow(row)
        other = total - sum(counts.get(p, 0) for p in CAM_PREFIXES)
        values = [day] + [str(counts.get(p, 0)) for p in CAM_PREFIXES] \
                 + [str(other), str(total)]
        per_cam = [counts.get(p, 0) for p in CAM_PREFIXES if counts.get(p, 0) > 0]
        mismatch = len(set(per_cam)) > 1  # cameras disagree -> likely missing clips
        for col, v in enumerate(values):
            item = QTableWidgetItem(v)
            if col > 0:
                item.setTextAlignment(Qt.AlignmentFlag.AlignRight |
                                      Qt.AlignmentFlag.AlignVCenter)
            if mismatch and 1 <= col <= len(CAM_PREFIXES):
                item.setForeground(QColor("red"))
            self.date_table.setItem(row, col, item)

    def on_date_scan_done(self, n):
        self.activity_label.setText(f"Datumscan voltooid: {n} datummap(pen).")

    # --------------------------------------------------- server process

    def restart_server(self):
        answer = QMessageBox.question(
            self, "Sony SDK herstarten",
            "fx30MultiRecord afsluiten en opnieuw starten?\n"
            "Camera's verbinden automatisch opnieuw (duurt ~30 s; als ze in een "
            "slechte staat zijn achtergelaten kan een uit-/aanschakeling nog nodig zijn).",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        self.restart_server_now()

    def restart_server_now(self):
        """Hard restart without confirmation (used by the banner buttons)."""
        subprocess.run(["pkill", "-f", "fx30MultiRecord"], check=False)
        self.notify("Cameraserver herstarten…")
        QTimer.singleShot(2000, self._launch_server)

    def _launch_server(self):
        binary = self.cfg["server_binary"]
        dl_path = str(Path(self.cfg["staging_dir"]) / "inbox")
        try:
            self.server_proc = subprocess.Popen(
                [binary, "--port", str(self.cfg["server_port"]),
                 "--download-path", dl_path,
                 "--capture-log-dir", str(LOG_DIR)],
                cwd=str(Path(binary).parent),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            self.activity_label.setText("Server opnieuw gestart, wachten op camera's…")
        except Exception as e:
            QMessageBox.critical(self, "Starten mislukt", str(e))

    def closeEvent(self, event):
        self.poller.stop()
        self.poller.wait(3000)
        if hasattr(self, "count_thread") and self.count_thread.isRunning():
            self.count_thread.stop()
            self.count_thread.wait(3000)
        event.accept()


def main():
    cfg = load_config()
    app = QApplication(sys.argv)
    icon_path = APP_DIR / "sc_icon.png"
    if icon_path.exists():
        app.setWindowIcon(QIcon(str(icon_path)))
    win = MainWindow(cfg)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
