-- 本地私聊记录迁移：服务器改为只保存尚未送达的私聊，历史记录由客户端加密保存在本机。
-- 在账号数据库中手动执行一次；服务器不会自动执行。先停止旧服务器并备份数据库。
-- 前提：已执行 friends_schema.sql、recall_schema.sql、media_schema.sql。步骤见 LOCAL_HISTORY_MIGRATION.md。
-- 可以重复执行：旧记录边界和图片授权回填在同一事务内首次完成；以后不重建已确认删除的授权。
-- CREATE TABLE IF NOT EXISTS 不会修复已存在但定义不同的同名表，重复执行前请先核对。

-- 清除水位表（与 temporary_chat_clear.sql 相同），旧记录导出和会话索引回填都依赖它。
CREATE TABLE IF NOT EXISTS private_conversation_clears8 (
  owner_id VARBINARY(64) NOT NULL,
  peer_id VARBINARY(64) NOT NULL,
  clear_through_id BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (owner_id,peer_id),
  CONSTRAINT private_clear_other CHECK (owner_id <> peer_id)
) ENGINE=InnoDB;

-- 每个在线会话每秒按接收者读取待送达消息，必须有近期消息索引（与 temporary_chat_indexes.sql 相同）。
SET @local_history_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.statistics
    WHERE table_schema=DATABASE() AND table_name='private_messages8' AND index_name='private_sender_recent'),
  'SELECT 1',
  'ALTER TABLE private_messages8 ADD INDEX private_sender_recent (sender_id,id)');
PREPARE local_history_statement FROM @local_history_ddl;
EXECUTE local_history_statement;
DEALLOCATE PREPARE local_history_statement;

SET @local_history_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.statistics
    WHERE table_schema=DATABASE() AND table_name='private_messages8' AND index_name='private_recipient_recent'),
  'SELECT 1',
  'ALTER TABLE private_messages8 ADD INDEX private_recipient_recent (recipient_id,id)');
PREPARE local_history_statement FROM @local_history_ddl;
EXECUTE local_history_statement;
DEALLOCATE PREPARE local_history_statement;
SET @local_history_ddl = NULL;

-- 消息编号序列。next_id 保存最后一个已分配的编号；清空消息表或重启 MySQL 5.7 后编号也不会重复。
CREATE TABLE IF NOT EXISTS private_sequence8 (
  id TINYINT NOT NULL,
  next_id BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (id),
  CONSTRAINT private_sequence_single CHECK (id = 1)
) ENGINE=InnoDB;

-- 旧记录边界：编号不大于 legacy_max_id 的消息是迁移前的历史，由双方各自导出后才删除。
CREATE TABLE IF NOT EXISTS private_legacy8 (
  id TINYINT NOT NULL,
  legacy_max_id BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (id),
  CONSTRAINT private_legacy_single CHECK (id = 1)
) ENGINE=InnoDB;

-- 发送防重复凭据：只保存发送者、nonce 和消息编号，不保存正文；服务器保留7天。
CREATE TABLE IF NOT EXISTS private_nonces8 (
  sender_id VARBINARY(64) NOT NULL,
  nonce BINARY(16) NOT NULL,
  message_id BIGINT UNSIGNED NOT NULL,
  created_at DATETIME NOT NULL,
  PRIMARY KEY (sender_id,nonce),
  KEY private_nonce_age (created_at)
) ENGINE=InnoDB;

-- 已送达消息的撤回事件，接收者确认后删除。
CREATE TABLE IF NOT EXISTS private_recalls8 (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  recipient_id VARBINARY(64) NOT NULL,
  sender_id VARBINARY(64) NOT NULL,
  message_id BIGINT UNSIGNED NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY private_recall_recipient (recipient_id,id),
  KEY private_recall_pair (recipient_id,sender_id,message_id)
) ENGINE=InnoDB;

-- 撤回时在对方账号锁内检查同一事件是否已在排队；没有此索引时要扫描对方的全部待送达事件。
SET @local_history_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.statistics
    WHERE table_schema=DATABASE() AND table_name='private_recalls8' AND index_name='private_recall_pair'),
  'SELECT 1',
  'ALTER TABLE private_recalls8 ADD INDEX private_recall_pair (recipient_id,sender_id,message_id)');
PREPARE local_history_statement FROM @local_history_ddl;
EXECUTE local_history_statement;
DEALLOCATE PREPARE local_history_statement;
SET @local_history_ddl = NULL;

-- 临时会话索引：只记录谁与谁最近有私聊，不含正文。
CREATE TABLE IF NOT EXISTS private_contacts8 (
  owner_id VARBINARY(64) NOT NULL,
  peer_id VARBINARY(64) NOT NULL,
  last_id BIGINT UNSIGNED NOT NULL,
  last_at DATETIME NOT NULL,
  PRIMARY KEY (owner_id,peer_id),
  KEY private_contact_recent (owner_id,last_id),
  CONSTRAINT private_contact_other CHECK (owner_id <> peer_id)
) ENGINE=InnoDB;

-- 每个账号的一次性旧记录导出状态。
CREATE TABLE IF NOT EXISTS private_export8 (
  owner_id VARBINARY(64) NOT NULL,
  through_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  completed_at DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (owner_id)
) ENGINE=InnoDB;

-- 私聊图片下载授权：消息删除后，双方仍可下载本人收发过的图片。
CREATE TABLE IF NOT EXISTS media_grants8 (
  owner_id VARBINARY(64) NOT NULL,
  hash BINARY(32) NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (owner_id,hash)
) ENGINE=InnoDB;

-- 以下数据迁移一起提交。执行出错必须停止并 ROLLBACK，不要使用忽略错误后继续执行的选项。
-- DDL 全在事务之前；已有 private_legacy8(id=1) 表示已经完成过首次迁移。
START TRANSACTION;

-- 序列从现有最大编号继续。重复执行只会把序列向前推，不会倒退。
INSERT INTO private_sequence8 (id,next_id)
SELECT 1, seed.max_id FROM (SELECT COALESCE(MAX(id),0) AS max_id FROM private_messages8) AS seed
ON DUPLICATE KEY UPDATE next_id = GREATEST(next_id, seed.max_id);

-- 旧记录边界只在第一次执行时写入，之后保持不变。
INSERT INTO private_legacy8 (id,legacy_max_id)
SELECT 1, seed.max_id FROM (SELECT COALESCE(MAX(id),0) AS max_id FROM private_messages8) AS seed
WHERE NOT EXISTS (SELECT 1 FROM private_legacy8 AS l WHERE l.id = 1);
SET @local_history_first_migration = ROW_COUNT();

-- 回填临时会话索引：双方各一条，跳过本人已清除的部分。
INSERT INTO private_contacts8 (owner_id,peer_id,last_id,last_at)
SELECT recent.owner_id, recent.peer_id, recent.last_id, m.created_at
FROM (
  SELECT pairs.owner_id, pairs.peer_id, MAX(pairs.id) AS last_id
  FROM (
    SELECT sender_id AS owner_id, recipient_id AS peer_id, id FROM private_messages8
    UNION ALL
    SELECT recipient_id AS owner_id, sender_id AS peer_id, id FROM private_messages8
  ) AS pairs
  LEFT JOIN private_conversation_clears8 AS c ON c.owner_id = pairs.owner_id AND c.peer_id = pairs.peer_id
  WHERE pairs.id > COALESCE(c.clear_through_id,0)
  GROUP BY pairs.owner_id, pairs.peer_id
) AS recent
JOIN private_messages8 AS m ON m.id = recent.last_id
WHERE NOT EXISTS (SELECT 1 FROM private_contacts8 AS x WHERE x.owner_id = recent.owner_id AND x.peer_id = recent.peer_id);

-- 仅首次迁移回填边界内的旧图片，跳过已撤回及本人已清除的消息。
-- 重跑不能补回已确认删除的旧授权，也不能为迁移后新消息生成空 peer_id 的旧式授权。
INSERT INTO media_grants8 (owner_id,hash)
SELECT grants.owner_id, grants.hash
FROM (
  SELECT m.sender_id AS owner_id, a.hash AS hash
  FROM private_message_media8 AS a
  JOIN private_messages8 AS m ON m.id = a.message_id
  JOIN private_legacy8 AS l ON l.id = 1 AND m.id <= l.legacy_max_id
  LEFT JOIN private_conversation_clears8 AS c ON c.owner_id = m.sender_id AND c.peer_id = m.recipient_id
  WHERE @local_history_first_migration = 1 AND m.recalled_at IS NULL AND m.id > COALESCE(c.clear_through_id,0)
  UNION
  SELECT m.recipient_id AS owner_id, a.hash AS hash
  FROM private_message_media8 AS a
  JOIN private_messages8 AS m ON m.id = a.message_id
  JOIN private_legacy8 AS l ON l.id = 1 AND m.id <= l.legacy_max_id
  LEFT JOIN private_conversation_clears8 AS c ON c.owner_id = m.recipient_id AND c.peer_id = m.sender_id
  WHERE @local_history_first_migration = 1 AND m.recalled_at IS NULL AND m.id > COALESCE(c.clear_through_id,0)
) AS grants
WHERE NOT EXISTS (SELECT 1 FROM media_grants8 AS g WHERE g.owner_id = grants.owner_id AND g.hash = grants.hash);

COMMIT;
SET @local_history_first_migration = NULL;

-- 核对：两行编号应相同（第一次执行时），会话索引和图片授权数量应大于0（有历史时）。
SELECT 'private_sequence8' AS item, next_id AS value FROM private_sequence8 WHERE id = 1
UNION ALL SELECT 'private_legacy8', legacy_max_id FROM private_legacy8 WHERE id = 1
UNION ALL SELECT 'private_contacts8', COUNT(*) FROM private_contacts8
UNION ALL SELECT 'media_grants8', COUNT(*) FROM media_grants8;
