from __future__ import annotations

import ast
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from uuid import uuid4


DEFAULT_REPORT_SCRIPT = '''"""Replace this report with your plugin-specific database analysis."""
from html import escape


def render(connection, context):
    identity = (context["run_id"], context["plugin"])
    run = connection.execute(
        "SELECT run_id, expected_files, status FROM plugin_runs WHERE run_id=%s AND plugin_type=%s",
        identity,
    ).fetchone()
    if not run:
        raise ValueError("没有找到持久运行记录")
    rows = connection.execute(
        "SELECT m.file, o.status, o.detail FROM plugin_run_files m "
        "LEFT JOIN plugin_file_outcomes o ON o.run_id=m.run_id "
        "AND o.plugin_type=m.plugin_type AND o.file=m.file "
        "WHERE m.run_id=%s AND m.plugin_type=%s ORDER BY m.file", identity,
    ).fetchall()
    content = "".join("<tr>" + "".join("<td>" + escape(str(row.get(key) or "")) + "</td>"
                                    for key in ("file", "status", "detail")) + "</tr>" for row in rows)
    return ("<!doctype html><html lang=zh-CN><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>数据库结果报告</title><style>body{font:16px/1.6 system-ui;margin:24px;color:#27344b}"
            "table{border-collapse:collapse;width:100%}td,th{padding:10px;text-align:left;"
            "border-bottom:1px solid #ddd}td{overflow-wrap:anywhere}input{font:inherit;padding:8px}"
            "</style><h1>" + escape(context["plugin"]) + " 结果报告</h1><p>运行：" +
            escape(context["run_id"]) + " · 计划文件：" + str(run["expected_files"]) +
            "</p><p>以下为已入库的逐文件结果。可替换此 Python 脚本，读取插件业务表并生成分析图表。</p>"
            "<input id=filter placeholder=筛选文件或状态 aria-label=筛选结果><table><thead>"
            "<tr><th>文件</th><th>状态</th><th>说明</th></tr></thead><tbody>" + content +
            "</tbody></table><script>document.querySelector('#filter').oninput=function(){"
            "for(const row of document.querySelectorAll('tbody tr'))"
            "row.hidden=!row.textContent.toLowerCase().includes(this.value.toLowerCase());};</script></html>")
'''


class ReportError(ValueError):
    """A report script could not be saved, executed, or found."""


class ReportStore:
    """Editable Python scripts and immutable, database-derived HTML reports."""

    def __init__(self, scripts_dir: str | Path, artifacts_dir: str | Path,
                 default_scripts_dir: str | Path | None = None,
                 python_executable: str | Path | None = None,
                 timeout_seconds: float = 60):
        self.scripts_dir = Path(scripts_dir).resolve()
        self.artifacts_dir = Path(artifacts_dir).resolve()
        self.default_scripts_dir = Path(default_scripts_dir).resolve() if default_scripts_dir else None
        self.plugin_source_root = Path(__file__).resolve().parents[2] / "plugins"
        self.python_executable = str(python_executable or sys.executable)
        self.timeout_seconds = timeout_seconds

    @staticmethod
    def _plugin_name(plugin: str) -> str:
        if not isinstance(plugin, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", plugin):
            raise ReportError("invalid plugin name")
        return plugin

    def get_script(self, plugin: str) -> dict[str, Any]:
        plugin = self._plugin_name(plugin)
        path = self.scripts_dir / f"{plugin}.py"
        origin = "runtime"
        if not path.is_file():
            path = (self.default_scripts_dir / f"{plugin}.py" if self.default_scripts_dir
                    else self.plugin_source_root / plugin / "result_report.py")
            origin = "default"
        try:
            source = path.read_text(encoding="utf-8-sig")
        except FileNotFoundError:
            source = DEFAULT_REPORT_SCRIPT
            origin = "generic"
        except OSError as exc:
            raise ReportError(f"cannot read report script for plugin: {plugin}") from exc
        return {"plugin": plugin, "source": source, "origin": origin, "path": str(path)}

    def save_script(self, plugin: str, source: str) -> dict[str, Any]:
        plugin = self._plugin_name(plugin)
        self._validate_script(source)
        self.scripts_dir.mkdir(parents=True, exist_ok=True)
        target = self.scripts_dir / f"{plugin}.py"
        temporary = self.scripts_dir / f".{plugin}.{uuid4().hex}.tmp"
        try:
            temporary.write_text(source, encoding="utf-8")
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
        return self.get_script(plugin)

    @staticmethod
    def _validate_script(source: str) -> None:
        if not isinstance(source, str) or not source.strip():
            raise ReportError("report source must be a non-empty Python script")
        try:
            tree = ast.parse(source, filename="result_report.py")
            compile(tree, "result_report.py", "exec")
        except (SyntaxError, ValueError) as exc:
            raise ReportError(f"invalid Python report script: {exc}") from exc
        if not any(isinstance(node, ast.FunctionDef) and node.name == "render" for node in tree.body):
            raise ReportError("report script must define render(connection, context)")

    def generate(self, plugin: str, run_id: str, db_conn: str,
                 options: dict[str, Any] | None = None,
                 parameters: dict[str, Any] | None = None) -> dict[str, Any]:
        if not isinstance(run_id, str) or not run_id.strip():
            raise ReportError("run_id is required")
        if not db_conn.strip():
            raise ReportError("database is not configured")
        if parameters is not None and not isinstance(parameters, dict):
            raise ReportError("report parameters must be an object")
        script = self.get_script(plugin)
        self._validate_script(script["source"])
        report_id = uuid4().hex
        self.artifacts_dir.mkdir(parents=True, exist_ok=True)
        temporary = self.artifacts_dir / f".{report_id}.tmp"
        temporary.mkdir()
        try:
            script_path = temporary / "script.py"
            script_path.write_text(script["source"], encoding="utf-8")
            request = {
                "db_conn": db_conn,
                "context": {"plugin": plugin, "run_id": run_id,
                            "options": options or {}, "parameters": parameters or {}},
                "statement_timeout_ms": max(1, int(self.timeout_seconds * 1000)),
            }
            command = [self.python_executable, "-B", str(Path(__file__).with_name("report_worker.py")),
                       str(script_path), str(temporary / "index.html")]
            try:
                process = subprocess.run(
                    command, input=json.dumps(request, ensure_ascii=False),
                    text=True, encoding="utf-8", capture_output=True,
                    timeout=self.timeout_seconds, cwd=temporary,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                )
            except subprocess.TimeoutExpired as exc:
                raise ReportError("report script timed out; the previous report is unchanged") from exc
            except OSError as exc:
                raise ReportError("could not start the Python report worker") from exc
            if process.returncode:
                detail = process.stderr.strip()[-3000:] or "worker exited without an error message"
                raise ReportError(f"report script failed: {detail}")
            output = temporary / "index.html"
            if not output.is_file() or not output.stat().st_size:
                raise ReportError("report script did not produce HTML")
            metadata = {
                "report_id": report_id, "plugin": plugin, "run_id": run_id,
                "url": f"/reports/{report_id}/index.html",
                "created_at": datetime.now(timezone.utc).isoformat(),
                "script_sha256": hashlib.sha256(script["source"].encode("utf-8")).hexdigest(),
                "parameters": parameters or {},
            }
            (temporary / "metadata.json").write_text(
                json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
            os.replace(temporary, self.artifacts_dir / report_id)
            return metadata
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)

    def artifact_path(self, report_id: str) -> Path:
        if not isinstance(report_id, str) or not re.fullmatch(r"[a-f0-9]{32}", report_id):
            raise ReportError("invalid report identifier")
        path = self.artifacts_dir / report_id / "index.html"
        if not path.is_file():
            raise ReportError("report not found")
        return path

    def latest(self, plugin: str, run_id: str) -> dict[str, Any] | None:
        plugin = self._plugin_name(plugin)
        if not isinstance(run_id, str) or not run_id.strip():
            raise ReportError("run_id is required")
        latest: dict[str, Any] | None = None
        for metadata_path in self.artifacts_dir.glob("*/metadata.json"):
            report_id = metadata_path.parent.name
            if not re.fullmatch(r"[a-f0-9]{32}", report_id):
                continue
            if not (metadata_path.parent / "index.html").is_file():
                continue
            try:
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if (not isinstance(metadata, dict) or metadata.get("report_id") != report_id
                    or metadata.get("plugin") != plugin or metadata.get("run_id") != run_id
                    or not isinstance(metadata.get("created_at"), str)):
                continue
            if latest is None or metadata["created_at"] > latest["created_at"]:
                latest = metadata
        return latest
