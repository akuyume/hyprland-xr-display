#!/usr/bin/env python3
"""Recovery tests for XR's monitor power lifecycle; no compositor required."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

loader = importlib.machinery.SourceFileLoader("xr_display", str(Path(__file__).resolve().parents[1] / "scripts/xr-display"))
spec = importlib.util.spec_from_loader(loader.name, loader)
xr = importlib.util.module_from_spec(spec)
loader.exec_module(xr)


def monitor(name, make="Example", model="Desktop", on=True, physical=True):
    return dict(name=name, make=make, model=model, serial=name + "-serial",
                physicalWidth=500 if physical else 0,
                physicalHeight=300 if physical else 0, dpmsStatus=on)


class MonitorPowerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.env = patch.dict(os.environ, XDG_RUNTIME_DIR=self.temporary.name,
                              XDG_STATE_HOME=self.temporary.name)
        self.env.start()
        self.addCleanup(self.env.stop)
        self.path = Path(self.temporary.name) / "xr-hyprland/monitors.json"
        self.monitors = [monitor("DP-1"), monitor("DP-2", on=False),
                         monitor("DP-3", "Nreal", "Air"),
                         monitor("XR-VIRTUAL", make="", model="", physical=False)]
        self.calls = []
        self.properties = "ActiveState=activating\nSubState=start-post\nStatusText=XR display ready\n"
        self.failure = None
        for name, replacement in (("query", lambda _: self.monitors),
                                  ("configuration", lambda: {}),
                                  ("run", lambda *a, **kw: subprocess.CompletedProcess(a, 0, self.properties)),
                                  ("monitor_power", self.power)):
            mock = patch.object(xr, name, replacement)
            mock.start()
            self.addCleanup(mock.stop)

    def power(self, name, enabled):
        self.calls.append((name, enabled))
        # Recovery must already be durable when the first screen goes dark.
        if not enabled:
            self.assertIn(name, json.loads(self.path.read_text())["monitors"])
        if (name, enabled) == self.failure:
            raise RuntimeError("Simulated compositor failure")
        next(m for m in self.monitors if m["name"] == name)["dpmsStatus"] = enabled

    def saved(self):
        return json.loads(self.path.read_text())["monitors"]

    def test_only_lit_regular_physical_monitors_and_restore(self):
        xr.monitors_off()
        xr.monitors_off()  # Repeated invocation must preserve the restore point.
        self.assertEqual(self.calls, [("DP-1", False)])
        self.assertEqual(set(self.saved()), {"DP-1"})
        xr.restore_monitors()
        xr.restore_monitors()
        self.assertEqual(self.calls, [("DP-1", False), ("DP-1", True)])
        self.assertEqual(self.saved(), {})
        self.assertFalse(self.monitors[1]["dpmsStatus"])

    def test_preview_and_stopping_service_never_blank_monitors(self):
        for properties in ("ActiveState=active\nStatusText=XR preview ready\n",
                           "ActiveState=active\nStatusText=\n",
                           "ActiveState=deactivating\nStatusText=XR display ready\n"):
            self.properties = properties
            xr.monitors_off()
        self.assertEqual(self.calls, [])

    def test_missing_glasses_never_blank_monitors(self):
        self.monitors = [m for m in self.monitors if m["name"] != "DP-3"]
        with self.assertRaisesRegex(RuntimeError, "glasses unavailable"):
            xr.monitors_off()
        self.assertEqual(self.calls, [])

    def test_failure_midway_rolls_back_completed_changes(self):
        self.monitors.append(monitor("HDMI-A-1"))
        self.failure = ("HDMI-A-1", False)
        with self.assertRaisesRegex(RuntimeError, "Simulated"):
            xr.monitors_off()
        self.assertIn(("DP-1", True), self.calls)
        self.assertTrue(self.monitors[0]["dpmsStatus"])
        self.assertEqual(self.saved(), {})

    def test_restore_failure_does_not_block_other_monitors_and_can_retry(self):
        self.monitors.append(monitor("HDMI-A-1"))
        xr.monitors_off()
        self.failure = ("DP-1", True)
        with self.assertRaisesRegex(RuntimeError, "Simulated"):
            xr.restore_monitors()
        self.assertIn(("HDMI-A-1", True), self.calls)
        self.assertEqual(set(self.saved()), {"DP-1"})
        self.failure = None
        xr.restore_monitors()
        self.assertEqual(self.saved(), {})

    def test_replacement_display_is_not_woken(self):
        xr.monitors_off()
        self.monitors[0]["serial"] = "replacement"
        xr.restore_monitors()
        self.assertNotIn(("DP-1", True), self.calls)
        self.assertEqual(set(self.saved()), {"DP-1"})

    def test_window_restore_failure_still_wakes_monitors(self):
        xr.monitors_off()
        with patch.object(xr, "restore_windows", side_effect=RuntimeError("Bad window state")):
            with self.assertRaisesRegex(RuntimeError, "Bad window state"):
                xr.cleanup()
        self.assertTrue(self.monitors[0]["dpmsStatus"])
        self.assertEqual(self.saved(), {})

    def test_monitor_failure_still_attempts_window_restore(self):
        xr.monitors_off()
        self.failure = ("DP-1", True)
        with patch.object(xr, "restore_windows") as restore:
            with self.assertRaisesRegex(RuntimeError, "Simulated"):
                xr.cleanup()
            restore.assert_called_once()


if __name__ == "__main__":
    unittest.main()
