-- Run manually in the selected account database after friends_schema.sql.
-- Safe to rerun on MySQL 5.7/8; the server does not execute this migration.
-- Existing accounts, relationships and messages are unchanged.
SET @temporary_chat_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.statistics
    WHERE table_schema=DATABASE() AND table_name='private_messages8' AND index_name='private_sender_recent'),
  'SELECT 1',
  'ALTER TABLE private_messages8 ADD INDEX private_sender_recent (sender_id,id)');
PREPARE temporary_chat_statement FROM @temporary_chat_ddl;
EXECUTE temporary_chat_statement;
DEALLOCATE PREPARE temporary_chat_statement;

SET @temporary_chat_ddl = IF(
  EXISTS (SELECT 1 FROM information_schema.statistics
    WHERE table_schema=DATABASE() AND table_name='private_messages8' AND index_name='private_recipient_recent'),
  'SELECT 1',
  'ALTER TABLE private_messages8 ADD INDEX private_recipient_recent (recipient_id,id)');
PREPARE temporary_chat_statement FROM @temporary_chat_ddl;
EXECUTE temporary_chat_statement;
DEALLOCATE PREPARE temporary_chat_statement;
SET @temporary_chat_ddl = NULL;
