-- 聊天原图迁移：把 media_assets8 的图片大小检查约束 media_asset_size 从2 MiB（2097152字节）放宽到15 MiB（15728640字节）。
-- 在账号数据库中手动执行；服务器不会自动执行。需要 MySQL 8.0.16 或更高版本。步骤见 MEDIA_MIGRATION.md。
-- 前提：已执行 media_schema.sql。可以重复执行：约束已经是15 MiB时不做任何修改（不重建表）。
-- 替换约束会以 ALGORITHM=COPY 重建 media_assets8：执行期间无法写入新图片，并需要约等于该表大小的空闲磁盘空间，请在维护时段执行。
-- data 列为 MEDIUMBLOB（最大16 MiB），无需修改。MySQL 的 max_allowed_packet 至少需要16M（8.0默认64M）。

SET @media_original_ddl = (
  SELECT CASE
    WHEN COUNT(*) = 0 THEN
      'ALTER TABLE media_assets8 ADD CONSTRAINT media_asset_size CHECK (OCTET_LENGTH(data) BETWEEN 1 AND 15728640)'
    WHEN SUM(t.ENFORCED = 'YES' AND LOWER(c.CHECK_CLAUSE) LIKE '%between 1 and 15728640)') = COUNT(*) THEN
      'DO 0'
    ELSE
      'ALTER TABLE media_assets8 DROP CHECK media_asset_size, ADD CONSTRAINT media_asset_size CHECK (OCTET_LENGTH(data) BETWEEN 1 AND 15728640)'
  END
  FROM information_schema.TABLE_CONSTRAINTS AS t
  JOIN information_schema.CHECK_CONSTRAINTS AS c
    ON c.CONSTRAINT_SCHEMA = t.CONSTRAINT_SCHEMA AND c.CONSTRAINT_NAME = t.CONSTRAINT_NAME
  WHERE t.TABLE_SCHEMA = DATABASE() AND t.TABLE_NAME = 'media_assets8'
    AND t.CONSTRAINT_TYPE = 'CHECK' AND t.CONSTRAINT_NAME = 'media_asset_size');
PREPARE media_original_statement FROM @media_original_ddl;
EXECUTE media_original_statement;
DEALLOCATE PREPARE media_original_statement;
SET @media_original_ddl = NULL;

-- 核对：应返回一行，upgraded 和 packet_ok 都为1。packet_ok 为0时请把 max_allowed_packet 调到至少16M（建议64M）后重启MySQL或设置GLOBAL。
SELECT t.TABLE_NAME AS table_name, t.CONSTRAINT_NAME AS constraint_name, c.CHECK_CLAUSE AS check_clause, t.ENFORCED AS enforced,
  (t.ENFORCED = 'YES' AND LOWER(c.CHECK_CLAUSE) LIKE '%between 1 and 15728640)') AS upgraded,
  @@GLOBAL.max_allowed_packet AS max_allowed_packet,
  (@@GLOBAL.max_allowed_packet >= 16777216) AS packet_ok
FROM information_schema.TABLE_CONSTRAINTS AS t
JOIN information_schema.CHECK_CONSTRAINTS AS c
  ON c.CONSTRAINT_SCHEMA = t.CONSTRAINT_SCHEMA AND c.CONSTRAINT_NAME = t.CONSTRAINT_NAME
WHERE t.TABLE_SCHEMA = DATABASE() AND t.TABLE_NAME = 'media_assets8'
  AND t.CONSTRAINT_TYPE = 'CHECK' AND t.CONSTRAINT_NAME = 'media_asset_size';
