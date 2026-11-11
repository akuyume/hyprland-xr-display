#!/usr/bin/env python3
"""Workspace migration recovery and startup ordering requirements."""
import copy
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

loader = importlib.machinery.SourceFileLoader('xr_desktop', str(Path(__file__).resolve().parents[1] / 'scripts/xr-display'))
spec = importlib.util.spec_from_loader(loader.name, loader)
xr = importlib.util.module_from_spec(spec)
loader.exec_module(xr)


def monitor(name, ident, width=1920, height=1080, x=0):
    return dict(name=name, id=ident, width=width, height=height, scale=1, x=x, y=0,
                make='Desktop', model='Test', serial=name, physicalWidth=500,
                physicalHeight=300, activeWorkspace={'id': ident + 1, 'name': str(ident + 1)})


class DesktopTransferTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        env = patch.dict(os.environ, XDG_STATE_HOME=temp.name, XDG_RUNTIME_DIR=temp.name)
        env.start()
        self.addCleanup(env.stop)
        self.monitors = [monitor('DP-1', 0, x=-1920), monitor('DP-2', 1, 2560, 1440)]
        self.events = []
        for key, value in (('configuration', lambda: {'view_mode': 'windows'}),
                           ('inventory', lambda: []), ('query', self.query),
                           ('create_desktop_output', self.create),
                           ('dispatch', lambda expression: self.events.append(('dispatch', expression)))):
            replacement = patch.object(xr, key, value)
            replacement.start()
            self.addCleanup(replacement.stop)

    def query(self, name):
        if name == 'monitors': return copy.deepcopy(self.monitors)
        if name == 'activewindow': return {}
        if name == 'clients': return []
        if name == 'workspaces':
            return [m['activeWorkspace'] | {'monitorID': m['id']} for m in self.monitors]
        raise AssertionError(name)

    def create(self, name, original):
        self.events.append(('create', name))
        clone = monitor(name, len(self.monitors) + 10, original['width'], original['height'])
        clone.update(make='', model='', physicalWidth=0, physicalHeight=0)
        self.monitors.append(clone)
        return clone

    def test_all_outputs_configured_before_any_workspace_moves(self):
        generation = xr.gather_desktop()
        self.assertTrue(generation)
        self.assertEqual([event[0] for event in self.events[:2]], ['create', 'create'])
        self.assertFalse(any(event[0] == 'create' for event in self.events[2:]))
        state = xr.window_state()['desktop']
        self.assertEqual(state['anchor']['name'], 'DP-2')
        self.assertEqual(state['phase'], 'ready')
        self.assertEqual({output['workspace']['id'] for output in state['outputs'].values()}, {1, 2})
        self.assertEqual(xr.gather_desktop(), generation)

    def test_prepare_preserves_visible_workspace_before_output_reconfiguration(self):
        self.monitors[0]['activeWorkspace'] = {'id': 9, 'name': '9'}
        with patch.object(xr, 'cleanup'):
            xr.prepare()
        self.monitors[0]['activeWorkspace'] = {'id': 1, 'name': '1'}
        xr.gather_desktop()
        saved = xr.window_state()['desktop']['outputs']['XR-VIRTUAL-desktop-0']
        self.assertEqual(saved['workspace']['id'], 9)

    def test_partial_create_failure_keeps_recovery_record(self):
        def partial(name, original):
            if self.events:
                raise RuntimeError('failure')
            return self.create(name, original)
        with patch.object(xr, 'create_desktop_output', side_effect=partial):
            with self.assertRaisesRegex(RuntimeError, 'failure'):
                xr.gather_desktop()
        saved = xr.window_state()['desktop']
        self.assertEqual(saved['phase'], 'preparing')
        self.assertEqual(len(saved['outputs']), 2)
        self.assertIsNotNone(saved['outputs']['XR-VIRTUAL-desktop-0']['id'])

    def test_missing_original_monitor_keeps_its_clone_for_recovery(self):
        xr.gather_desktop()
        self.monitors = [m for m in self.monitors if m['name'] != 'DP-1']
        with patch.object(xr, 'run'):
            with self.assertRaisesRegex(RuntimeError, 'Original monitor DP-1 unavailable'):
                xr.restore_desktop()
        self.assertEqual(set(xr.window_state()['desktop']['outputs']), {'XR-VIRTUAL-desktop-0'})

    def test_replaced_clone_is_not_destroyed(self):
        xr.gather_desktop()
        self.monitors[-1]['id'] += 100
        with patch.object(xr, 'run') as command:
            with self.assertRaisesRegex(RuntimeError, 'unrelated output'):
                xr.restore_desktop()
            destroyed = [call.args[-1] for call in command.call_args_list]
            self.assertNotIn('XR-VIRTUAL-desktop-1', destroyed)

    def test_native_coordinates_map_back_to_original_monitor(self):
        xr.gather_desktop()
        state = xr.window_state()
        clone = next(m for m in self.monitors if m['name'] == 'XR-VIRTUAL-desktop-0')
        clone['x'] = 9000
        at, anchor = xr.scene_geometry({'monitor': clone['id'], 'at': [9010, 40], 'size': [800, 600]})
        self.assertEqual(at, [-1910, 40])
        self.assertEqual(anchor['width'], 2560)
        self.assertEqual(anchor['height'], 1440)

    def test_power_off_waits_for_current_desktop_capture_generation(self):
        properties = [{'StatusText': 'XR display ready', 'ActiveState': 'activating'},
                      {'StatusText': 'XR desktop ready:current', 'ActiveState': 'activating'}]
        with patch.object(xr, 'service_properties', side_effect=properties), patch.object(xr, 'gather_desktop', return_value='current'), patch.object(xr, 'monitors_off') as off:
            xr.enter()
            off.assert_called_once()

    def test_preview_does_not_transfer_workspaces(self):
        with patch.object(xr, 'service_properties', return_value={'StatusText': 'XR preview ready'}), patch.object(xr, 'gather_desktop') as gather, patch.object(xr, 'monitors_off') as off:
            xr.enter()
            gather.assert_not_called()
            off.assert_not_called()


if __name__ == '__main__':
    unittest.main()
