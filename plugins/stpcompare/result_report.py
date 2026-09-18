"""Editable database-only comparison report; no executor or scheduler imports."""
from html import escape
import re


def render(connection, context):
    run_id = context["run_id"]
    table = context.get("options", {}).get("result_table", "")
    if not isinstance(table, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", table):
        raise ValueError("请配置 plugin_options.stpcompare.result_table")
    run = connection.execute(
        "SELECT run_id, expected_files, status FROM plugin_runs "
        "WHERE run_id=%s AND plugin_type='stpcompare'", (run_id,),
    ).fetchone()
    if not run:
        raise ValueError("没有找到 stpcompare 的持久运行记录")
    exists = connection.execute("SELECT to_regclass(%s) AS relation", (table,)).fetchone()["relation"]
    if exists:
        select = ("CASE WHEN o.status IS DISTINCT FROM 'success' OR r.file IS NULL OR r.info_txt IS NULL "
                  "OR r.info_new_txt IS NULL THEN 'incomplete' WHEN r.info_txt=r.info_new_txt "
                  "THEN 'same' ELSE 'different' END AS comparison, "
                  "left(r.info_txt, 2000) AS before_text, left(r.info_new_txt, 2000) AS after_text ")
        join = f"LEFT JOIN {table} r ON r.run_id=m.run_id AND r.file=m.file "
    else:
        select = "'incomplete' AS comparison, NULL::text AS before_text, NULL::text AS after_text "
        join = ""
    rows = connection.execute(
        "SELECT m.file, o.status, o.detail, " + select + "FROM plugin_run_files m "
        "LEFT JOIN plugin_file_outcomes o ON o.run_id=m.run_id "
        "AND o.plugin_type=m.plugin_type AND o.file=m.file " + join +
        "WHERE m.run_id=%s AND m.plugin_type='stpcompare' ORDER BY m.file", (run_id,),
    ).fetchall()
    expected = int(run["expected_files"])
    same = sum(row["comparison"] == "same" for row in rows)
    different = sum(row["comparison"] == "different" for row in rows)
    incomplete = max(0, expected - same - different)
    complete = len(rows) == expected and same + different == expected
    status = "原始数据完整，可进行完整比较" if complete else "原始数据尚不完整，当前展示已入库结果"
    labels = {"same": "相同", "different": "不同", "incomplete": "不完整"}
    cards = [("计划文件", expected), ("相同", same), ("不同", different), ("不完整", incomplete)]
    metrics = "".join('<div class="metric"><span>' + label + '</span><strong>' + str(value) +
                      '</strong></div>' for label, value in cards)
    table_rows = ""
    for row in rows:
        details = ""
        if row["comparison"] == "different":
            details = '<details><summary>查看文本（各前 2000 字符）</summary><div class="diff"><pre>' + escape(
                row["before_text"] or "") + '</pre><pre>' + escape(row["after_text"] or "") + '</pre></div></details>'
        table_rows += ('<tr data-status="' + escape(row["comparison"]) + '"><td>' + escape(row["file"]) +
                       '</td><td>' + labels[row["comparison"]] + '</td><td>' +
                       escape(row["status"] or "尚未入库") + '</td><td>' + escape(row["detail"] or "") + details + '</td></tr>')
    bars = "".join('<div class="bar-row"><span>' + label + '</span><div class="track"><i style="width:' +
                   str(100 * value / max(1, expected)) + '%;background:' + color + '"></i></div><b>' +
                   str(value) + '</b></div>' for label, value, color in
                   [("相同", same, "#47b79e"), ("不同", different, "#dc9a48"), ("不完整", incomplete, "#b9c4d5")])
    file_filter = escape(str(context.get("parameters", {}).get("file_filter", "")), quote=True)
    return """<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>STP 对比分析报告</title><style>
*{box-sizing:border-box}body{margin:0;padding:24px;background:#f6f8fc;color:#27344b;font:15px/1.6 system-ui,sans-serif}h1{font-size:24px;margin:0 0 6px}.muted{color:#6b7890;overflow-wrap:anywhere}.panel{background:white;border:1px solid #e1e7f0;border-radius:14px;padding:20px;margin:18px 0}.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:14px;margin-top:20px}.metric{border-left:3px solid #5d83d7;padding-left:14px}.metric span{display:block;color:#6b7890}.metric strong{font-size:25px}.bar-row{display:flex;align-items:center;gap:14px;margin:10px 0}.bar-row span{width:90px}.bar-row b{min-width:35px}.track{height:13px;background:#edf1f7;flex:1;border-radius:8px;overflow:hidden}.track i{display:block;height:100%}input,select{padding:10px 12px;border:1px solid #ccd6e5;border-radius:8px;font:inherit;max-width:100%;margin:0 8px 8px 0}input{width:320px}.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%;min-width:580px}th,td{text-align:left;padding:12px;border-bottom:1px solid #edf1f6;vertical-align:top}th button{border:0;background:none;font:inherit;color:inherit;cursor:pointer;padding:0}td:first-child{overflow-wrap:anywhere;max-width:400px}.diff{display:grid;grid-template-columns:1fr 1fr;gap:10px}.diff pre{white-space:pre-wrap;overflow-wrap:anywhere;min-width:0;background:#f5f7fb;padding:10px}summary{cursor:pointer;color:#3864af}@media(max-width:600px){body{padding:12px}.panel{padding:14px}.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}.diff{grid-template-columns:1fr}}
</style><h1>STP 对比分析报告</h1><div class="muted">运行：""" + escape(run_id) + """</div><section class="panel"><strong>""" + status + '</strong><div class="metrics">' + metrics + '</div></section><section class="panel">' + bars + """</section>
<section class="panel"><h2>逐文件比较</h2><input id="filter" aria-label="筛选文件" placeholder="筛选文件或说明" value=""" + '"' + file_filter + '"' + """>
<select id="status" aria-label="筛选比较状态"><option value="">全部结果</option value="same">相同</option><option value="different">不同</option><option value="incomplete">不完整</option></select><p id="count" class="muted"></p><div class="table-wrap"><table><thead><tr><th><button data-column="0">文件 ↕</button></th><th><button data-column="1">比较结果 ↕</button></th><th><button data-column="2">入库状态 ↕</button></th><th>说明与文本</th></tr></thead><tbody>""" + table_rows + """</tbody></table></div></section>
<script>
const body=document.querySelector('tbody'),filter=document.querySelector('#filter'),statusFilter=document.querySelector('#status');
function applyFilter(){let shown=0;for(const row of body.rows){row.hidden=!(row.textContent.toLowerCase().includes(filter.value.toLowerCase())&&(!statusFilter.value||row.dataset.status===statusFilter.value));if(!row.hidden)shown++;}document.querySelector('#count').textContent=`显示 ${shown} / ${body.rows.length} 个文件`;}
filter.addEventListener('input',applyFilter);statusFilter.addEventListener('change',applyFilter);applyFilter();
for(const button of document.querySelectorAll('[data-column]'))button.addEventListener('click',()=>{const i=Number(button.dataset.column),direction=button.dataset.direction==='asc'?-1:1;button.dataset.direction=direction===1?'asc':'desc';Array.from(body.rows).sort((a,b)=>a.cells[i].textContent.localeCompare(b.cells[i].textContent,'zh-CN',{numeric:true})*direction).forEach(row=>body.appendChild(row));});
</script></html>"""
