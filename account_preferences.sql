-- Select the account database, back it up, and run manually after friends_schema.sql.
-- Re-running is safe. Existing tables with a different definition must be reviewed
-- separately: CREATE TABLE IF NOT EXISTS does not repair them. No account data is changed.
CREATE TABLE IF NOT EXISTS account_preferences8 (
  owner_id VARBINARY(64) NOT NULL,
  hide_id TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (owner_id),
  CONSTRAINT account_preferences_hide CHECK (hide_id IN (0,1))
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friend_notes8 (
  owner_id VARBINARY(64) NOT NULL,
  peer_id VARBINARY(64) NOT NULL,
  note VARCHAR(144) NOT NULL,
  PRIMARY KEY (owner_id,peer_id),
  CONSTRAINT friend_notes_other CHECK (owner_id <> peer_id),
  CONSTRAINT friend_notes_length CHECK (CHAR_LENGTH(note) BETWEEN 1 AND 36 AND OCTET_LENGTH(note) <= 144)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
