# 可替换 Python 分析报告

结果分析只读取数据库中的长期运行清单、逐文件 outcome 和插件业务数据。
运行时的 scheduler、客户端、`tasks`、checkpoint 都不是分析报告的依赖。
运行已结束、原始数据完整、后处理汇总已生成是三种不同状态。coverage 的内置
Python 脚本直接读取逐文件覆盖点，生成确定性的贪心精简集合；新增覆盖点相同时优先
选择较小的 STP 文件，再按路径排序。该结果是实用的紧凑集合，不声称是数学全局最优。

每个插件有一个可替换的 Python 脚本，接口为：

```python
from html import escape


def render(connection, context):
    rows = connection.execute(
        "SELECT file, status FROM plugin_file_outcomes "
        "WHERE run_id=%s AND plugin_type=%s ORDER BY file",
        (context["run_id"], context["plugin"]),
    ).fetchall()
    items = "".join("<li>" + escape(row["file"]) + "</li>" for row in rows)
    return "<!doctype html><meta charset=utf-8><h1>已入库文件</h1><ul>" + items + "</ul>"
```

`connection` 是返回字典行的 psycopg 连接，默认使用只读事务。
`context` 包含 `plugin`、`run_id`、`options`（该插件配置）和 `parameters`
（本次报告参数）。数据库凭据不进入脚本文本、报告 HTML 或元数据。
脚本返回完整 HTML 字符串，可以使用内联 CSS、JavaScript、SVG 实现筛选、排序和交互图表。
内置模板完全离线运行。基础 runtime 提供标准库和 psycopg；携带分析 wheelhouse 的
服务器包还提供 numpy、pandas、openpyxl、matplotlib 和 plotly，可通过
`tools/install_python_analysis_offline.ps1` 离线安装或恢复。其他 Python 依赖也需要
预先加入服务器包的 Python runtime。

Plotly 报告使用 `fig.to_html(full_html=True, include_plotlyjs=True)` 将图表脚本内联到
HTML；不要使用 `include_plotlyjs="cdn"` 或外部字体、脚本地址，否则内网断网时无法显示。

报告脚本在服务器单独的 Python 子进程里执行，每次读取最新源码，默认超时为 60 秒。
语法不合法的修改不会覆盖已保存脚本，运行错误或超时不会覆盖历史报告。
这是可信内网中的脚本执行能力，子进程用于隔离失败，不是 Python 权限沙箱。

结果页支持编辑脚本，或通过右侧 AI 生成脚本后应用、运行。
AI 可以读取服务端补充的运行目录、数据完整性和数据库表结构，描述缺失数据后生成报告。
替换脚本后重新运行即可更新展示；历史报告保留生成时的数据快照。

| 内容 | 源码运行默认位置 | 服务器包位置 |
|---|---|---|
| coverage 内置脚本模板 | `plugins/coverage/result_report.py` | `result_service/report_templates/coverage.py` |
| 使用者修改的脚本 | `runtime/result_service/analysis_scripts/<plugin>.py` | `data/analysis_scripts/<plugin>.py` |
| HTML、源码快照、元数据 | `runtime/result_service/reports/<report_id>/` | `data/reports/<report_id>/` |

当前只为 coverage 提供业务分析模板，并可从报告下载筛选出的 STP 文件清单。其他插件
由使用者在内网编写或通过 DSH 生成脚本；尚未保存专用脚本时，界面使用只展示持久
manifest/outcome 的通用模板。使用者脚本优先于内置模板。
每份报告保存脚本 SHA-256、参数、插件、run ID 和生成时间。迁移部署时保留 `data` 目录，
模板升级不会覆盖使用者修改的脚本。

HTTP 接口：

- `GET /api/analysis/catalog?plugin=<plugin>`：列出数据库中的运行、完整性、表结构。
- `GET /api/analysis/runs/<run_id>?plugin=<plugin>`：读取持久结果详情。
- `GET /api/plugins/<plugin>/report-script`：读取当前 Python `source` 和内容摘要 `revision`。
- `PUT /api/plugins/<plugin>/report-script`：保存 `{"source": "完整 Python 源码", "revision": "编辑前读取的 revision", "operator_id": "当前会话 UUID"}`。
- `POST /api/plugins/<plugin>/reports`：提交 `{"run_id": "...", "parameters": {}}`，运行报告。
- `GET /api/plugins/<plugin>/reports?run_id=<run_id>`：返回 `{"latest": ...}`，恢复最近一次成功报告；没有报告时 `latest` 为 `null`。
- `GET /reports/<report_id>/index.html`：通过主服务地址显示生成结果，局域网浏览器可直接访问。

保存脚本必须先读取当前源码及其 revision。DSH 原生界面使用当前浏览器会话的
`operator_id`；原生 DSH 通过 `TESTTRACKING_OPERATOR_ID` 获取该身份，不应自行生成另一
个身份。缺少 revision 或有效身份时接口拒绝保存。若返回 HTTP 409，应保留草稿，读取
最新源码与 revision，合并双方修改后再保存；不能直接替换 revision 重试覆盖。修改的
脚本成功保存后再生成报告，保存失败不代表新脚本已生效。

这里的 `parameters` 是数据库分析脚本的报告参数，与 scheduler 创建任务时的 `params`
分开。coverage 和 stpcompare 当前不接受非空执行 `params`，不影响分析脚本自行定义
报告参数。

HTTP 服务和报告进程使用同一套数据库配置，插件查询自行定义。
需要更新派生数据库结果的结算仍由独立后处理工具负责，不会因刷新页面而运行。
