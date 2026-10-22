import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
import bbport_launcher_win as core


class QuietUpdateCheckTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        cache = Path(self.directory.name) / 'update-check.json'
        for name, value in {'CONFIG_DIR': cache.parent, 'UPDATE_CACHE': cache,
                            'CURRENT_RELEASE_TAG': 'qml-launcher-preview-8', 'VERSION': '1.6.12'}.items():
            patcher = patch.object(core, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.cache = cache

    def reply(self, tag):
        release = {'tag_name': tag, 'html_url': 'https://example.invalid/page',
                   'assets': [{'name': 'bbport-windows.zip', 'browser_download_url': 'https://example.invalid/zip'}]}

        class Response:
            def __enter__(self): return self
            def __exit__(self, *args): return False
            def read(self, *args): return json.dumps(release).encode()
        return patch.object(core.urllib.request, 'urlopen', return_value=Response())

    def test_newer_release_is_remembered_for_the_launcher(self):
        with self.reply('qml-launcher-preview-9'):
            self.assertTrue(core.quiet_update_check({'check_updates': True}, now=1000))
        self.assertEqual(core.remembered_update(), ('qml-launcher-preview-9', 'https://example.invalid/zip',
                                                    'https://example.invalid/page'))

    def test_current_or_older_release_is_not_offered(self):
        with self.reply('qml-launcher-preview-8'):
            core.quiet_update_check({'check_updates': True})
        self.assertIsNone(core.remembered_update())

    def test_switched_off_and_throttled(self):
        with self.reply('qml-launcher-preview-9') as urlopen:
            self.assertFalse(core.quiet_update_check({'check_updates': False}))
            self.assertTrue(core.quiet_update_check({'check_updates': True}, now=core.time.time()))
            self.assertFalse(core.quiet_update_check({'check_updates': True}, now=core.time.time() + 60))
            self.assertTrue(core.quiet_update_check({'check_updates': True},
                                                    now=core.time.time() + core.QUIET_CHECK_INTERVAL + 1))
        self.assertEqual(urlopen.call_count, 2)

    def test_update_banner_does_not_require_removed_sidebar_widget(self):
        # The sidebar is a Canvas; there is no side_note widget to pack before.
        # A remembered release must not crash Launcher.__init__ on startup.
        class Widget:
            def __init__(self, master=None):
                self.master = master
                self.packed = []
                self.configured = []
            def pack(self, **kwargs):
                self.packed.append(kwargs)
            def configure(self, **kwargs):
                self.configured.append(kwargs)

        footer = object()
        status = Widget(footer)
        frames = []
        buttons = []

        class FakeTk:
            @staticmethod
            def Frame(master, **_kwargs):
                frame = Widget(master)
                frames.append(frame)
                return frame

        class FakeTtk:
            @staticmethod
            def Button(master, **kwargs):
                button = Widget(master)
                button.command = kwargs['command']
                buttons.append(button)
                return button

        launcher = core.Launcher.__new__(core.Launcher)
        launcher.tk = FakeTk()
        launcher.ttk = FakeTtk()
        launcher.status = status
        launcher.process = None
        launcher.update_box = None
        launcher.px = lambda size: size
        # Intentionally no launcher.side_note: the current UI has no such widget.
        launcher.offer_update('v2', 'https://example.invalid/zip', 'https://example.invalid/news')
        self.assertIs(launcher.update_box.master, footer)
        self.assertIs(launcher.update_label, status)
        self.assertEqual(len(buttons), 2)
        self.assertEqual(launcher.update_box.packed[0]['side'], 'right')
        self.assertIn('v2', status.configured[-1]['text'])
        # Repeated cached/network checks must not produce duplicate notices.
        launcher.offer_update('v3', None, 'https://example.invalid/news')
        self.assertEqual(len(frames), 1)
        self.assertEqual(len(buttons), 2)

    def test_network_failure_is_silent_and_corrupt_cache_ignored(self):
        self.cache.write_text('not json', encoding='utf-8')
        self.assertIsNone(core.remembered_update())
        with patch.object(core.urllib.request, 'urlopen', side_effect=OSError('offline')):
            self.assertFalse(core.quiet_update_check({'check_updates': True}))


if __name__ == '__main__':
    unittest.main()
