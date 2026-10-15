-- 在目标账号数据库中手动执行；先备份并核对现有同名表结构。
-- 不修改 user8、FriendsList 或 userlogin8，不自动导入旧好友字符串。
CREATE TABLE IF NOT EXISTS friend_accounts8 (
  accountID VARBINARY(64) NOT NULL,
  PRIMARY KEY (accountID)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friendships8 (
  account_low VARBINARY(64) NOT NULL,
  account_high VARBINARY(64) NOT NULL,
  requested_by VARBINARY(64) NOT NULL,
  state TINYINT UNSIGNED NOT NULL COMMENT '1 pending, 2 accepted',
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (account_low, account_high),
  KEY friendship_high (account_high),
  CONSTRAINT friendship_order CHECK (account_low < account_high),
  CONSTRAINT friendship_requester CHECK (requested_by = account_low OR requested_by = account_high),
  CONSTRAINT friendship_state CHECK (state IN (1, 2))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS private_messages8 (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_low VARBINARY(64) NOT NULL,
  account_high VARBINARY(64) NOT NULL,
  sender_id VARBINARY(64) NOT NULL,
  recipient_id VARBINARY(64) NOT NULL,
  nonce BINARY(16) NOT NULL,
  body VARCHAR(480) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY private_nonce (sender_id, nonce),
  KEY private_conversation (account_low, account_high, id),
  KEY private_sender_recent (sender_id, id),
  KEY private_recipient_recent (recipient_id, id),
  CONSTRAINT private_order CHECK (account_low < account_high),
  CONSTRAINT private_participants CHECK (
    (sender_id = account_low AND recipient_id = account_high) OR
    (sender_id = account_high AND recipient_id = account_low))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
