# Learnings

## [LRN-20260930-001] 用完整生命周期验证超时、内存额度和迁移重跑

**Logged**: 2026-09-30T16:38:05Z
**Priority**: medium
**Status**: resolved
**Area**: tests

### Summary
超时必须由正常轮询推进；图片额度须覆盖实际缓冲生命周期；迁移重跑必须考虑已确认删除的授权。

### Evidence and Context
CONTEXT: 修复战绩60秒超时、换IP组队路由、图片额度提前释放及私聊图片迁移重跑。
REFLECTION: 实际等待测试在60.016秒观察到超时；组队与会话1459次检查通过。420条媒体断言检查阻塞消费和缓冲释放时计数器仍持有额度。隔离MySQL的34项检查验证确认后的授权不会被重建，首次回填失败时边界与授权一起回滚。
LESSON: 回归测试应包含正常轮询、阻塞消费者以及迁移后确认/新消息等中间状态；只验证函数返回或连续执行两次空库迁移不能覆盖这些问题。

### Suggested Action
修改相关逻辑时运行 tests/session_regression_tests.cpp、tests/media_budget_tests.cpp 和 tests/media_migration_tests.py；媒体测试使用数据库桩，迁移测试只执行实际回收引用检查SQL，不等同于线上传输压力测试。

### Metadata
- Source: reflection
- Related Files: sessions.h, media_session.h, local_history_migration.sql, tests/session_regression_tests.cpp, tests/media_budget_tests.cpp, tests/media_migration_tests.py
- Pattern-Key: tests.session-buffer-migration-lifecycle
- Recurrence-Count: 1
- First-Seen: 2026-09-30
- Last-Seen: 2026-09-30

### Resolution
2026-09-30：上述回归及Release/x64构建通过；存量空发送方授权不能可靠区分来源，修复脚本不自动删除。

## [LRN-20260918-004] 高频快照先按修订号缓存再考虑连接池

**Logged**: 2026-09-18T08:02:00+08:00
**Priority**: medium
**Status**: resolved
**Area**: backend

### Summary
服务端主循环和社交刷新会频繁读取在线人数、主连接及游戏状态；这些值只在对应修订号变化时改变。按修订号缓存快照可以去掉无变化轮询中的集合构造，同时不改变连接生命周期或数据库故障语义。TLS record 缓冲也可在单线程 Stream 内复用，但关闭时必须擦除最后一份明文/加密工作区。

### Evidence and Context
服务器上限为 200 个连接，主循环约每 200ms 检查一次；排行榜查询最多 4096 行。Release/x64 构建通过，TLS/快照修改未改变协议布局。数据库连接复用仍需独立的线程、重连和停止时序压测，未在本轮引入。

### Suggested Action
后续性能工作优先用修订号或不可变快照降低锁内分配，再评估数据库连接池；新增缓存时为 enter/leave/invalidate 和 secondary game revision 补充回归覆盖。

### Metadata
- Source: reflection
- Related Files: sessions.h, network/tls_transport.h, hero_ranking.h
- Pattern-Key: revision-keyed-session-cache-and-reused-tls-buffer
- Recurrence-Count: 1
- First-Seen: 2026-09-18
- Last-Seen: 2026-09-18

### Resolution
2026-09-18：在服务器中加入按 revision/game_revision 的在线快照缓存，并复用 TLS record 缓冲；Release/x64 编译及启动帮助回归通过。

## [LRN-20260916-001] 控制台中文必须验证实际屏幕缓冲区

**Logged**: 2026-09-16T23:52:42+08:00
**Priority**: medium
**Status**: resolved
**Area**: tests

### Summary
正确解码捕获的 UTF-8 管道输出，不能证明 Windows 控制台中的中文显示正确。

### Evidence and Context
用户确认登录记录正常后仍报告服务器控制台乱码。旧验证仅捕获 stdout/stderr；本次改为控制台句柄使用 WriteConsoleW，文件/管道保留原 UTF-8 输出。隐藏真实控制台在代码页 936、437、65001 下通过中文、混合数字、分片 UTF-8、长输出及实际 EXE 的帮助和错误提示检查。使用 ReadConsoleOutputCharacterW/ReadConsoleOutputW 验证字符；不等同于验证远程机器的字体。

### Suggested Action
控制台输出变更同时验证真实 console 和重定向输出。不要单凭 subprocess.capture_output 的 UTF-8 解码结果声称控制台显示已验证。

### Metadata
- Source: user_feedback
- Related Files: console_output.h, startup_diagnostics.h, D:/x96dbg/chh8d-console-unicode/console-unicode-tests.cpp
- Pattern-Key: tests.windows-console-vs-utf8-pipe
- Recurrence-Count: 1
- First-Seen: 2026-09-16
- Last-Seen: 2026-09-16

### Resolution
2026-09-16：真实 console 的 2912 项断言通过；现有 7 项启动输出回归通过。保留字体和远程运行环境的验证边界。

## [LRN-20261004-001] 在线秒数的完整无符号范围需要真实数据库验证

**Logged**: 2026-10-04
**Priority**: medium
**Status**: resolved
**Area**: backend

### Summary
MySQL 8.0.39 的预处理语句中，字符串绑定参数直接使用 CAST(? AS UNSIGNED) 在本工程的保存查询里对超过 INT64_MAX 的数返回1292；先 CAST(? AS CHAR) 再 CAST AS UNSIGNED 后保持完整uint64精度。

### Evidence and Context
CONTEXT: 为 user8.onlineSeconds 添加累计在线时长和排行榜，字段为BIGINT UNSIGNED。
REFLECTION: 计时和回包测试通过后，独立临时数据库的InnoDB/MyISAM测试仍发现12项高位整数写入失败；改为双CAST后60项真实数据库检查全部通过。仅为本查询和该MySQL版本的实测结果，不推断所有版本行为。
LESSON: 无符号计数器除C++/协议测试外，应通过真实预处理语句往返验证2^53+1、2^63+1及UINT64_MAX。

### Suggested Action
保留 tests/run_online_time_database_tests.ps1 中的精度与幂等测试；累计时长的检查点以固定基数计算，未知提交结果重试不重新读取基数。当前账本只保证单服务进程对同账号的统计，跨进程汇总需另设计。

### Metadata
- Source: reflection
- Related Files: online_time.h, online_time_store.h, tests/online_time_database_tests.cpp
- Pattern-Key: backend.mysql-prepared-unsigned-checkpoint
- Recurrence-Count: 1
- First-Seen: 2026-10-04
- Last-Seen: 2026-10-04

### Resolution
2026-10-04：双CAST查询通过独立MySQL 8.0.39的60项检查；服务端Release/x64编译通过，计时74项、榜单回包115项、现有权限接口139项检查通过。
