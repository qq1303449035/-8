-- 私聊图片存储转发迁移：私聊图片不再永久保存在服务器，接收方下载并确认后由服务器自动删除；大厅图片只保存在服务器内存。
-- 在账号数据库中手动执行；服务器不会自动执行。需要 MySQL 8.0.16 或更高版本。步骤见 PRIVATE_MEDIA_MIGRATION.md。
-- 前提：已执行 media_schema.sql。可在 local_history_migration.sql 之前或之后执行，两者都可以重复执行。
-- 可以重复执行：已完成的步骤不再修改，已有授权行和图片都保留。
-- 已有的收藏图片由收藏继续保留；只有大厅使用过的图片和旧私聊图片不做保留标记，在清理开关打开后按新规则清理。

-- 1. 待下载授权表：接收方（owner_id）对图片（hash）的待下载授权，peer_id 为发送方（迁移前的旧行为空字符串），id 为推送游标，
--    refs 为该发送方仍需要这条授权的消息数（送达前撤回其中一条时减一，减到0才删除授权）。
CREATE TABLE IF NOT EXISTS media_grants8 (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  owner_id VARBINARY(64) NOT NULL,
  hash BINARY(32) NOT NULL,
  peer_id VARBINARY(64) NOT NULL DEFAULT '',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  refs INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (owner_id,hash,peer_id),
  UNIQUE KEY media_grant_id (id),
  KEY media_grant_owner (owner_id,id),
  KEY media_grant_peer (owner_id,peer_id),
  KEY media_grant_hash (hash)
) ENGINE=InnoDB;

-- 2. 升级 local_history_migration.sql 创建的旧结构（owner_id,hash,created_at）：补充发送方、推送游标和消息数，主键改为 (owner_id,hash,peer_id)。
SET @private_media_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND COLUMN_NAME='peer_id'),
  'DO 0',
  'ALTER TABLE media_grants8 ADD COLUMN peer_id VARBINARY(64) NOT NULL DEFAULT '''' AFTER hash');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;

SET @private_media_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND COLUMN_NAME='id'),
  'DO 0',
  'ALTER TABLE media_grants8 ADD COLUMN id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT FIRST, ADD UNIQUE KEY media_grant_id (id)');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;

SET @private_media_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND COLUMN_NAME='refs'),
  'DO 0',
  'ALTER TABLE media_grants8 ADD COLUMN refs INT UNSIGNED NOT NULL DEFAULT 1');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;

SET @private_media_ddl = IF(
  (SELECT GROUP_CONCAT(COLUMN_NAME ORDER BY ORDINAL_POSITION) FROM information_schema.KEY_COLUMN_USAGE
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND CONSTRAINT_NAME='PRIMARY') <=> 'owner_id,hash,peer_id',
  'DO 0',
  'ALTER TABLE media_grants8 DROP PRIMARY KEY, ADD PRIMARY KEY (owner_id,hash,peer_id)');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;

SET @private_media_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND INDEX_NAME='media_grant_owner'),
  'DO 0',
  'ALTER TABLE media_grants8 ADD INDEX media_grant_owner (owner_id,id)');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;

SET @private_media_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND INDEX_NAME='media_grant_peer'),
  'DO 0',
  'ALTER TABLE media_grants8 ADD INDEX media_grant_peer (owner_id,peer_id)');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;

SET @private_media_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND INDEX_NAME='media_grant_hash'),
  'DO 0',
  'ALTER TABLE media_grants8 ADD INDEX media_grant_hash (hash)');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;

-- 3. 清理时按图片检查收藏，需要按 hash 的索引。
SET @private_media_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_favorites8' AND INDEX_NAME='media_favorite_hash'),
  'DO 0',
  'ALTER TABLE media_favorites8 ADD INDEX media_favorite_hash (hash)');
PREPARE private_media_statement FROM @private_media_ddl;
EXECUTE private_media_statement;
DEALLOCATE PREPARE private_media_statement;
SET @private_media_ddl = NULL;

-- 4. 清理开关：enabled_after 之前服务器不删除任何图片（授权仍在确认后删除）。第一次执行时设为一天以后，
--    这段安全期内仍可回退到旧服务器；之后重复执行不改变已有的时间。管理员可以随时修改（见 PRIVATE_MEDIA_MIGRATION.md）。
CREATE TABLE IF NOT EXISTS media_collection8 (
  id TINYINT NOT NULL,
  enabled_after DATETIME NOT NULL,
  PRIMARY KEY (id),
  CONSTRAINT media_collection_single CHECK (id = 1)
) ENGINE=InnoDB;
INSERT INTO media_collection8 (id,enabled_after)
SELECT 1, NOW() + INTERVAL 1 DAY FROM DUAL WHERE NOT EXISTS (SELECT 1 FROM media_collection8 WHERE id = 1);

-- 核对：第一行应为 columns=id,owner_id,hash,peer_id,created_at,refs，primary_key=owner_id,hash,peer_id，
-- 索引包含 media_grant_hash、media_grant_id、media_grant_owner、media_grant_peer；
-- 第二行为授权数、迁移前的旧授权数（peer_id 为空）、图片数、清理开关时间及是否已开始清理。
SELECT 'media_grants8' AS table_name,
  (SELECT GROUP_CONCAT(COLUMN_NAME ORDER BY ORDINAL_POSITION) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8') AS columns,
  (SELECT GROUP_CONCAT(COLUMN_NAME ORDER BY ORDINAL_POSITION) FROM information_schema.KEY_COLUMN_USAGE
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8' AND CONSTRAINT_NAME='PRIMARY') AS primary_key,
  (SELECT GROUP_CONCAT(DISTINCT INDEX_NAME ORDER BY INDEX_NAME) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='media_grants8') AS indexes;
SELECT (SELECT COUNT(*) FROM media_grants8) AS rights,
  (SELECT COUNT(*) FROM media_grants8 WHERE peer_id = '') AS legacy_rights,
  (SELECT COUNT(*) FROM media_assets8) AS assets,
  c.enabled_after, c.enabled_after <= NOW() AS collecting
FROM media_collection8 AS c WHERE c.id = 1;
