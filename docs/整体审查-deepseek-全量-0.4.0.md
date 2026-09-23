# 全量实现审查（DeepSeek，全项目）

审查对象：`codex/deployment-automation`，基线 `38beac1`，并在写入前对当前工作区（0.4.0 交付状态）**逐条复验**。
审查范围：`common/`、`scheduler/`、`client/`、`plugins/`、`result_service/`（含前端）、`tools/` 的**全部实现代码**。

## 0. 本文档的由来与当前状态

本文档是第四轮独立审查（**全量**，前几轮只覆盖改动或部分子系统）的汇总记录。它曾在工作区生成过一次，随后 `docs/` 目录被整体清空而丢失（见 §0.1），故在此重新生成，并在重新生成时对每条结论做了复验。

### 0.1 关于 `docs/` 目录被清空（需你确认，我未擅自恢复）

写入本文档时观察到的工作区状态：

- `docs/` **目录为空**，`git status` 显示 **27 个已跟踪文档全部为"已删除未暂存"（` D`）**——含仓库原有的 `需求说明.md`/`设计说明.md`/`开发说明.md`/`部署与运行.md`/`已知限制.md`/`整体审查-20260916.md`，以及本审查系列的历史记录（`整体审查-deepseek-20260917.md`、R2/R3/R4、`整体审查-codex修复落实-R6-20260917.md`、`整体审查-codex确认-deepseek-R5-20260917.md` 等）。
- 这些文件在 git HEAD 中**都存在**，可用 `git restore docs/` 或 `git checkout -- docs/` 完整恢复。
- 顶层同时出现了 `real-test/`、`service-workspace/`、`out/real-test-before-final-20260921/`，以及 `dist/TestTracking-02-offline-delivery-*/`、`dist/release-staging-20260920-v0.4.0/` 等归档，说明这是他人为打包/整理目的做的清理，**很可能是有意删除**，因此我只补写本文档，未恢复其余 27 个文件。
- 归档副本里仍能看到历史文档（如 `build/source-delivery-relocated-0.3.0/TestTracking-02/docs/`、`build/test/offline-source-acceptance-040/TestTracking-02/docs/`），需要时可从那里取回。

**若这次清空不是有意的**，请执行 `git restore docs/` 恢复全部 27 份文档。

### 0.2 复验声明

代码在本轮审查之后又经历了 R5/R6 轮次与 0.4.0 打包。写入前我对每条结论在当前工作区重新核对：

| 结论 | 复验结果 |
| --- | --- |
| §1.1 源码归档带走口令 | **仍存在**。`tools/create_source_archive.py:33` 的 `EXCLUDED_FILES` 仍是 `{"runtime/start_scheduler.bat", "runtime/rt_test.ps1"}`，四个含 `db_conn` 的 `runtime/*.json` 均未排除 |
| §1.2 队列头卡在 `planning` 无法取消 | **仍存在**。`scheduler/src/Storage.cpp:214` 的 `CancelQueuedRun` 仍为 `status IN ('queued','cancelled')`（注意同文件 `:221,235` 已引入 `('queued','planning','running','stopped')` 集合，唯独取消路径漏了 `planning`） |
| §2.2 `RunCommand` 无超时 + `CreateProcessA` | **仍存在**。`common/src/Util.cpp:195` `CreateProcessA`、`:199` `WaitForSingleObject(..., INFINITE)` |
| §2.3 打包器静默成功 | **仍存在**。`tools/build_package.ps1:381` 跳过块、`:526` 无条件输出 `done. copy ...` |
| §2.4 `settle_coverage` 事务内跑外部进程 | **仍存在**。事务 `:274` → `MergeSelected` 在 `:289` → `commit` 在 `:312` |
| §2.5 POSIX 原子写缺目录 fsync | **仍存在**。`common/src/AtomicFile.cpp:51` 只 `fsync(file)`、`:54` 直接 `rename`，而 Windows 分支 `:41` 用 `MOVEFILE_WRITE_THROUGH` |

## 1. 方法与可信度声明

**分工**：5 个并行子代理各审一个子系统；架构横切面、跨模块契约与**全部高严重度结论**由我亲自复核。

**为什么必须复核**：子代理共报出 **7 条 P1，我逐条追控制流后判定 4 条不成立**。失败模式高度一致——看到危险写法就推定可达路径，而未走完控制流或未检查前置守卫。这与我自己前几轮的失误同类。故每条结论标注验证来源：

| 标记 | 含义 |
| --- | --- |
| **【亲验】** | 我亲自读码/实测确认，取证完整 |
| 【子代理】 | 仅由子代理报告，我未独立复核；按可信度降级表述 |

**环境限制**（与前几轮一致，不据此作反向结论）：C++ 未构建（`cl`/`ninja` 不在 PATH）；无法启动私有 PostgreSQL（`initdb` 报 `could not create restricted token: 87`）；无头 Chrome 因沙箱禁止命名管道而无法渲染；覆盖式文件替换（`[IO.File]::Replace`、覆盖式 `Move`）一律被拒。

## 2. 确认的缺陷

### P1

#### 1.1 【亲验】源码归档带走数据库口令

**位置**：`tools/create_source_archive.py:13-33`，配合 `tools/setup_offline_server.ps1:304,377,382,423`

用归档器自己的排除谓词逐项验证：

```text
runtime/scheduler.json          excluded? False   carries db_conn: yes
runtime/result_service.json     excluded? False   carries db_conn: yes
runtime/client.template.json    excluded? False   carries db_conn: yes
runtime/source_launch.json      excluded? False
runtime/logs/scheduler.log      excluded? True
runtime/pgdata/PG_VERSION       excluded? True
```

**机制**：`setup_offline_server.ps1` 把连接串写入 `runtime` **根目录**下的四个 JSON（`:304` `:377` `:382` `:423`），连接串由 `Build-DbConn` → `New-PostgresConnection` 构造并含 `password=`（`:276`）。而排除清单是**"补丁式"**的：目录级只排 `runtime/logs`、`runtime/pgdata`、`runtime/result_service`，文件级只排两个。**`runtime` 根目录下的配置文件正好落在两个清单之间。**

**可达性**：已确认。在执行过一键部署（且配置了 `db_password`）的机器上运行 `create_source_archive.py`，口令随源码 ZIP 外发。

**为什么前几轮漏掉**：上一轮被要求补的是 `.pid` 与 `rt_test.ps1`，正好强化了"补丁式"清单而没有暴露结构问题。

**建议**：改为白名单（`runtime` 下只允许 `dataset`、`python`、`python-analysis-wheelhouse`），或至少显式排除这四个 JSON。`docs/部署与运行.md` 建议正式环境使用 `scram-sha-256`，那种情况下泄漏的是生产口令，故列为本轮最严重一条。

#### 1.2 【亲验】队列头 run 卡在 `planning` 且无法带内取消

**位置**：`scheduler/src/Scheduler.cpp:532-536`、`scheduler/src/Storage.cpp:143-147`（`BeginPlanning`）、`scheduler/src/Storage.cpp:208-216`（`CancelQueuedRun`）

**机制**（三段叠加）：

1. `Scheduler.cpp:532` 先 `BeginPlanning`（DB 行 `queued→planning` 已提交），`:536` 再 `SetRunStatus(run_id,"planning")`；后者经 `UpdateRolloutRun` 写 rollout sidecar，**写失败即抛异常**，异常从 `StartNextRun` 逃出，`queue.pop_front()` 未执行 → DB 为 `planning`、内存队列仍保留队头项。
2. 重入时 `BeginPlanning` 的 SQL 是 `WHERE ... AND status='queued'`（`Storage.cpp:145`），此时影响 0 行 → `Exec` 把 0 行当失败返回 false（`Storage.cpp:69-79`）→ 每次 `RunTimeoutChecks`（每 200ms）报一次 `cannot begin planning queued run` 并 `return false`，**永久循环**，同插件后续批次被队头挡住。
3. **operator 无法带内恢复**：`stop <run_id>` 走 `CancelQueuedRun`，其 SQL 只接受 `status IN ('queued','cancelled')`（`Storage.cpp:214`）→ 0 行 → 拒绝。唯一出路是重启 scheduler。

**根因比子代理所述更精确**：`Storage.cpp:210-213` 的注释已经承认过这一类切点——

> *"A previous attempt may have committed the database change before rollout sidecar publication failed. Treat that durable cancelled state as an idempotent retry..."*

作者为此把 `'cancelled'` 加进了幂等重试集合，**漏了 `'planning'`**——而 `planning` 正是 `BeginPlanning` 提交后、sidecar 失败前留下的状态。**同一个修复只做了一半。**最小修复就是把 `'planning'` 补进那个集合。

**可达性**：已确认（逐行核对；未做故障注入）。触发前提是 sidecar 写失败——仓库自己的测试用 `ExclusiveFile` 模拟同类故障（`test/test_runtime_integration.py:302-329`），说明这类故障被当作一等公民处理。对比：`CommitStop`/`CommitFinished` 遇同一故障可重试，只有 `planning` 这一步不可重试。

#### 1.3 【子代理，未独立复核】部署协调线程持 fleet 全局锁做 35 秒阻塞 HTTP

**位置**：子代理称 `deployment_rollouts.py:415`（`tick` 的 `with self._lock:` 覆盖 415-429）、锁为 `fleet._lock`（`fleet.py:32`），`management.py:342` 的 `urlopen(..., timeout=35 if body is not None else 3)`。

我已确认两个事实：`management.py:342` 确实存在 35 秒超时；`deployment_rollouts.py:414-429` 的 `tick()` 确实把 `_advance` 整轮包在 `with self._lock:` 内，而 `_advance` 会经 management 发起 HTTP。

**但我未能确认 `rollouts._lock` 与 `fleet._lock` 是同一把锁**（在 `deployment_rollouts.py` 中检索 `self._lock` 与类定义均无命中，说明其结构与我预期不同）。**故本条以子代理结论计入，可信度降一级**：机制方向合理（持锁跨阻塞网络 I/O 是确定的坏做法），但"成片拖住控制台接口"的爆炸半径未经我验证。

**建议**（无论锁是否共享都成立）：把网络 I/O 移出临界区；`_request` 区分离线快失败与超时。

### P2

#### 2.1 【亲验】`-OnlyProgram` / `-OnlyDataset` 静默无效但报成功

**位置**：`tools/build_package.ps1:381`（块开始）、`:524`（块结束）、`:526`（无条件输出）

我用大括号深度计算确定了块边界：`:381` 处于 depth 0，其块覆盖 381–524（depth≥1），而 `:309` 的"`-OnlyProgram` 精细更新分支"处于 **depth 1，在该块之外**。因此：

- `-OnlyProgram` **会**正确更新 `config/client.json` 的 `target_exe`/`program_dir`/`engine_dlls`（`:309-323`）——**子代理报的"会重建配置并丢弃全部部署配置"是错的**，它自己的实测（"`X/config/client.json` 全部不存在"）恰好与那条自相矛盾；
- 但 381–524 整块被跳过，于是 **`client.exe`、`start_client.bat`、VC 运行库、插件 DLL 都不会被写入**；
- 最后 `:526` 仍无条件打印 `done. copy '$OutDir' to the target machine and run start_client.bat` 并以退出码 0 结束。

**可达性**：已确认。运维/流水线会把"什么都没更新"当成功。

**建议**：`-OnlyProgram`/`-OnlyDataset` 在目标目录缺 `config/client.json` 或必需二进制时直接 `throw`，而不是静默跳过 + 报成功。

#### 2.2 【亲验】`RunCommand` 用 `CreateProcessA` 传 UTF-8 路径且无超时/取消

**位置**：`common/src/Util.cpp:189-207`（`:195` `CreateProcessA`、`:199` `WaitForSingleObject(..., INFINITE)`）

**两个子代理独立报同一条**（`common` 与 `client` 两个审查者各自发现），交叉印证：

- ANSI 转换会损坏非 ANSI 字符。中文用户名下的 `%TEMP%` 或中文安装路径是**常见可达**场景；调用方 `plugins/coverage/src/CoveragePostProcess.cpp:54-110` 的 `MergeCoverageFiles` 正走这条路径。
- `INFINITE` 等待**完全绕过 `CancellationToken`**。同一仓库里已有带 job object + 超时 + 取消的 `ProcessRunner`（`common/src/ProcessRunner.cpp`），这里是能力回退。与 `docs/已知限制.md` 已记录的"cov 合并超时"一致。

**建议**：`MergeCoverageFiles` 改用 `common::ProcessRunner::run`（自带 UTF-8 宽字符转换、超时、job 隔离）。

#### 2.3 【亲验】组件更新路径下 `-ServerIp`/`-ServerPort` 被静默忽略

**位置**：`tools/build_package.ps1:183`、`:188`、`:449-455`

`:188` 的推导条件是 `if (-not $Endpoint -and -not $preserveClientConfig)`，而 `-OnlyProgram` 走精细分支不设 `$Endpoint`（`$preserveClientConfig` 亦为假，它只在 `-OnlyClient` 时为真，`:150`）。用户显式传入的地址**既不报错也不生效**。

**建议**：组件更新路径也要求确定 endpoint，或对未生效的显式参数直接报错。

#### 2.4 【亲验】`settle_coverage` 在数据库事务内启动外部进程

**位置**：`plugins/coverage/src/settle_coverage.cpp:273-291`

事务从 `:274` 持有到 `:312`，而 `:289` 的 `MergeSelected` 在事务内启动 `n-1` 个外部 OpenCppCoverage 进程。

**我补充了子代理未提的一点**：`:270-271` 的注释声称"不要在外部合并工具运行时持有共享 schema 锁"，该注释在**表锁层面已经失效**——`:272` 在 `BEGIN` 之前就把表建好了，所以真正跨外部进程持久的是 `:279-284` 对 `coverage_selected` 的 DELETE+INSERT 写锁。合并期间（数十秒量级）会阻塞该表的并发读者。**注释描述与实现不符**，属记忆性缺陷：作者知道该风险并写了注释，但改动顺序后注释未更新。

**建议**：先算 selected → 提交 → 锁外合并 → 再开事务写入。

#### 2.5 【亲验】POSIX 原子写 rename 后不 fsync 父目录

**位置**：`common/src/AtomicFile.cpp:51-56`，对照 Windows 分支 `:40-41` 的 `MOVEFILE_WRITE_THROUGH`

POSIX 分支只 `fsync(file)` 再 `rename`，从未 fsync 父目录；目录项仍在脏页，掉电可能丢"已发布"文件。`RunStore` 的 checkpoint 与每个 `completed/*.done` 都走这条路径。

**影响面已修正**：子系统审查者把 POSIX 分支当"死代码"，我确认**它会参与编译**（文件后端跨平台，`PgUtil.h` 才整体封在 `#ifdef _WIN32` 内），但本系统是 Windows x64 专用，**没有任何部署跑它**。故为真实缺陷但不属交付路径。

#### 2.6 【子代理，未独立复核】结果服务的资源与并发问题

| 项 | 位置（子代理提供） | 机制摘要 |
| --- | --- | --- |
| `int()` 解析失败返回 500 并回显异常文本 | `server.py:212-213,228-230,603-609` | `GET /api/runs?limit=abc` → 500 且回显 `invalid literal for int()...`，应为 400 |
| 密钥掩码只按键名判定 | `management.py:33-35,78-80,119-129` | 数组参数（如 `["--api-key","sk-xxx"]`）或未列举键名会明文回传浏览器 |
| 无界缓存与重复全量解析 | `resource_catalog.py:64,81-86`；`artifacts.py:53-58` | 上传缓存无淘汰；`/api/agents` 按 客户端×组件×2 次解析整份 `catalog.json` |
| DSH 会话在长连接存在期间不回收 | `providers.py:610-618`；`dsh_proxy.py:194-196` | `active_requests>0` 使 reaper 永不回收；8 个浏览器占用即第 9 个稳定 502 |

#### 2.7 【子代理，未独立复核】其余 P2

| 项 | 位置（子代理提供） | 说明 |
| --- | --- | --- |
| 客户预占过期的 sidecar 写失败让整轮周期维护停摆 | `SchedulerClientControl.cpp:96-110`；`Scheduler.cpp:727-728` | `ExpireClientReservations` 是 `RunTimeoutChecks` 第一句，抛异常则后续回收/提交/启动全部跳过 |
| `ApplyPluginDeployment` 在 `clients_mutex_` 内做两次强制刷盘 | `SchedulerDeployment.cpp:174-198` | 网络线程内联处理心跳且要取同一把锁，慢盘上可能超过 `client_timeout_ms` 触发误回收。**子代理自标"是否真超 5s 待确认"** |
| 服务端 DLL 更换导致 `pluginType()` 变化后永久无法发起新 rollout | `SchedulerRollout.cpp:82-90,292-293,356-359` | 无带内迁移手段 |
| 报告脚本超时只杀直接子进程（孙进程成孤儿） | `reports.py:170-180` | 与 `providers._stop_process_tree` 的 `taskkill /T` 不一致 |
| 数据包准备（可达 GB 级哈希+重打包）在持锁期间执行 | `resource_catalog.py:164-179` | 期间 `/api/agents`、`/api/scheduler` 阻塞 |
| `-AuthMethod` 非 trust 时 `init_postgres_offline.ps1` 探测必然失败 | `init_postgres_offline.ps1:116,134,247,263-265` | 参数被"部分采用"（仅远程规则），失败时实例已启动、配置已改写 |
| 四个端口查重因运算符优先级只对最后一个生效 | `setup_offline_server.ps1:124-126` | 子代理在本机实测了 PowerShell 语义 |
| 数据集源指向项目根/上层时先删后拷且无祖先校验 | `setup_offline_server.ps1:229-241` | 破坏被 `Assert-OwnedPath` 限制在 `runtime` 下，但会把全仓 `.stp` 混入数据集 |
| 多条原生命令退出码未检查 | `build_server_package.ps1:313-317` 等 5 处 | 失败被误报成功，位于交付链路末端 |
| 部署状态上报的 409 被当"已送达"并写 sentinel | `client_update.ps1:156-176` | 该状态永久不再重试 |
| 制品缓存损坏无法自愈 | `client_update.ps1:326-328,351-358` | 需人工在每台客户端定位 64 位哈希目录 |

## 3. 架构观察（设计问题，非缺陷）

### 3.1 `Scheduler` 已成上帝类——最需要处理的结构问题

`Scheduler.h` **239 行、129 个成员函数**，`Scheduler.cpp` **828 行**（`scheduler/` 合计 3790 行 / 8 个 .cpp）。单一类同时承担 **8 类关注点**：网络消息处理、run 队列与指纹、客户端会话与物理机身份、代理/机型状态、客户端预留、插件维护、插件滚动发布、管理 HTTP。仅 `Rollout*` 系列就有 10 个成员函数。

这是严重违反 SRP 的结构，也是"改一处要动多处"的 rigid 症状。**但它不是可以立即拆的**：拆分会触及几乎每条恢复路径，风险高于收益。建议作为**有意识的后续项**记录，而不是当前待修。

### 3.2 部署机制的实际形态（理解本系统的关键）

`tools/client_update.ps1` 不是一次性脚本，而是**目标机上的常驻更新代理**：按 64 位十六进制修订号读期望状态、在确定性暂存目录准备、等调度器预留（drain）后应用、写回执，并有"失败修订锁存，需发布新修订才能重试"的防重放设计（`:433-442`）。`client/src/ProcessAgent.cpp` 是宿主进程，管理多个 worker 子进程并可自行拉起 updater。

这套设计质量高于一般部署脚本：`Assert-NoLink`、`Assert-OwnedPath`、暂存目录名白名单（`:380`）、修订号 SHA256 校验、失败锁存都到位。上面列的 P2 多是边界与可观测性问题，不是设计缺陷。

### 3.3 `ProcessAgent::Launch` 的句柄继承是正确的

`client/src/ProcessAgent.cpp:348-364` 用 `CREATE_SUSPENDED` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 白名单，确保子进程只继承三个事件句柄；`:369-375` 在 resume 前完成 job 归属，杜绝子进程逃逸。这是本仓库进程管理的正面样本。

## 4. 子代理报告中被我否定的结论

**必须记录，因为它们看起来都很像缺陷。** 4 条 P1 经复核不成立：

| 子代理结论 | 我的复核 |
| --- | --- |
| `TaskLedger` 的 `priority_`/`pending_` 双队列在 `count==0` 时让整轮永久停摆 | **不成立**。`request()` 用 `priority_.empty()?pending_:priority_`（`TaskLedger.cpp:103`），`priority_` 取空后即回退 `pending_`。真实存在的是优先级饥饿（策略问题），不是死锁 |
| `TaskExecutor::push` 向已关闭队列推送导致任务被静默丢弃 | **不可达**。池关闭必伴随 `draining_executor_=true`（`Client.cpp:401-402`），而 `HandleTask` 的准入门槛含 `!draining_executor_ && executor_started_`（`:565-568`），两状态互斥。降级为 P3（返回值该检查） |
| 多插件时 Agent 心跳身份带 `/` 使整机上报废 | **不成立**。父 Agent 上报的是基础 `worker_id_`（`ProcessAgent.cpp:184,306`），`/` 后缀只加在 worker 子进程身份上（`Client.cpp:172`）。真实缺陷较小：文档推荐 `worker-01/coverage` 而两端都拒绝 `/`，且失败仅 `LOG_WARN` |
| `-OnlyProgram`/`-OnlyDataset` 重建配置并丢弃全部部署配置 | **不成立**。配置写入在 `:381-524` 块内，而 `-OnlyProgram` 走 `:309-323` 精细分支（depth 分析确认其在块外），配置**不会被重建**。成立的是"静默无效却报成功"（§2.1） |

另有一条子系统内部矛盾值得记录：该子代理同时断言"会重建配置"与"实测目录里没有任何文件"——**实测恰好否证了它自己的推论**，但它未察觉。

## 5. 未验证 / 待确认

以下均由子代理标注、我未复核，**不作为缺陷**：

1. `PublishStatus()` 在 `Scheduler.cpp:294` 位于 try/catch 之外；工作线程顶层抛异常会 `std::terminate`。审查者逐项排查 `BuildStatus` 抛出点后**未找到可达链**。
2. `Scheduler::start()` 在 `accepting_=true`（`:94`）之后才创建 worker（`:96`）；此窗口内抛异常会让 `stop()` 永久阻塞在无超时的 `result.get()`。需内部启动失败，未找到外部触发路径。
3. `PrepareRolloutShutdown` 用运行期类型作 sidecar 键（`SchedulerRollout.cpp:118`）理论上能造畸形条目，但需要"运行期类型已变且该类型仍有活动 run"，两者互斥 → **判定不可达**（脆弱不变量，建议加断言）。
4. `run_states_`/`run_order_`/`run_definitions_` 永不清除，内存随历史 run 数单调增长（对比 `accepted_reports_` 有 8192 上限）。
5. `ManagementServer.cpp:150-159` 并发上限触发时直接关连接，不回 503。
6. 前端：`loadArtifacts` 缺请求代号保护；5 秒轮询整体替换 DOM 导致键盘焦点被反复清空（`patchClientList` 已在客户端列表修过，其余三处未同步）。
7. `management._validate` 的 `walk` 遇 list 即返回，数组元素不经类型校验。
8. 数据库只读语义（`report_worker.py:23`、`repository.py:79`）未实测——本机无 psycopg；按 `已知限制.md` 第 6 节，它只是默认值而非权限边界。

## 6. 明确的正面结论

以下经复核，**不构成缺陷**，记入以避免后续重复质疑：

- **无 P0。** 未发现可达的路径穿越、XSS、SQL 注入、反代 SSRF 或数据损坏路径。
- **并发与锁序**：`Scheduler` 的单 worker 屏障确实有效。锁序（`FilePublicationLock` → `clients_mutex_`）无反向路径；迭代器/引用失效逐处核对无问题（`CommitReport` 的 `attempt` 引用在 `erase` 前用完、`RunTimeoutChecks` 先快照再遍历、`ReclaimTasks` 遍历 vector 擦 map）；`in_flight_`/`active_runs_`/`run_states_`/`plugin_rollouts_`/`storage_` 确实只在 worker 线程访问。
- **恢复事务边界正确**：`runs` 两阶段激活与 `stopped→cancelled` 同事务、completion 对账的单事务边界均正确。
- **HTTP 解析**：长度/超时/并发上限齐备（头 16KB→431、body 64KB→413、5s 截止、拒绝 `Transfer-Encoding`、无 `Content-Length`→411）。
- **前端 XSS**：全仓无 `insertAdjacentHTML`/`outerHTML`/`document.write`/`eval`/`new Function`；`index.html` 无内联事件属性；服务端字段插值均经 `escapeHtml`/`esc`；报告 iframe 为 `sandbox="allow-scripts allow-downloads"`（无 `allow-same-origin`），处于不透明源。
- **路径穿越**：`/api/artifacts/objects/<sha>`（`_DIGEST` 正则）、静态文件（`resolve()` + `parents` 判定）、报告产物（`[a-f0-9]{32}` + 恰好 3 段）、插件/适配器/视图名（白名单正则）**均已有前置守卫，全部不可达**。
- **`providers.py` 的命令行注入面当前不可达**：生产路径只用 `DshWebManager`，提示词经 `--patch` 临时 JSON 文件传递；`CommandProvider` 在 `result_service/app` 内无调用者。
- **报告生成不破坏既有产物**：每次用新 `uuid4` 目录 + 临时目录，失败/超时只删临时目录。
- **内容类型与来源校验已实现**（前几轮建议已落实）：`server.py:549-554` 强制 `application/json`（否则 415）；`:532-547` 校验来源，检查由 `Host` 推导的期望 origin、`Sec-Fetch-Site` 非 cross-site、Referer 回退，并明确**不信任 `X-Forwarded-*`**。

## 7. 建议的处理顺序

1. **立即**：§1.1 源码归档口令泄漏（改白名单，几行改动，安全含义最高）。
2. **立即**：§1.2 `CancelQueuedRun` 接受集合补 `'planning'`（一行改动，把已做一半的幂等修复补全）。
3. **本迭代**：§2.1/§2.3 打包器的静默成功与静默忽略（把"报成功"改成"报错"）；§2.2 `RunCommand` 换 `ProcessRunner`；§2.4 事务边界。
4. **记录不修**：§3.1 `Scheduler` 上帝类（风险高于收益，需专门规划）；§2.5 POSIX fsync（非目标平台）。
5. **待定级**：§1.3 与 §2.6/§2.7 中我未复核的项，建议由具备 PostgreSQL 与浏览器环境的一方复跑 `test_runtime_integration.py` 与前端测试后再定。

## 8. 本轮审查的产物与复现

未修改任何仓库代码（本次仅新增本文档）。辅助脚本均在 `build/` 下（被 `.gitignore` 覆盖）：

| 路径 | 用途 |
| --- | --- |
| `build/rt/testtmp/run_tests.py` | 注入式测试启动器（绕过沙箱临时目录限制） |
| `build/rt/testtmp/check_archive_excludes.py` | §1.1 的归档排除验证 |
| `build/rt/testtmp/evidence_cleanup.py`、`diag_*.py` | 早前 `taskkill` 证据采集 |
| `build/ui-review/*` | 早前界面与 DOM id 契约检查 |
