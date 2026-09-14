"""Integration regressions for coverage_settle against a private PostgreSQL.

Run from the repository root, for example:
    python test/test_coverage_settle.py --exe build/bin/coverage_settle.exe \
        --pg-bin third_party/pgsql/bin -v

Only Python's standard library and PostgreSQL command-line programs are needed.
The suite creates its own cluster below build/test, binds only to loopback on an
unused port, and never accepts a connection string for an existing database.
"""

import argparse
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None
RAW_SCHEMA = """
CREATE TABLE coverage_file_points (
    run_id text NOT NULL, file text NOT NULL, points bytea NOT NULL,
    PRIMARY KEY (run_id, file));
CREATE TABLE coverage_cov_files (
    run_id text NOT NULL, file text NOT NULL, cov bytea, size_bytes bigint,
    PRIMARY KEY (run_id, file));
CREATE TABLE coverage_checkpoints (
    run_id text PRIMARY KEY, blob bytea NOT NULL,
    updated_at timestamptz DEFAULT now());
"""


def sql_text(value):
    return "'" + value.replace("'", "''") + "'"


def sql_bytes(value):
    return "decode('" + value.hex() + "', 'hex')"


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def checkpoint(total_files, version=3, remaining=False, priority=False):
    """Encode only the relevant CoverageCheckpoint protobuf fields."""
    result = b"\x08" + varint(version) + b"\x10" + varint(total_files)
    if remaining:
        result += b"\x22\x01\x01"  # Field 4: one unfinished serialized task.
    if priority:
        result += b"\x2a\x01\x01"  # Field 5: one priority serialized task.
    return result


def pack_points(points):
    return b"".join(struct.pack("<Q", point) for point in points)


class PrivatePostgres:
    def __init__(self, pg_bin, exe):
        self.pg_bin = pg_bin.resolve()
        self.exe = exe.resolve()
        self.test_root = (ROOT / "build" / "test").resolve()
        self.test_root.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix="coverage-settle-", dir=self.test_root))
        self.data = self.root / "pgdata"
        self.started = False
        # Do not inherit PGHOST/PGSERVICE/etc. from a business environment.
        self.env = {key: value for key, value in os.environ.items()
                    if not key.upper().startswith("PG")}
        self.env["PATH"] = os.pathsep.join(
            [str(self.exe.parent), str(self.pg_bin), self.env.get("PATH", "")])
        self.env["PGCLIENTENCODING"] = "UTF8"
        self.env["LC_ALL"] = "C"

    def program(self, name):
        return self.pg_bin / (name + (".exe" if os.name == "nt" else ""))

    def run(self, args, *, check=True, timeout=60, input_text=None, output_file=None):
        kwargs = dict(text=True, encoding="utf-8", errors="replace", timeout=timeout,
                      env=self.env, cwd=self.root,
                      creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        command = [str(arg) for arg in args]
        if output_file is None:
            result = subprocess.run(command, input=input_text, capture_output=True, **kwargs)
        else:
            # PostgreSQL's Windows server inherits standard handles from
            # pg_ctl. Pipes would keep communicate() waiting after pg_ctl exits.
            with output_file.open("w", encoding="utf-8") as output:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=output,
                                        stderr=subprocess.STDOUT, **kwargs)
            result.stdout = output_file.read_text(encoding="utf-8", errors="replace")
            result.stderr = ""
        if check and result.returncode:
            raise RuntimeError(
                f"Command failed ({result.returncode}): {args[0]}\n"
                f"{result.stdout}\n{result.stderr}")
        return result

    def start(self):
        for name in ("initdb", "pg_ctl", "psql"):
            if not self.program(name).is_file():
                raise RuntimeError(f"Missing PostgreSQL program: {self.program(name)}")
        self.run([self.program("initdb"), "-D", self.data, "-U", "coverage_test",
                  "--auth-local=trust", "--auth-host=trust", "--encoding=UTF8",
                  "--locale=C", "--no-sync"], timeout=120)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserve:
            if os.name == "nt":
                reserve.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            reserve.bind(("127.0.0.1", 0))
            self.port = reserve.getsockname()[1]
            # Let PostgreSQL claim this port immediately after releasing it.
        self.conn = (f"host=127.0.0.1 port={self.port} user=coverage_test "
                     "dbname=postgres connect_timeout=5")
        (self.data / "postgresql.auto.conf").write_text(
            f"listen_addresses = '127.0.0.1'\nport = {self.port}\n"
            "unix_socket_directories = ''\nfsync = off\n"
            "synchronous_commit = off\nfull_page_writes = off\n",
            encoding="utf-8")
        # A port collision fails this private server's startup; it must never
        # cause the suite to use whichever other server owns that port.
        self.run([self.program("pg_ctl"), "-D", self.data, "-l",
                  self.root / "postgres.log", "-w", "-t", "30", "start"], timeout=45,
                 output_file=self.root / "pg_ctl-start.log")
        self.started = True
        actual = Path(self.sql("SHOW data_directory;")).resolve()
        if actual != self.data.resolve():
            raise RuntimeError(f"Refusing unexpected PostgreSQL data directory: {actual}")

    def sql(self, query, conn=None):
        return self.run([self.program("psql"), "-X", "--no-password",
                         "--set=ON_ERROR_STOP=1", "-A", "-t", "-d", conn or self.conn,
                         "-f", "-"], input_text=query).stdout.strip()

    def close(self):
        # Also attempt shutdown after partial startup, using only our data dir.
        if self.data.is_dir() and self.program("pg_ctl").is_file():
            result = self.run([self.program("pg_ctl"), "-D", self.data, "-m", "immediate",
                               "-w", "-t", "30", "stop"], check=False, timeout=45)
            if result.returncode and (self.started or (self.data / "postmaster.pid").exists()):
                raise RuntimeError(f"Private PostgreSQL did not stop; retained {self.root}\n"
                                   f"{result.stdout}\n{result.stderr}")
        # Recursive cleanup is confined to the exact directory created above.
        target = self.root.resolve()
        if target.parent != self.test_root or not target.name.startswith("coverage-settle-"):
            raise RuntimeError(f"Refusing unsafe test cleanup path: {target}")
        shutil.rmtree(target)


class CoverageSettleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.exe)
        cls.addClassCleanup(cls.pg.close)
        cls.pg.start()

    def setUp(self):
        self.schema = "case_" + uuid.uuid4().hex
        self.pg.sql(f'CREATE SCHEMA "{self.schema}";')
        self.conn = self.pg.conn + f" options='-csearch_path={self.schema}'"
        self.sql(RAW_SCHEMA)
        self.run_id = "run-'" + uuid.uuid4().hex

    def sql(self, query):
        return self.pg.sql(query, self.conn)

    def set_raw(self, rows, *, run_id=None, blob=None, covs=None):
        run_id = run_id or self.run_id
        run = sql_text(run_id)
        self.sql(f"DELETE FROM coverage_file_points WHERE run_id={run};"
                 f"DELETE FROM coverage_cov_files WHERE run_id={run};"
                 f"DELETE FROM coverage_checkpoints WHERE run_id={run};")
        # Replacing a fixture's entire manifest also replaces its long-lived
        # metadata. Normal post-processing never mutates execution metadata.
        self.sql("DO $fixture$ BEGIN IF to_regclass('plugin_runs') IS NOT NULL THEN "
                 f"DELETE FROM plugin_file_outcomes WHERE run_id={run};"
                 f"DELETE FROM plugin_run_files WHERE run_id={run};"
                 f"DELETE FROM plugin_runs WHERE run_id={run};"
                 "END IF; END $fixture$;")
        for file, points in rows.items():
            points_blob = points if isinstance(points, bytes) else pack_points(points)
            self.sql("INSERT INTO coverage_file_points VALUES "
                     f"({run}, {sql_text(file)}, {sql_bytes(points_blob)});")
        blob = checkpoint(len(rows)) if blob is None else blob
        self.sql("INSERT INTO coverage_checkpoints(run_id, blob) VALUES "
                 f"({run}, {sql_bytes(blob)});")
        for file, cov in (covs or {}).items():
            self.sql("INSERT INTO coverage_cov_files VALUES "
                     f"({run}, {sql_text(file)}, "
                     f"{sql_bytes(cov) if cov is not None else 'NULL'}, "
                     f"{len(cov) if cov is not None else 'NULL'});")

    def settle(self, *extra, succeeds=True, run_id=None):
        result = self.pg.run([self.pg.exe, "--run", run_id or self.run_id,
                              "--db", self.conn, *extra], check=False)
        detail = f"exit={result.returncode}\n{result.stdout}\n{result.stderr}"
        if succeeds:
            self.assertEqual(result.returncode, 0, detail)
        else:
            self.assertNotEqual(result.returncode, 0, detail)
        return result

    def snapshot(self, tables):
        # Include timestamps, so a failed run cannot silently rewrite old rows.
        return tuple(self.sql(f"SELECT row_to_json(t) FROM "
                              f"(SELECT * FROM {table} ORDER BY run_id"
                              f"{', file' if table.endswith(('_points', '_files', '_selected')) else ''}) t;")
                     for table in tables)

    def raw_snapshot(self):
        return self.snapshot(("coverage_file_points", "coverage_cov_files", "coverage_checkpoints"))

    def derived_snapshot(self):
        return self.snapshot(("coverage_selected", "coverage_run_summary", "coverage_cumulative_cov"))

    def selected(self):
        value = self.sql("SELECT file FROM coverage_selected WHERE run_id="
                         f"{sql_text(self.run_id)} ORDER BY file;")
        return value.splitlines() if value else []

    def assert_summary(self, total, selected, cumulative):
        actual = self.sql("SELECT total_files, selected_files, cumulative_points "
                          f"FROM coverage_run_summary WHERE run_id={sql_text(self.run_id)};")
        self.assertEqual(actual, f"{total}|{selected}|{cumulative}")
        merge_totals = self.sql("SELECT covered_lines IS NULL, total_lines IS NULL, "
                                "percent IS NULL FROM coverage_run_summary "
                                f"WHERE run_id={sql_text(self.run_id)};")
        self.assertEqual(merge_totals, "t|t|t")

    def seed_previous_result(self):
        self.set_raw({"original.stp": [99]})
        self.settle()
        self.sql("INSERT INTO coverage_cumulative_cov(run_id, cov) VALUES "
                 f"({sql_text(self.run_id)}, {sql_bytes(b'previous merged cov')});")

    def assert_failure_preserves_data(self, *extra):
        raw, derived = self.raw_snapshot(), self.derived_snapshot()
        self.settle(*extra, succeeds=False)
        self.assertEqual(self.raw_snapshot(), raw)
        self.assertEqual(self.derived_snapshot(), derived)

    def work_parent(self):
        parent = self.pg.root / ("user-work-" + uuid.uuid4().hex)
        parent.mkdir()
        marker = parent / "user-file.txt"
        marker.write_text("Keep this user-owned file.\n", encoding="utf-8")
        return parent, marker

    def test_creates_derived_tables_and_counts_empty_point_sets(self):
        self.set_raw({"a.stp": [1, 2], "b.stp": [2, 3], "empty.stp": []})
        raw = self.raw_snapshot()
        self.settle()
        self.assertEqual(self.selected(), ["a.stp", "b.stp"])
        self.assert_summary(3, 2, 3)
        self.assertEqual(self.raw_snapshot(), raw)
        self.assertEqual(self.sql("SELECT count(*) FROM coverage_cumulative_cov;"), "0")

    def test_atomic_publication_and_database_recovery(self):
        helper = self.pg.exe.parent / "test_plugin_runtime.exe"
        result = self.pg.run([helper, "--db", self.conn], check=False)
        self.assertEqual(result.returncode, 0, result.stdout + "\\n" + result.stderr)

    def test_durable_manifest_survives_checkpoint_pruning(self):
        self.set_raw({"a.stp": [1, 2], "empty.stp": []})
        self.settle()
        run = sql_text(self.run_id)
        self.sql(f"DELETE FROM coverage_checkpoints WHERE run_id={run};")
        self.settle()
        self.assertEqual(self.selected(), ["a.stp"])
        self.assertEqual(self.sql("SELECT status FROM plugin_runs;"), "finished")
        self.assertEqual(self.sql("SELECT count(*) FROM plugin_run_files;"), "2")

    def test_failed_outcome_rejects_even_when_raw_points_exist(self):
        self.set_raw({"a.stp": [1]})
        self.settle()
        self.sql("UPDATE plugin_file_outcomes SET status='failed',detail='injected';")
        self.assert_failure_preserves_data()

    def test_all_empty_point_sets_are_a_successful_empty_selection(self):
        self.set_raw({"a.stp": [], "b.stp": []})
        raw = self.raw_snapshot()
        self.settle()
        self.assertEqual(self.selected(), [])
        self.assert_summary(2, 0, 0)
        self.assertEqual(self.raw_snapshot(), raw)

    def test_greedy_ties_choose_filename_deterministically(self):
        # The insertion order deliberately disagrees with filename order.
        self.set_raw({"z.stp": [1, 2], "a.stp": [1, 2], "middle.stp": [3]})
        for _ in range(3):
            self.settle()
            self.assertEqual(self.selected(), ["a.stp", "middle.stp"])
            self.assert_summary(3, 2, 3)

    def test_repeated_settlement_replaces_results_and_retains_raw_rows(self):
        self.seed_previous_result()
        self.set_raw({"new.stp": [1, 2], "redundant.stp": [2], "empty.stp": []})
        raw = self.raw_snapshot()
        for _ in range(2):
            self.settle()
            self.assertEqual(self.selected(), ["new.stp"])
            self.assert_summary(3, 1, 2)
            self.assertEqual(self.raw_snapshot(), raw)
            self.assertEqual(self.sql("SELECT count(*) FROM coverage_cumulative_cov;"), "0")

    def test_failed_selected_insert_rolls_back_every_derived_table(self):
        self.check_write_rollback("coverage_selected")

    def test_failed_summary_write_rolls_back_every_derived_table(self):
        self.check_write_rollback("coverage_run_summary")

    def check_write_rollback(self, table):
        self.seed_previous_result()
        self.set_raw({"replacement.stp": [1, 2]})
        self.sql("CREATE FUNCTION fail_derived() RETURNS trigger LANGUAGE plpgsql AS $$ "
                 "BEGIN RAISE EXCEPTION 'intentional settlement regression failure'; END; $$;"
                 f"CREATE TRIGGER force_failure BEFORE INSERT OR UPDATE ON {table} "
                 "FOR EACH ROW EXECUTE FUNCTION fail_derived();")
        self.assert_failure_preserves_data()

    def test_incomplete_or_invalid_checkpoints_preserve_previous_results(self):
        cases = {
            "remaining_tasks": checkpoint(1, remaining=True),
            "priority_tasks": checkpoint(1, priority=True),
            "old_version": checkpoint(1, version=2),
            "future_version": checkpoint(1, version=4),
            "zero_total": checkpoint(0),
            "corrupt_protobuf": b"\x80",
            "empty_protobuf": b"",
        }
        for name, blob in cases.items():
            with self.subTest(name=name):
                self.run_id = name
                self.seed_previous_result()
                self.set_raw({"new.stp": [1]}, blob=blob)
                self.assert_failure_preserves_data()

    def test_missing_checkpoint_or_task_results_preserve_previous_results(self):
        for case in ("missing_checkpoint", "missing_row", "extra_row", "no_rows"):
            with self.subTest(case=case):
                self.run_id = case
                self.seed_previous_result()
                rows = {"new.stp": [1]} if case != "no_rows" else {}
                if case == "extra_row":
                    rows["extra.stp"] = []
                total = 2 if case in ("missing_row", "no_rows") else 1
                self.set_raw(rows, blob=checkpoint(total))
                if case == "missing_checkpoint":
                    self.sql("DELETE FROM coverage_checkpoints WHERE run_id="
                             f"{sql_text(self.run_id)};")
                self.assert_failure_preserves_data()

    def test_invalid_point_blob_preserves_previous_results(self):
        self.seed_previous_result()
        self.set_raw({"bad.stp": pack_points([1]) + b"\x00"})
        self.assert_failure_preserves_data()

    def test_default_mode_succeeds_without_occ_even_when_cov_rows_exist(self):
        self.set_raw({"selected.stp": [1]}, covs={"selected.stp": b"cov exists"})
        parent, marker = self.work_parent()
        raw = self.raw_snapshot()
        self.settle("--opencppcoverage", str(parent / "does-not-exist.exe"),
                    "--workdir", str(parent))
        self.assertEqual(self.selected(), ["selected.stp"])
        self.assert_summary(1, 1, 1)
        self.assertEqual(self.raw_snapshot(), raw)
        self.assertEqual(marker.read_text(encoding="utf-8"), "Keep this user-owned file.\n")
        self.assertEqual(list(parent.iterdir()), [marker])

    def test_merge_rejects_missing_or_empty_selected_cov_preserving_results(self):
        for cov in ("missing", b"", None):
            with self.subTest(cov=cov):
                self.run_id = "missing-cov-" + uuid.uuid4().hex
                self.seed_previous_result()
                covs = {"first.stp": b"first cov"}
                if cov != "missing":
                    covs["second.stp"] = cov
                self.set_raw({"first.stp": [1], "second.stp": [2]}, covs=covs)
                parent, marker = self.work_parent()
                self.assert_failure_preserves_data(
                    "--merge-cov", "--opencppcoverage", sys.executable,
                    "--workdir", str(parent))
                self.assertEqual(marker.read_text(encoding="utf-8"), "Keep this user-owned file.\n")

    def test_merge_process_failure_preserves_results_and_user_workdir(self):
        self.seed_previous_result()
        self.set_raw({"selected.stp": [1, 2]}, covs={"selected.stp": b"cov data"})
        parent, marker = self.work_parent()
        # Python is a real executable but rejects OpenCppCoverage's --quiet
        # option with a nonzero exit, providing a portable merge failure.
        self.assert_failure_preserves_data(
            "--merge-cov", "--opencppcoverage", sys.executable, "--workdir", str(parent))
        self.assertEqual(marker.read_text(encoding="utf-8"), "Keep this user-owned file.\n")

    @unittest.skipUnless(os.name == "nt", "PowerShell wrapper is a Windows entry point")
    def test_wrapper_automatically_selects_latest_finished_coverage_run(self):
        self.set_raw({"selected.stp": [1]}, covs={"selected.stp": b"cov exists"})
        self.sql("CREATE TABLE runs (run_id text PRIMARY KEY, plugin_type text, "
                 "status text, created_at timestamptz);"
                 "INSERT INTO runs VALUES "
                 "('older-coverage', 'coverage', 'finished', '2026-01-01'),"
                 f"({sql_text(self.run_id)}, 'coverage', 'finished', '2026-01-02'),"
                 "('newer-stpcompare', 'stpcompare', 'finished', '2026-01-03'),"
                 "('latest-unfinished', 'coverage', 'running', '2026-01-04');")
        parent, marker = self.work_parent()
        raw = self.raw_snapshot()
        powershell = Path(os.environ["SystemRoot"]) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
        result = self.pg.run([
            powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-File", ROOT / "tools" / "settle_coverage.ps1", "-Exe", self.pg.exe,
            "-Psql", self.pg.program("psql"), "-DbConn", self.conn,
            "-OpenCppCoverage", parent / "does-not-exist.exe", "-WorkDir", parent],
            check=False)
        self.assertEqual(result.returncode, 0, result.stdout + "\n" + result.stderr)
        self.assertIn("settled coverage run=" + self.run_id, result.stdout)
        self.assertEqual(self.selected(), ["selected.stp"])
        self.assert_summary(1, 1, 1)
        self.assertEqual(self.sql("SELECT count(*) FROM coverage_run_summary;"), "1")
        self.assertEqual(self.raw_snapshot(), raw)
        self.assertEqual(marker.read_text(encoding="utf-8"), "Keep this user-owned file.\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True, help="coverage_settle executable")
    parser.add_argument("--pg-bin", type=Path,
                        default=Path(os.environ.get("PG_BIN", ROOT / "third_party" / "pgsql" / "bin")),
                        help="directory containing initdb, pg_ctl and psql")
    OPTIONS, unittest_args = parser.parse_known_args()
    if not OPTIONS.exe.is_file():
        parser.error(f"coverage_settle executable does not exist: {OPTIONS.exe}")
    unittest.main(argv=[sys.argv[0], *unittest_args])
