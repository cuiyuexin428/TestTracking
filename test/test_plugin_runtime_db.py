"""Run the plugin runtime database contracts against a private PostgreSQL.

test_plugin_runtime keeps every PostgreSQL assertion behind `--db <conninfo>`;
no other target supplies that argument, so without this wrapper the durable
publication, rollback, retry and disconnect-recovery contracts never execute in
CTest. The private instance is created and destroyed by the test, so a business
database is never touched.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None


class PluginRuntimeDatabaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.exe)
        cls.addClassCleanup(cls.pg.close)
        cls.pg.start()

    def test_database_contracts(self):
        artifacts = ROOT / "build" / "test"
        artifacts.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix="plugin-runtime-db-", dir=artifacts))
        self.addCleanup(shutil.rmtree, root, True)
        result = subprocess.run(
            [str(OPTIONS.exe), "--db", self.pg.conn],
            cwd=str(root), env=self.pg.env, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=180,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        print(result.stdout, flush=True)
        print(result.stderr, flush=True)
        self.assertEqual(result.returncode, 0,
                         f"plugin runtime database contracts failed\n{result.stdout}\n{result.stderr}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True,
                        help="test_plugin_runtime executable")
    parser.add_argument("--pg-bin", type=Path, default=ROOT / "third_party/pgsql/bin")
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.exe = OPTIONS.exe.resolve()
    OPTIONS.pg_bin = OPTIONS.pg_bin.resolve()
    unittest.main(argv=[__file__, *remaining])
