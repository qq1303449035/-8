# 本地私聊记录迁移

本版起，好友私聊和临时会话的聊天记录保存在客户端本机，由 Windows DPAPI（当前 Windows 用户）加密。服务器只暂存尚未送达的消息：接收方客户端把消息可靠写入本机后发送确认，服务器随即删除该消息及其图片关联。大厅聊天不变。

迁移前已经存在的服务器私聊（下称“旧记录”）不会立即删除。新客户端登录后自动把本账号的旧记录一次性下载到本机；某条旧记录的双方都下载完成后，服务器才删除它。

需要事先告知用户：

- 聊天记录只在当前电脑和当前 Windows 用户下可见。换电脑、重装系统或换 Windows 用户后看不到以前的记录。
- 在另一台电脑上发送的消息只保存在那台电脑上。
- 服务器与主客户端必须一起升级；数据库需要手动执行一次本迁移。

## 前提

1. 已按 `FRIENDS_MIGRATION.md` 部署 `friends_schema.sql`，并已执行 `recall_schema.sql` 和 `media_schema.sql`。本迁移会补建清除水位表和两个近期消息索引（与 `temporary_chat_clear.sql`、`temporary_chat_indexes.sql` 相同，可重复执行）。
2. 使用 InnoDB。脚本面向 MySQL 8.0.16 及以上；编号序列不依赖 AUTO_INCREMENT，5.7 重启后也不会重复使用消息编号。
3. 执行迁移的数据库账号需要 CREATE、ALTER、INDEX、SELECT、INSERT 权限。

## 部署步骤

1. 备份账号数据库，至少包括 `private_messages8`、`private_message_media8`、`private_conversation_clears8`、`media_assets8`。
2. 停止旧服务器。迁移期间不能再有旧服务器写入私聊，否则旧服务器分配的编号会与新序列冲突。
3. 在正确的账号数据库中手动执行 `local_history_migration.sql`。最后一个查询显示序列值、旧记录边界、会话索引数和图片授权数；第一次执行时前两项应相同。
4. 按下节为服务器运行账号授予权限。
5. 部署新服务器和新主客户端，并把 `appdata.version` 改为新版客户端的版本号（客户端 `update/version.h` 中的 `CHH8_RELEASE_VERSION`）。旧客户端登录时会收到更新通知，不能继续按旧方式读取服务器历史。
6. 用合成账号验证：在线收发、对方离线时发送后上线收取、撤回未送达和已送达的消息、清除临时会话、私聊图片下载、旧记录导出。

脚本可以重复执行：旧记录边界与首次图片授权回填在同一事务中提交；序列只会向前推进；会话索引补充缺少的行。图片授权只在首次创建旧记录边界时回填，且仅包含边界内未撤回、未被本人清除的消息。以后重跑不会恢复客户端已确认删除的授权，也不会给迁移后的新图片添加旧式授权。已有 `private_legacy8(id=1)` 的部署视为已经迁移，保留现有授权。执行出错须立即停止并回滚，禁止使用忽略错误继续执行的选项；断开连接也会回滚未提交的事务。`CREATE TABLE IF NOT EXISTS` 不会修复已存在但定义不同的同名表，重复执行前请先核对表结构。

## 运行账号权限

在原有权限（`user8`、`appdata`、`friend_accounts8`、`friendships8`、`private_conversation_clears8`、`media_*` 等）之外：

- `private_messages8`：SELECT、INSERT、UPDATE、DELETE（DELETE 为新增，用于删除已送达和双方都已导出的消息）。
- `private_message_media8`：SELECT、INSERT、DELETE（DELETE 为新增）。
- `private_sequence8`：SELECT、UPDATE。
- `private_legacy8`：SELECT。
- `private_nonces8`：SELECT、INSERT、DELETE。
- `private_recalls8`：SELECT、INSERT、DELETE。
- `private_contacts8`：SELECT、INSERT、UPDATE、DELETE。
- `private_export8`：SELECT、INSERT、UPDATE。
- `media_grants8`：SELECT、INSERT、UPDATE，执行 `private_media_migration.sql` 后另需 DELETE（删除已确认的图片下载授权，见 `PRIVATE_MEDIA_MIGRATION.md`）。

缺少新表或权限时，私聊返回“服务暂不可用”，控制台诊断会提示执行本迁移；登录和大厅不受影响。服务器不会自动执行 DDL。

## 新表用途

- `private_sequence8`：唯一一行，`next_id` 是最后分配的消息编号。每次发送在同一事务内用 `LAST_INSERT_ID(next_id+1)` 分配编号，并持有该行锁直到提交，所以编号按提交顺序递增，清空消息表后也不会重复。
- `private_legacy8`：唯一一行，迁移时冻结的旧记录边界 `legacy_max_id`。编号不大于它的消息是旧记录，只能通过一次性导出下载；更大的编号是待送达消息。
- `private_nonces8`：发送者、16字节 nonce、消息编号和时间，不含正文。消息送达删除后，客户端重试同一 nonce 仍得到原编号和时间，不会重复入库。服务器每小时删除7天前的凭据，每批最多10000条；一批删满说明还有积压，一分钟后继续。
- `private_recalls8`：撤回事件（接收者、撤回者、消息编号）。接收者确认后删除。接收方客户端只在本机该编号消息的发送者等于撤回者时才撤回。索引 `private_recall_pair` 供撤回时检查同一事件是否已在排队；较早执行过本迁移的数据库重新执行一次即可补建。
- `private_contacts8`：临时会话索引（本人、对方、最后消息编号和时间），不含正文。每次发送为双方各更新一行；清除临时会话删除本人一行。迁移时从现有私聊回填，跳过本人已清除的部分。
- `private_export8`：每个账号的一次性导出状态（导出到的编号、完成时间）。
- `media_grants8`：私聊图片下载授权（账号、图片 hash）。首次迁移从旧记录边界内的私聊图片为双方回填，之后不再重建授权。执行 `private_media_migration.sql` 后它表示待下载授权：新发送的图片只为接收方写一行（并记录发送方），接收方客户端把图片保存到本机后确认，服务器删除该行；回填的旧授权（发送方为空）同样在各自确认后删除。没有授权、消息关联、收藏和近期上传记录的图片由服务器自动删除，见 `PRIVATE_MEDIA_MIGRATION.md`。

`private_messages8` 的表结构不变。新消息在 `id` 列写入序列分配的编号，不再使用 AUTO_INCREMENT；不要对该表执行 TRUNCATE 或修改 AUTO_INCREMENT 来“修复”编号。

## 旧记录的导出和删除

- 新客户端登录后按编号分页下载本账号的旧记录（每页50条，不含本人清除位置之前的部分），全部写入本机后向服务器报告完成，服务器在 `private_export8` 记录完成时间。中途断线下次登录继续。
- 一条旧记录在以下条件都满足后删除，同时删除其图片关联：本账号已完成导出；对方账号已完成导出、或已清除到该记录之后、或账号已不存在。删除在完成导出时以及之后每次登录时分批进行，每批最多500条。
- 从不再登录新客户端的账号，其旧记录一直保留。运营上可以设定保留期限后手动清理，例如删除180天前的旧记录：

```sql
DELETE a FROM private_message_media8 AS a
JOIN private_messages8 AS m ON m.id = a.message_id
JOIN private_legacy8 AS l ON l.id = 1
WHERE m.id <= l.legacy_max_id AND m.created_at < NOW() - INTERVAL 180 DAY;

DELETE m FROM private_messages8 AS m
JOIN private_legacy8 AS l ON l.id = 1
WHERE m.id <= l.legacy_max_id AND m.created_at < NOW() - INTERVAL 180 DAY;
```

  图片授权保留到各自的客户端确认为止，已下载过这些记录的一方不受影响；尚未导出的一方将永久失去这部分记录。图片关联删除后，不再被引用的图片由服务器清理（见 `PRIVATE_MEDIA_MIGRATION.md`）。
- 长期不登录的账号的待送达消息同样一直保留，可按 `created_at` 以相同方式手动清理（条件改为 `m.id > l.legacy_max_id`）。
- 查看剩余旧记录和导出进度：

```sql
SELECT COUNT(*) FROM private_messages8 AS m JOIN private_legacy8 AS l ON l.id = 1 WHERE m.id <= l.legacy_max_id;
SELECT COUNT(*) FROM private_export8 WHERE completed_at IS NOT NULL;
```

## 回退

已送达或双方都已导出的消息从服务器删除后无法由服务器恢复，只能从迁移前的备份恢复。回退到旧服务器时必须同时恢复旧客户端和 `appdata.version`；新表可以保留，旧服务器不会读取它们。之后再次升级前要重新执行本迁移，使编号序列追上旧服务器期间写入的消息。

注意：重新执行不会移动旧记录边界（`legacy_max_id` 只在第一次执行时写入）。再次升级后，旧服务器期间写入的私聊编号都大于边界，被当作待送达消息：只推送给接收者一次（即使在旧客户端已读过，也作为新消息出现），接收者确认后即从服务器删除，发送者本机不会得到副本。这些消息需要双方都保留时，从迁移前的备份恢复数据库，或在重新执行迁移前删除 `private_legacy8`，使边界按当前最大编号重新冻结（新版本期间尚未送达的消息也随之变为旧记录，以导出方式到达）。只有 `private_export8` 为空、即还没有任何新客户端完成导出时才能删除 `private_legacy8`：否则服务器把这些账号当作已完整导出，而它们从未下载过新边界以下的消息，清理时会删除这些消息。同样的原因，不要手动调大 `legacy_max_id`：已完成导出的客户端每次登录只重发完成报告（116），不会重新导出。
