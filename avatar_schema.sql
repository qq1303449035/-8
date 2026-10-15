-- Run in the same account database selected by CHH_DB_NAME. No user8 changes.
CREATE TABLE IF NOT EXISTS user_avatars8 (
  accountID VARBINARY(64) NOT NULL,
  hash BINARY(32) NOT NULL,
  png MEDIUMBLOB NOT NULL,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (accountID),
  KEY avatar_hash (hash),
  CONSTRAINT avatar_png_size CHECK (OCTET_LENGTH(png) BETWEEN 1 AND 393216)
) ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4;
