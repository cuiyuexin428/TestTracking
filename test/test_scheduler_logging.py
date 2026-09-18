"""Check durable scheduler logs without replacing its interactive stdin."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest


class SchedulerLoggingTests(unittest.TestCase):
    def test_commands_remain_interactive_and_logs_append_across_restart(self):
        with tempfile.TemporaryDirectory(prefix='scheduler logging ') as temporary:
            root = Path(temporary)
            log = root / 'data/logs/scheduler.log'
            for _ in range(2):
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1', 0))
                    port = listener.getsockname()[1]
                config = root / 'scheduler.json'
                config.write_text(json.dumps({'server': {'router_endpoint': f'tcp://127.0.0.1:{port}',
                    'db_conn': '', 'log_file': 'data/logs/scheduler.log'}, 'plugins': {}}))
                result = subprocess.run([str(SCHEDULER), str(config)], input='status\nquit\n',
                    cwd=root, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn('plugins=0 in_flight=0', result.stdout)
                self.assertIn('scheduler started', result.stdout)
                self.assertIn('scheduler stopped', result.stdout)
            saved = log.read_text(encoding='utf-8')
            self.assertEqual(2, saved.count('scheduler started, config='))
            self.assertEqual(2, saved.count('scheduler stopped'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--scheduler', type=Path, required=True)
    options, remaining = parser.parse_known_args()
    SCHEDULER = options.scheduler.resolve()
    unittest.main(argv=[__file__, *remaining])
