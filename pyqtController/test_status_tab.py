"""Tests for the Status tab of the FX30 controller.

Run headless, from this folder:
    QT_QPA_PLATFORM=offscreen python -m pytest test_status_tab.py
Nothing here starts the camera server, Chrome or rclone.
"""

import json
import os
import sys
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent))

import pytest
from PyQt6.QtCore import QCoreApplication
from PyQt6.QtWidgets import QApplication, QTabWidget

import fx30_controller as fx

HELP = "https://example.org/studio/troubleshooting/#studio-"

MIXED = {
    "generated_at": "2026-10-07T14:02:11+02:00",
    "host": "signlabs-mini",
    "overall": "fail",
    "checks": [
        {"id": "research_drive", "title": "Research drive", "status": "ok",
         "detail": "mounted, 1.0 TB free", "action": "",
         "help_url": HELP + "research-drive"},
        {"id": "cache_disk", "title": "Cache disk", "status": "warn",
         "detail": "42 GB free", "action": "Empty the Resolve cache",
         "help_url": HELP + "cache-disk"},
        {"id": "resolve", "title": "DaVinci Resolve", "status": "fail",
         "detail": "last batch failed at 13:40", "action": "Restart Resolve",
         "help_url": HELP + "resolve"},
        {"id": "qr_screen", "title": "QR screen", "status": "unknown",
         "detail": "only the controller app knows", "action": "",
         "help_url": HELP + "qr-screen"},
    ],
}


@pytest.fixture(scope="session")
def app():
    return QApplication.instance() or QApplication([])


def pump(condition, timeout=10.0):
    """Process Qt events until condition() is true."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        QCoreApplication.processEvents()
        if condition():
            return True
        time.sleep(0.01)
    return False


def fake_health(tmp_path, body, name="health.py"):
    """A tiny stand-in for tools/health.py; returns the command to run it."""
    script = tmp_path / name
    script.write_text(body)
    return [sys.executable, str(script), "--json"]


def by_title(tab):
    return {r.title_label.text(): r for r in tab.rows}


# ------------------------------------------------------------- rendering

def test_rows_for_every_status(app):
    tab = fx.StatusTab()
    tab.show_result(MIXED)
    rows = by_title(tab)
    assert list(rows) == ["Research drive", "Cache disk", "DaVinci Resolve", "QR screen"]

    ok = rows["Research drive"]
    assert ok.status == "ok"
    assert ok.detail_label.text() == "mounted, 1.0 TB free"
    assert ok.action_label is None and ok.help_button is None

    warn = rows["Cache disk"]
    assert warn.status == "warn"
    assert warn.action_label.text() == "→ Empty the Resolve cache"
    assert warn.help_button.text() == "Hulp"

    fail = rows["DaVinci Resolve"]
    assert fail.status == "fail"
    assert fail.action_label.text() == "→ Restart Resolve"
    assert fail.help_button is not None

    # the app was not told about the QR screen: the row stays as reported
    unknown = rows["QR screen"]
    assert unknown.status == "unknown"
    assert unknown.action_label is None

    assert tab.overall == "fail"
    assert tab.summary_label.text() == "1 probleem · 1 waarschuwing"
    assert "signlabs-mini" in tab.checked_label.text()
    assert "laatst gecontroleerd" in tab.checked_label.text()
    assert tab.btn_refresh.isEnabled()


def test_all_ok(app):
    tab = fx.StatusTab()
    tab.show_result({"overall": "ok", "checks": [MIXED["checks"][0]]})
    assert tab.overall == "ok"
    assert tab.summary_label.text() == "Alles in orde"


def test_odd_input_does_not_crash(app):
    tab = fx.StatusTab()
    tab.show_result({"overall": "purple", "checks": [
        "not a dict", {}, {"id": "x", "status": "bogus", "detail": None},
        {"id": "network", "title": "<b>Network</b>", "status": "warn"}]})
    assert [r.status for r in tab.rows] == ["unknown", "unknown", "warn"]
    assert tab.rows[0].title_label.text() == "?"
    assert tab.rows[1].title_label.text() == "x"
    assert tab.overall == "warn"


def test_help_button_opens_help_url(app, monkeypatch):
    opened = []
    monkeypatch.setattr(fx, "open_help_url", opened.append)
    tab = fx.StatusTab()
    tab.show_result(MIXED)
    by_title(tab)["DaVinci Resolve"].help_button.click()
    assert opened == [HELP + "resolve"]


def test_help_url_must_be_a_web_link(app, monkeypatch):
    opened = []
    monkeypatch.setattr(fx.QDesktopServices, "openUrl", opened.append)
    fx.open_help_url("file:///etc/passwd")
    assert opened == []
    fx.open_help_url("https://example.org/x")
    assert len(opened) == 1
    row = fx.HealthRow({"status": "fail", "title": "t", "help_url": "file:///etc/passwd"})
    assert row.help_button is None


# ------------------------------------------------------ tab indicator

def test_overall_shows_on_the_tab(app):
    tabs = QTabWidget()
    tabs.addTab(fx.QWidget(), "Camera's")
    tab = fx.StatusTab()
    tab.add_to_tabs(tabs)
    seen = []
    tab.overall_changed.connect(seen.append)
    assert tabs.tabText(1) == "Status"

    tab.show_result(MIXED)
    assert tabs.tabText(1) == "Status ⚠"
    assert tabs.tabBar().tabTextColor(1) == fx.HEALTH_COLORS["fail"]
    assert not tabs.tabIcon(1).isNull()
    assert tabs.currentIndex() == 0          # never steals the first tab

    tab.show_result({"overall": "warn", "checks": [MIXED["checks"][1]]})
    assert tabs.tabText(1) == "Status ⚠"
    assert tabs.tabBar().tabTextColor(1) == fx.HEALTH_COLORS["warn"]

    tab.show_result({"overall": "ok", "checks": [MIXED["checks"][0]]})
    assert tabs.tabText(1) == "Status"

    tab.show_error("boom")
    assert tabs.tabText(1) == "Status ⚠"
    assert seen == ["fail", "warn", "ok", "error"]


# -------------------------------------------------------- qr override

def test_qr_screen_is_filled_in_by_the_app(app):
    data = {"overall": "ok", "checks": [MIXED["checks"][0], MIXED["checks"][3]]}
    tab = fx.StatusTab()
    tab.set_qr_open(False)
    tab.show_result(data)
    qr = by_title(tab)["QR screen"]
    assert qr.status == "fail"
    assert "QR-scherm openen" in qr.action_label.text()
    assert qr.help_url == HELP + "qr-screen" and qr.help_button is not None
    assert tab.overall == "fail"

    # the kiosk opens: the row turns green at once, without a new health run
    tab.set_qr_open(True)
    qr = by_title(tab)["QR screen"]
    assert qr.status == "ok" and qr.action_label is None
    assert tab.overall == "ok"
    assert data["checks"][1]["status"] == "unknown"      # input left alone


def test_qr_screen_is_left_alone_when_health_knows(app):
    check = dict(MIXED["checks"][3], status="warn", detail="half open")
    tab = fx.StatusTab()
    tab.set_qr_open(True)
    tab.show_result({"overall": "warn", "checks": [check]})
    assert tab.rows[0].status == "warn"
    assert tab.overall == "warn"


# ------------------------------------------------------------- runner

def test_health_command_from_config():
    assert fx.health_command({}) == [
        "/usr/bin/python3", "/Users/signlab/drs/tools/health.py", "--json"]
    assert fx.health_command({"drs_dir": "/opt/drs"})[1] == "/opt/drs/tools/health.py"
    assert fx.health_command({"health_command": ["a", "b"], "drs_dir": "/x"}) == ["a", "b"]


def test_run_health_ok(tmp_path):
    cmd = fake_health(tmp_path, f"import json\nprint(json.dumps({MIXED!r}))\n")
    assert fx.run_health(cmd, timeout=20) == MIXED


def test_run_health_missing_script(tmp_path):
    cmd = [sys.executable, str(tmp_path / "tools" / "health.py"), "--json"]
    with pytest.raises(fx.HealthError) as e:
        fx.run_health(cmd, timeout=20)
    assert str(e.value) == f"health.py niet gevonden in {tmp_path / 'tools'}"


def test_run_health_missing_interpreter(tmp_path):
    with pytest.raises(fx.HealthError, match="niet gevonden"):
        fx.run_health([str(tmp_path / "no-python"), "--json"], timeout=20)


def test_run_health_timeout(tmp_path):
    cmd = fake_health(tmp_path, "import time\ntime.sleep(60)\n")
    start = time.monotonic()
    with pytest.raises(fx.HealthError, match="geen antwoord binnen 1 s"):
        fx.run_health(cmd, timeout=1)
    assert time.monotonic() - start < 10


def test_run_health_non_zero_exit(tmp_path):
    cmd = fake_health(tmp_path, "import sys\nsys.stderr.write('disk on fire\\n')\nsys.exit(3)\n")
    with pytest.raises(fx.HealthError) as e:
        fx.run_health(cmd, timeout=20)
    assert str(e.value) == "afgesloten met code 3: disk on fire"


def test_run_health_invalid_json(tmp_path):
    cmd = fake_health(tmp_path, "print('<html>not json')\n")
    with pytest.raises(fx.HealthError, match="ongeldige JSON"):
        fx.run_health(cmd, timeout=20)


def test_run_health_wrong_shape(tmp_path):
    cmd = fake_health(tmp_path, "print('[1, 2]')\n")
    with pytest.raises(fx.HealthError, match="onverwacht antwoord"):
        fx.run_health(cmd, timeout=20)


# ------------------------------------------- thread + tab, end to end

def test_tab_runs_the_health_program(app, tmp_path):
    counter = tmp_path / "runs"
    cmd = fake_health(tmp_path, (
        "import json, pathlib\n"
        f"p = pathlib.Path({str(counter)!r})\n"
        "p.write_text(p.read_text() + 'x' if p.exists() else 'x')\n"
        f"print(json.dumps({MIXED!r}))\n"))
    tab = fx.StatusTab()
    tab.start_checks(cmd, interval_s=3600, timeout_s=20)
    try:
        assert pump(lambda: len(tab.rows) == 4)
        assert tab.btn_refresh.isEnabled()
        assert counter.read_text() == "x"
        tab.btn_refresh.click()              # Vernieuwen: runs again at once
        assert not tab.btn_refresh.isEnabled()
        assert pump(lambda: counter.read_text() == "xx" and tab.btn_refresh.isEnabled())
    finally:
        tab.stop_checks()


@pytest.mark.parametrize("body, reason", [
    (None, "health.py niet gevonden in"),
    ("import time\ntime.sleep(60)\n", "geen antwoord binnen 1 s"),
    ("import sys\nsys.exit(2)\n", "afgesloten met code 2"),
    ("print('nope')\n", "ongeldige JSON"),
])
def test_tab_shows_why_the_check_could_not_run(app, tmp_path, body, reason):
    if body is None:
        cmd = fx.health_command({"drs_dir": str(tmp_path)})
    else:
        cmd = fake_health(tmp_path, body)
    tab = fx.StatusTab()
    tab.show_result(MIXED)                   # an earlier good result
    tab.start_checks(cmd, interval_s=3600, timeout_s=1)
    try:
        assert pump(lambda: tab.overall == "error")
    finally:
        tab.stop_checks()
    assert len(tab.rows) == 1
    row = tab.rows[0]
    assert row.status == "unknown"
    assert row.title_label.text() == "Statuscontrole kon niet worden uitgevoerd"
    assert reason in row.detail_label.text()
    assert tab.checked_label.text().startswith("Laatste geslaagde controle: ")
    assert tab.btn_refresh.isEnabled()


def test_stop_does_not_wait_for_a_hanging_check(app, tmp_path):
    cmd = fake_health(tmp_path, "import time\ntime.sleep(60)\n")
    tab = fx.StatusTab()
    tab.start_checks(cmd, interval_s=3600, timeout_s=30)
    thread = tab.health_thread
    assert pump(lambda: thread._proc is not None)
    start = time.monotonic()
    tab.stop_checks()
    assert time.monotonic() - start < 5
    assert thread.isFinished()


# ------------------------------------------------- the real main window

def test_main_window_starts_with_the_status_tab(app, tmp_path, monkeypatch):
    """Build the real MainWindow with the camera server, Chrome, rclone and
    the network stubbed out: the tab must not break start-up."""
    launched = []

    def no_api(*a, **k):
        raise OSError("no server in tests")

    class NoThread:
        """Stands in for the rclone worker threads."""
        def __init__(self, *a, **k):
            self.result = self.row_ready = self.finished_all = self
        def connect(self, *_):
            pass
        def start(self):
            pass
        def isRunning(self):
            return False

    monkeypatch.setattr(fx, "LOG_DIR", tmp_path / "capture_logs")
    monkeypatch.setattr(fx, "api_request", no_api)
    monkeypatch.setattr(fx, "count_usb_cameras", lambda: 0)
    monkeypatch.setattr(fx, "DateCountThread", NoThread)
    monkeypatch.setattr(fx, "CaptureCheckThread", NoThread)
    monkeypatch.setattr(fx.MainWindow, "_launch_server", lambda self: launched.append("server"))
    monkeypatch.setattr(fx.MainWindow, "_launch_kiosk", lambda self, s: launched.append("kiosk"))
    monkeypatch.setattr(fx.MainWindow, "_kiosk_open", lambda self: False)
    monkeypatch.setattr(fx.MainWindow, "catch_up_inbox", lambda self: None)

    cfg = json.loads((Path(fx.APP_DIR) / "config.example.json").read_text())
    cfg.update(api_url="http://127.0.0.1:9", staging_dir=str(tmp_path / "staging"),
               display_url="", studio_url="",
               health_command=fake_health(
                   tmp_path, f"import json\nprint(json.dumps({MIXED!r}))\n"))
    win = fx.MainWindow(cfg)
    try:
        win.show()
        assert win.centralWidget() is win.tabs
        assert [win.tabs.tabText(i) for i in range(2)] == ["Camera's", "Status"]
        assert win.tabs.currentIndex() == 0
        assert (win.width(), win.height()) == (1150, 760)
        assert win.poller.isRunning() and win.capture_timer.isActive()

        # the health run arrives; qr_screen comes from the app (kiosk not open)
        assert pump(lambda: len(win.status_tab.rows) == 4)
        assert by_title(win.status_tab)["QR screen"].status == "fail"
        assert win.tabs.tabText(1) == "Status ⚠"
        assert win.display_status_label.text() == "QR-scherm: ○ niet open"

        # the camera banner still sits centred on the camera page, also after
        # the window was resized while the Status tab was in front
        page = win.tabs.widget(0)
        win.show_banner("test", win.STYLE_WARN)
        win.tabs.setCurrentIndex(1)
        win.resize(1300, 820)
        pump(lambda: False, 0.2)
        win.tabs.setCurrentIndex(0)
        pump(lambda: False, 0.2)
        assert win.banner.parentWidget() is page and win.banner.isVisible()
        assert win.banner.width() == int(page.width() * 0.7)
        assert abs(win.banner.x() - (page.width() - win.banner.width()) // 2) <= 1
        # the unreachable server went down the normal path (stubbed launch)
        assert pump(lambda: "onbereikbaar" in win.server_label.text())
        assert "kiosk" not in launched
    finally:
        win.close()
        pump(lambda: False, 0.1)
    assert win.status_tab.health_thread is None
    assert not win.poller.isRunning()
