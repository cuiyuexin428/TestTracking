"""Editable coverage report. Its only data source is the durable database."""
from html import escape


def render(connection, context):
    run_id = context["run_id"]
    run = connection.execute(
        "SELECT run_id, expected_files, status, dataset_version, program_version "
        "FROM plugin_runs WHERE run_id=%s AND plugin_type='coverage'", (run_id,)
    ).fetchone()
    if not run:
        raise ValueError("没有找到 coverage 的持久运行记录")
    raw_exists = connection.execute(
        "SELECT to_regclass('coverage_file_points') AS relation"
    ).fetchone()["relation"]
    points = "octet_length(p.points) / 8" if raw_exists else "NULL::integer"
    raw_join = ("LEFT JOIN coverage_file_points p ON p.run_id=m.run_id AND p.file=m.file "
                if raw_exists else "")
    rows = connection.execute(
        "SELECT m.file, o.status, o.detail, " + points + " AS points "
        "FROM plugin_run_files m LEFT JOIN plugin_file_outcomes o "
        "ON o.run_id=m.run_id AND o.plugin_type=m.plugin_type AND o.file=m.file "
        + raw_join + "WHERE m.run_id=%s AND m.plugin_type='coverage' ORDER BY m.file",
        (run_id,),
    ).fetchall()
    summary_exists = connection.execute(
        "SELECT to_regclass('coverage_run_summary') AS relation"
    ).fetchone()["relation"]
    summary = connection.execute(
        "SELECT total_files, selected_files, cumulative_points, covered_lines, "
        "total_lines, percent FROM coverage_run_summary WHERE run_id=%s", (run_id,)
    ).fetchone() if summary_exists else None
    expected = int(run["expected_files"])
    success = sum(row["status"] == "success" and row["points"] is not None for row in rows)
    failed = sum(row["status"] == "failed" for row in rows)
    missing = max(0, expected - success - failed)
    complete = len(rows) == expected and success == expected
    readiness = "原始数据完整，可进行完整分析" if complete else "原始数据尚不完整，当前展示已入库结果"
    cards = [("计划文件", expected), ("原始结果齐全", success), ("执行失败", failed), ("待补齐", missing)]
    if summary:
        cards += [("精简集合文件", summary["selected_files"]),
                  ("覆盖点数", summary["cumulative_points"]),
                  ("行覆盖率", str(summary["percent"]) + "%")]
    summary_note = ("已生成结算汇总。" if summary else
                    "尚未生成结算汇总；这不影响查看已经入库的原始结果。完整覆盖率和精简集合需要运行独立后处理工具。")
    statuses = [("原始结果齐全", success, "#47b79e"), ("执行失败", failed, "#de6d7a"),
                ("待补齐", missing, "#b9c4d5")]
    bars = "".join('<div class="bar-row"><span>' + escape(label) + '</span><div class="track">'
                   '<i style="width:' + str(100 * count / max(1, expected)) + '%;background:' + color +
                   '"></i></div><b>' + str(count) + '</b></div>' for label, count, color in statuses)
    table_rows = ""
    for row in rows:
        status = ("原始结果齐全" if row["status"] == "success" and row["points"] is not None
                  else "执行失败" if row["status"] == "failed" else "待补齐")
        values = [row["file"], status, row["points"] if row["points"] is not None else "—", row["detail"] or ""]
        table_rows += "<tr>" + "".join("<td>" + escape(str(value)) + "</td>" for value in values) + "</tr>"
    metrics = "".join('<div class="metric"><span>' + escape(label) + '</span><strong>' +
                      escape(str(value)) + '</strong></div>' for label, value in cards)
    file_filter = escape(str(context.get("parameters", {}).get("file_filter", "")), quote=True)
    return """<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Coverage 分析报告</title>
<style>
*{box-sizing:border-box}body{margin:0;padding:24px;background:#f6f8fc;color:#27344b;font:15px/1.6 system-ui,sans-serif}
h1{font-size:24px;margin:0 0 6px}.muted{color:#6b7890;overflow-wrap:anywhere}.panel{background:white;border:1px solid #e1e7f0;border-radius:14px;padding:20px;margin:18px 0}
.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:14px}.metric{border-left:3px solid #5d83d7;padding-left:14px}.metric span{display:block;color:#6b7890}.metric strong{font-size:25px}
.bar-row{display:flex;align-items:center;gap:14px;margin:10px 0}.bar-row span{width:110px}.bar-row b{min-width:35px}.track{height:13px;background:#edf1f7;flex:1;border-radius:8px;overflow:hidden}.track i{display:block;height:100%}
input{padding:10px 12px;border:1px solid #ccd6e5;border-radius:8px;width:min(100%,420px);font:inherit}.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%;min-width:570px}th,td{text-align:left;padding:12px;border-bottom:1px solid #edf1f6;vertical-align:top}th button{border:0;background:none;font:inherit;color:inherit;cursor:pointer;padding:0}td:first-child{overflow-wrap:anywhere;max-width:440px}button:hover{text-decoration:underline}
@media(max-width:600px){body{padding:12px}.panel{padding:14px}.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}h1{font-size:21px}}
</style><h1>Coverage 分析报告</h1><div class="muted">运行：""" + escape(run_id) + """</div>
<section class="panel"><strong>""" + readiness + "</strong><p>" + summary_note + """</p>
<div class="metrics">""" + metrics + "</div></section><section class=\"panel\">" + bars + """</section>
<section class="panel"><h2>逐文件原始结果</h2><input id="filter" aria-label="筛选文件" placeholder="筛选文件、状态或说明" value=""" + '"' + file_filter + '"' + """>
<p class="muted" id="count"></p><div class="table-wrap"><table><thead><tr><th><button data-column="0">文件 ↕</button></th><th><button data-column="1">结果状态 ↕</button></th><th><button data-column="2">覆盖点数 ↕</button></th><th><button data-column="3">说明 ↕</button></th></tr></thead><tbody>""" + table_rows + """</tbody></table></div></section>
<script>
const body=document.querySelector('tbody'), filter=document.querySelector('#filter');
function applyFilter(){let shown=0;for(const row of body.rows){row.hidden=!row.textContent.toLowerCase().includes(filter.value.toLowerCase());if(!row.hidden)shown++;}document.querySelector('#count').textContent=`显示 ${shown} / ${body.rows.length} 个文件`;}
filter.addEventListener('input',applyFilter);applyFilter();
for(const button of document.querySelectorAll('[data-column]'))button.addEventListener('click',()=>{const i=Number(button.dataset.column),direction=button.dataset.direction==='asc'?-1:1;button.dataset.direction=direction===1?'asc':'desc';const rows=Array.from(body.rows);rows.sort((a,b)=>a.cells[i].textContent.localeCompare(b.cells[i].textContent,'zh-CN',{numeric:true})*direction);rows.forEach(row=>body.appendChild(row));});
</script></html>"""
