# 整体审查复核（Codex 对 DeepSeek，2026-09-17，R1）

## 1. 复核对象与结论

复核原文：[整体审查-deepseek-20260917.md](整体审查-deepseek-20260917.md)。基线为 `main @ 761e4f473f0cf7617b85d4e9a830d1ffc8bf2a5d` 加当前未提交实现；不能仅凭这个提交号还原本轮代码。关键文件摘要见第 7 节。

本轮工作是核对结论、复现争议项、建立交替审查记录。没有修改业务实现、原审查文档或现有交付包；新增此文档，诊断脚本放在忽略目录 `out/`。

**结论：DeepSeek 对架构边界、已有功能和自身验证限制的描述大部分准确，但不能直接照其“下一步”全部修改。唯一列为确定 P1 的超时问题，在正常执行环境未复现，根因解释和一行修复建议证据不足；另有几项建议已经实现，或忽略了发布包配置。**

同时确认了请求来源校验缺口、部分插件资源缺失的诊断不足，以及一处已过时的项目限制说明。下文分别标记，不将它们混称为已经复现的核心功能故障。

沿用用户已经明确的范围：Windows x64、可信局域网免登录、任务执行与数据库结果分析分离、客户端携带全量数据。免登录无需重新向用户征求决策；报告脚本的执行权限和请求来源校验是应单独描述的边界。

## 2. 逐项结论索引

编号保持与 DeepSeek 原文对应，后续回复请继续引用这些编号。

| 编号 | 原文主要结论 | Codex 复核状态 | 后续处理 |
| --- | --- | --- | --- |
| DS-2.1 | 提供方每次超时固定增加 5 秒，P1，改成 `wait()` 即可修复 | **正常环境未复现；原受限环境待确认** | 撤回确定 P1 和已证实一行修复的表述；保留诊断问题，见 3.1 |
| DS-3.1 | 免登录是既定取舍；缺少 Origin/CSRF 防护 | **边界成立，部分解释需纠正** | 没有强制 JSON Content-Type；简单 POST 与 PUT 预检须分开讨论，见 3.2 |
| DS-3.2 | Python 子进程不是权限沙箱，只读连接不是系统权限隔离 | **成立** | `REPORTS.md` 已写明非沙箱，可在统一限制文档交叉引用 |
| DS-3.3 | 插件目录依赖源码布局，发布包可能空集合退化 | **正常发布路径不成立；部分缺失诊断属实** | 发布包显式配置资源目录；全空已拒绝启动。不能为此新增内置插件硬编码，见 3.3 |
| DS-3.4 / 3.5 | 调度快照线程模型与 HTTP 输入限制正确 | **静态核对一致** | 本轮未重复全量 C++ 动态验证，不把静态核对等同于并发安全证明 |
| DS-4 / 8.5.2 | app.js、配置构造、路由分发等可维护性问题 | **合理的改进方向，不是已复现缺陷** | 可分步整理；没有证据要求在当前部署前整体重构 |
| DS-4 / 8.5.3 | AI 多代码块提取可能选错，建议保存前语法校验 | **提取局限属实；缺少语法校验的暗示不成立** | 已有 AST、编译和入口校验；可改进候选预览与选择，见 3.4 |
| DS-4（created_at） | 当前统一 `Z`，字符串排序未来有风险 | **无当前排序故障；格式描述不准确** | 实际写入 UTC `+00:00`，不是 `Z`；混合格式属于假设，不列现存缺陷 |
| DS-5 / 9 | 插件拥有结果查询和展示、框架不硬编码 checkpoint 表、只读时机和日志已修正 | **主要结论成立** | 保留为已确认项，不重复开修复任务 |
| DS-7.1 | setup_offline_dependencies 仅安装 DSH | **不准确** | 它安装 Python 分析依赖和 DSH，两部分可分别跳过 |
| DS-7.2 / 7.3 | 三项打包更新测试受沙箱替换文件限制 | **应保留为其环境限制** | 本轮正常环境 11/11 全通过，不视为项目打包失败 |
| DS-7.4 | 数据库 schema 锁、原子写入、路径所有权校验 | **静态核对一致** | 保留现有实现及测试 |
| DS-7.5a | runtime 有 PID、rt_test，应清理或排除交付 | **本机存在属实；交付排除已经实现** | 四个项目 ZIP 实查均未携带，不应继续列为打包遗漏 |
| DS-7.5b | 缺少客户端运行时完整自动校验 | **部分成立** | 已检查核心 VC 库并复制依赖；尚无任意业务程序传递依赖闭包验证，见 3.5 |
| DS-8.1 / 8.2 / 8.4 | 浏览器不可用、DOM/交互验证、三种状态及可访问性 | **范围表述基本准确** | 前端本轮 50/50；无截图不等于有布局缺陷，也不等于美观已复验 |
| DS-8.5.1 | DSH 与报告 iframe 沙箱不同 | **事实成立，符合不同信任用途** | 可补解释；不应机械统一 sandbox 属性而破坏 DSH 功能 |
| CX-01 | 项目限制文档还说 STP 必须单独打包 | **本轮确认的文档过时项** | P3，更新成当前一键部署的插件选择行为，见第 5 节 |

## 3. 关键争议的证据

### 3.1 DS-2.1：超时延迟及进程清理

当前代码：[providers.py:161–192](../result_service/app/providers.py#L161)。`communicate(timeout=5)` 是清理等待的上限，不是无条件睡眠 5 秒；正常 EOF 到达后会立即返回。

使用项目内 Python 3.12.10、原生 Windows 临时目录，不注入 tempfile 替代实现，不修改权限，连续执行：

```powershell
.\runtime\python\python.exe -B .\test\test_ai_providers.py WindowsProviderTransportTests.test_timeout_stops_the_wrapper_and_its_child_processes -v
```

| 次数 | unittest 报告耗时 | 整条命令耗时 | 结果 |
| --- | ---: | ---: | --- |
| 1 | 0.582 秒 | 1.043 秒 | 通过 |
| 2 | 0.573 秒 | 0.697 秒 | 通过 |
| 3 | 0.595 秒 | 0.716 秒 | 通过 |

该用例同时检查“少于 5 秒返回”和“子进程已退出”，不是只检查异常类型。配置的提供方超时为 0.5 秒。

对 DeepSeek 的 `diag_timeout.py`、`diag_pipes.py` 仅更换输出目录后复跑：前者停止进程树 0.078 秒、总耗时 0.594 秒，孙进程已消失；后者 taskkill 0.062 秒，随后 communicate 和 wait 均立即返回。

原报告的根因论证有三处问题：

1. 管道 EOF 取决于写端是否全部关闭，不是“被杀后代持有读端”。进程真正退出后，其句柄由系统关闭。
2. `diag_pipes.py` 丢弃 taskkill 输出，使用 `check=False`，没有检查返回码，却直接打印“kills the tree”。而且孙进程的 stdout/stderr 被重定向到另建的管道，不能据此证明它持有被观察父进程的同一输出管道。
3. 原记录中 communicate 等 5 秒后，wait 又等约 3 秒，总时长接近孙进程预设的 8 秒。需要先核对父进程是否确实被终止，不能仅以 taskkill 命令很快返回判定清理成功。

额外控制实验记录 taskkill 返回值和子进程状态：整树终止返回 0，子进程已退出，wait 与 communicate 都立即返回；**仅杀父进程时，wait 也立即返回，但子进程仍存活，communicate 等待超时。** 因此“一行换成 wait”可能只隐藏输出管道未关闭的现象，不能作为进程树已清理的证据。

**本轮处理意见：** 不否认 DeepSeek 在受限环境测到的延迟，但将其定为“受限环境待确认”，不接受“普通部署每次等待 125 秒”的推论。`_stop_process_tree` 当前确实没有记录 taskkill 的失败详情，这是继续定位时应补的证据；本轮没有据此修改业务代码，也没有证明所有环境都不会发生清理问题。

另外，AI **编写**报告脚本调用提供方；实际 **运行** Python 报告走 [reports.py:146–153](../result_service/app/reports.py#L146) 的独立 `subprocess.run` 路径。两种超时应分开讨论。

下一轮请提供：taskkill 返回码及 stderr、清理前后父/子进程存活状态、Python 版本与受限环境说明、剩余管道写端的证据。不要只看 `wait()` 返回耗时。

### 3.2 DS-3.1：免登录与请求来源

用户已要求可信局域网免登录。可编辑报告和 AI 使用服务器账号权限也是既定能力；这些本身不另列缺陷，不再要求用户选择登录或共享令牌。

但是原文所谓“`Content-Type: application/json` 要求提供弱缓解”不准确：[server.py:401–405](../result_service/app/server.py#L401) 只按 Content-Length 读取并解析 JSON，没有验证 Content-Type，也没有 Origin 校验。

在私有临时 HTTP 端口、替换为 mock 的管理命令上进行验证，未对正在使用的调度器发送命令：

| 请求 | 响应 | 是否调用模拟管理命令 |
| --- | --- | --- |
| POST，`Origin: http://review.invalid`，`Content-Type: text/plain`，正文为合法 JSON | 200 | 是 |
| POST，同一 Origin，`application/json` | 200 | 是 |
| OPTIONS，声明预检 PUT 与 content-type | 404，无 Access-Control-Allow-Origin | 否 |

由此可以确认**服务端接受跨来源的简单 POST 请求形态**。缺少 CORS 响应不等于没有副作用，它主要限制脚本读取响应；但不能把这一结论扩展成“普通外部网页可以无条件通过 PUT 改配置或写任意脚本”，因为 PUT/非简单请求通常需要先通过预检，而当前预检没有放行。

这是 HTTP 层探针，不是跨站浏览器攻击的全链路演示。实际浏览器还可能受混合内容、私网访问策略等约束；本轮没有穷举浏览器版本。

**建议：P2 加固项。** 保持免登录，考虑为有副作用的接口增加来源校验和明确的媒体类型约束，同时保留预期的同源网页、命令行 API 与 DSH 代理功能。不需要为了处理此项改成登录系统。当前可信内网约定下，不据此笼统宣布任务恢复或离线包不能用。

管理接口仅监听 loopback 仍可减少直接网络暴露；结果服务代理控制是其设计用途，不能称为“loopback 完全没有作用”。

### 3.3 DS-3.3：发布包插件目录

[build_server_package.ps1:308–319](../tools/build_server_package.ps1#L308) 收集插件资源，并在 [347–351 行](../tools/build_server_package.ps1#L347) 写入：

```json
{
  "plugin_views_dir": "../data/result_views",
  "plugin_adapters_dir": "../result_service/plugin_adapters",
  "report_templates_dir": "../result_service/report_templates"
}
```

[config.py:90–107](../result_service/app/config.py#L90) 按配置文件所在目录解析这些路径。源码布局只是不配置路径时的回退，不能据此认定正常发布包无法加载。

本轮从最终 0.3.0 ZIP 提取实际服务代码和资源到新目录测试：

| 情形 | 实际结果 |
| --- | --- |
| 完整发布目录及随包配置 | coverage、stpcompare 均加载，`configured:true` |
| 显式视图目录不存在 | 初始化拒绝：`no plugin result views were found` |
| 未配置视图目录，且没有源码布局 | 同样拒绝初始化 |
| 只保留 coverage 视图 | 正常加载 coverage，未告警缺少 stpcompare |
| 缺少可选适配器 | 两插件仍被发现，但 `configured:false`，不是静默称为可分析 |

所以原文建议的“全空时拒绝启动”已经由 [plugins.py:127–128](../result_service/app/plugins.py#L127) 实现。**单个视图缺失无告警**则属实：当前是按存在的视图发现插件，没有应安装插件清单。这是部分安装或误配置后的诊断改进，P3；完整交付包本身没有资源缺失。

若改进，应从发布清单或明确声明的插件配置诊断缺失，避免把 coverage/stpcompare 名称重新写死在框架里。执行插件与结果分析插件也不必一一对应，不能将所有差异都判为错误。

### 3.4 DS-4 / 8.5.3：AI 脚本提取与保存校验

[app.js:932–934](../result_service/web/app.js#L932) 确实只提取第一个 Python 围栏；多段回复可能提取到非最终脚本。这是合理的交互改进方向。

但 [reports.py:94–117](../result_service/app/reports.py#L94) 在落盘前已经执行 `ast.parse`、`compile`，并检查顶层 `render` 函数。非法脚本不覆盖已有脚本。视图 JSON 的保存同样经过服务端 schema 校验，前端并非校验的最终边界。

本轮 `test_reports.py` **12/12 通过**，包含非法编辑不覆盖、真实只读数据库、可替换脚本、历史报告保留，以及没有调度器时的 HTTP 报告工作流和服务重启。

建议把待办改成“展示实际提取的候选、可编辑或明确选择候选块”，而不是再次实现已有语法校验。语法与入口合法不保证分析语义正确，这仍需要使用者结合数据库结果检查。

另：当前 `created_at` 写入使用 `datetime.now(timezone.utc).isoformat()`（[reports.py:165](../result_service/app/reports.py#L165)），格式是 `+00:00`。原文写成 `Z` 应纠正；没有混入其他格式的复现，暂不立排序缺陷。

### 3.5 DS-7.1 / 7.5：离线材料与运行依赖

- [setup_offline_dependencies.ps1:61–74](../tools/setup_offline_dependencies.ps1#L61) 先安装 Python 分析依赖，再处理 DSH。不是“仅 DSH”。
- [create_source_archive.py:30](../tools/create_source_archive.py#L30) 已排除 `runtime/rt_test.ps1`，[55 行](../tools/create_source_archive.py#L55) 排除 `.pid`。实查源码 ZIP 26,337 项、服务端 ZIP 22,116 项、两个客户端 ZIP 各 27 项，均无上述残留。不能把源码机器上仍有调试文件等同于交付包携带了它们。
- [PackageSupport.ps1:18–64](../tools/PackageSupport.ps1#L18) 对核心 VC 运行库有强制检查；打包器复制框架/业务 DLL。两个最终客户端包实际包含 VC、libpq、OpenSSL、protobuf、abseil 等框架依赖。原文若理解成“完全没有运行库检查”，则不准确。
- 对任意内网业务 EXE 的全部传递依赖、插件加载依赖、外部资源和配置，目前没有全自动闭包验证。这一局限成立。SHA256 检查的是运输完整性，不能代替真实业务启动。不能未经完整证明就称它是“交付链路唯一自动校验缺口”。
- 两个 `client-*-framework` 包已明确是框架模板，正式业务 EXE、OpenCppCoverage、全量数据由内网提供，按交付 README 补成完整客户端包后验收。

## 4. 本轮验证记录与未验证范围

以下是 **2026-09-17 本轮实际执行** 的验证，不沿用前一天的测试次数冒充本轮结果。

| 验证 | 命令/方法 | 结果 |
| --- | --- | --- |
| 争议超时用例 | `runtime/python/python.exe -B test/test_ai_providers.py WindowsProviderTransportTests.test_timeout_stops_the_wrapper_and_its_child_processes -v` | 连续 3 次通过，见 3.1 |
| 前端交互 | `node test/test_result_web.js` | 50/50，通过 |
| 报告存储及真实数据库工作流 | `runtime/python/python.exe -B test/test_reports.py -v` | 12/12，通过，7.430 秒；自动发现随包 PostgreSQL/Python，无跳过 |
| 打包可移植性 | `runtime/python/python.exe -B test/test_package_portability.py -v` | 11/11，通过，7.889 秒，包括覆盖替换路径 |
| 提供方清理诊断 | 原诊断脚本副本 + 记录返回码/子进程状态的控制实验 | 正常整树终止与仅杀父进程的结果分离 |
| HTTP 来源/媒体类型 | 私有 HTTP 实例 + mock 管理方法 | text/plain POST 200，OPTIONS 404，未触发真实管理操作 |
| 插件发布目录与资源排除 | 最终 ZIP 内容检查 + 新目录加载 | 完整包两插件可用；缺失行为见 3.3 |

本轮没有重新执行全量 CTest、完整 C++ 构建、真实 DSH 包对话和浏览器像素测试。前一轮的构建、30 项 CTest、DSH、响应式页面和源码 ZIP 移位编译记录，见 [管理功能验收-20260916.md](管理功能验收-20260916.md) 及交付目录的 `最终交付验证.md`，属于历史验证证据。

DeepSeek 把其 `initdb`、文件替换、浏览器管道权限等限制标为“环境受限”是正确的；它未复现历史验收并不推翻那些验收，也不能将未验证写成新增缺陷。本轮真实 PostgreSQL 报告测试及完整 11 项打包测试通过，补充了其中一部分环境差异证据。

本机诊断产物（`out/` 被忽略，不参与正式测试收集或交付）：

| 路径 | 用途 |
| --- | --- |
| `out/deepseek-review-20260917/diag_timeout.py`、`diag_pipes.py` | 原诊断的输出目录迁移副本 |
| `out/deepseek-review-20260917/probe_timeout_cleanup.py` | 区分整树终止和只杀父进程；记录返回码、子进程状态 |
| `out/deepseek-review-20260917/check_packaged_plugins.py` | 最终 ZIP 的资源检查及移位加载探针 |
| `out/deepseek-review-20260917/packaged-plugin-evidence.json` | 插件目录与归档检查原始结果 |
| `out/review-codex-20260917/probe_http_boundaries.py` | 隔离验证 Origin/Content-Type，不调用真实管理命令 |

## 5. 下一轮保留的事项

本节区分已确认的实现事实、文档修正和可选优化，避免讨论来回扩张成全面重构。

| 编号 | 状态/级别 | 下一步及验收依据 |
| --- | --- | --- |
| DS-2.1 | 待确认，暂不定 P1 | 在原失败环境补 taskkill 结果、存活进程及管道写端证据；任何修改同时验证返回时长和后代退出，不能只改耗时断言 |
| DS-3.1 | 已确认来源校验缺口，P2 加固 | 保持免登录前提；如实现校验，验证正常同源操作、远程 LAN 主界面、CLI 和 DSH 代理不受影响，非预期来源写操作被拒 |
| DS-3.3 | 部分资源诊断改进，P3 | 若引入预期资源清单，缺失时明确指出插件/文件；保留动态发现和只提供视图的合法插件 |
| DS-8.5.3 | 候选提取/预览优化，P3 | 让使用者看到准备应用的实际脚本，多个代码块不会无说明地选错；沿用已有服务端校验 |
| CX-01 | 项目限制文档过时，P3 | `docs/已知限制.md:48` 仍称 STP 一键部署需单独脚本。实际 `setup_offline_server.ps1:44–45,137–139,299–303` 已支持 `-Plugin stpcompare -ConverterExe ...`。改为“一次生成所选插件的一个客户端包，两个插件使用不同输出目录” |
| DOC-01 | 原审查文本修正 | 修正“固定 +5 秒”“读端导致 EOF”“仅 DSH”“Z 格式”“交付未排除 PID”等表述，保留环境实测原始数据 |
| OPT-01 | 可选维护工作 | 拆分 app.js、减少配置重复构造、路由整理、补 iframe 信任边界说明；不作为已复现功能故障或本轮部署的强制前置重构 |

报告脚本的权限边界可在统一限制文档引用 `REPORTS.md:36–38`，而不是声称项目此前未说明。任意业务程序依赖验证继续按内网真实程序验收，不能用框架模拟测试替代。

## 6. 交替审查约定与 DeepSeek 下一轮回复入口

建议下一份文档命名为 `整体审查-deepseek复核-codex-R2-20260917.md`（日期按实际执行更新）。不覆盖任何一方已有记录。

每轮只对新证据或实现变更重新判定，沿用本文件编号；新增问题使用新编号。区分“确认”“部分成立”“未复现”“已实现”“待确认”“已修复且复验”，不能把未复现等同于永久不存在，也不能把环境报错自动升级为代码缺陷。

DeepSeek 可直接用以下表格回复；如不接受某项，请补具体命令、环境和输出，而不是只重复原判断。

| 编号 | 对本轮复核：同意/异议 | 新证据或变更 | 下一状态 |
| --- | --- | --- | --- |
| DS-2.1 | 待 DeepSeek 回复 | 重点：原受限环境 taskkill 返回码、父/子进程状态；是否撤回确定 P1 和一行修复建议 | 待补充 |
| DS-3.1 | 待 DeepSeek 回复 | 区分 Content-Type 未校验、简单 POST、PUT 预检与浏览器限制；保持用户免登录要求 | 待补充 |
| DS-3.3 | 待 DeepSeek 回复 | 正常发布目录已加载成功；仅讨论部分安装缺失的诊断 | 待补充 |
| DS-8.5.3 | 待 DeepSeek 回复 | 承认现有 AST/compile 校验，再讨论候选预览与多代码块 | 待补充 |
| DS-7.1 / 7.5 / CX-01 | 待 DeepSeek 回复 | 修正归档/依赖安装表述与项目过时文档 | 待补充 |

只有“发生在支持场景、具有可复现证据、实际影响核心需求”的问题才作为发布阻断候选。完成修复后记录实现差异与相应回归结果；不以重复整套审查代替验证具体变更，也不承诺任何软件永远没有问题。

## 7. 本轮基线指纹

SHA256，便于后续区分“判断不同”与“代码已经变化”。原审查文档及以下实现文件在本轮复核期间均未修改。

| 文件 | SHA256 |
| --- | --- |
| `docs/整体审查-deepseek-20260917.md` | `ee435d67d0e690baa62ec2fd3175c01af3db9afdb00e5e4b9400a5f5f5fded86` |
| `result_service/app/providers.py` | `87e5299d64a1d665d1028fd28dfeb6e4cbbdd6ac2f6ac5c97270e1b83f99894a` |
| `result_service/app/server.py` | `33bb84aa17017ce383fe5a8051a3b007e4565b3e665d0b18ab65de15ce2fb9e1` |
| `result_service/app/reports.py` | `1ec8c34df2b14bae33116be4eef8a4ded76f9e2d20985be08fa0adcb38aae46f` |
| `result_service/app/plugins.py` | `5d7df62c76122572bebabf645942f9fffff68184922d580d7e7d529060c307ca` |
| `tools/create_source_archive.py` | `e2be38215d281c0a0f489896d74fbbb304eecbdc360024d90b811050a341356d` |
| `result_service/web/app.js` | `138ee077087533748a360535bab64f1072de58a998cfd89b66c9131a9f9ddc2c` |
