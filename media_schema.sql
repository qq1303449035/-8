-- Execute in the account database after friends_schema.sql. The server never runs this file.
-- Existing user8, private_messages8 and avatar tables are not altered.
-- Images keep their original size: at most 15 MiB (15728640 bytes) per canonical PNG/GIF.
-- An existing media_assets8 with the older 2 MiB CHECK is upgraded by media_original_migration.sql.
CREATE TABLE IF NOT EXISTS media_assets8 (
  hash BINARY(32) NOT NULL,
  data MEDIUMBLOB NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (hash),
  CONSTRAINT media_asset_size CHECK (OCTET_LENGTH(data) BETWEEN 1 AND 15728640)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS media_uploads8 (
  accountID VARBINARY(64) NOT NULL,
  hash BINARY(32) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (accountID,hash),
  KEY media_upload_hash (hash)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS private_message_media8 (
  message_id BIGINT UNSIGNED NOT NULL,
  hash BINARY(32) NOT NULL,
  PRIMARY KEY (message_id),
  KEY private_media_hash (hash,message_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS media_favorites8 (
  accountID VARBINARY(64) NOT NULL,
  hash BINARY(32) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (accountID,hash)
) ENGINE=InnoDB;
