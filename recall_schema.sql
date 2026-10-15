-- Execute once in the account database after friends_schema.sql.
-- The server never runs this migration automatically.
-- recalled_at is a soft-delete marker: message ordering and audit data stay
-- intact while clients render the standard recall placeholder.
SET @chh8_recalled_at_exists := (
  SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'private_messages8'
    AND COLUMN_NAME = 'recalled_at'
);
SET @chh8_recall_sql := IF(
  @chh8_recalled_at_exists = 0,
  'ALTER TABLE private_messages8 ADD COLUMN recalled_at TIMESTAMP NULL DEFAULT NULL AFTER body',
  'SELECT 1'
);
PREPARE chh8_recall_stmt FROM @chh8_recall_sql;
EXECUTE chh8_recall_stmt;
DEALLOCATE PREPARE chh8_recall_stmt;
