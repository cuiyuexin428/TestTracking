"""Execute one trusted Python report in a separate process using only a database."""
from __future__ import annotations

import json
from pathlib import Path
import sys
from types import ModuleType


def render_report(script_path: Path, output_path: Path, request: dict) -> None:
    import psycopg
    from psycopg.rows import dict_row

    # Compile current source directly: edits never reuse an old __pycache__ entry.
    module = ModuleType("testtracking_report")
    module.__file__ = str(script_path)
    sys.modules[module.__name__] = module
    exec(compile(script_path.read_text(encoding="utf-8-sig"), str(script_path), "exec"), module.__dict__)
    render = getattr(module, "render", None)
    if not callable(render):
        raise ValueError("report script must define render(connection, context)")
    with psycopg.connect(request["db_conn"], row_factory=dict_row, connect_timeout=5) as connection:
        connection.read_only = True
        connection.execute("SELECT set_config('statement_timeout', %s, false)",
                           (str(request["statement_timeout_ms"]),))
        html = render(connection, request["context"])
        if not isinstance(html, str) or not html.strip():
            raise ValueError("render(connection, context) must return non-empty HTML text")
        output_path.write_text(html, encoding="utf-8")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    try:
        request = json.loads(sys.stdin.buffer.read().decode("utf-8"))
        render_report(Path(sys.argv[1]), Path(sys.argv[2]), request)
        return 0
    except Exception as exc:
        # The parent records the failure without replacing any earlier report.
        print(f"{type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
