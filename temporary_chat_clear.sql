-- Select the account database and run manually after friends_schema.sql.
-- Re-running preserves every account, message, attachment, and existing watermark.
-- Check pre-existing table definitions: IF NOT EXISTS does not repair old schemas.
CREATE TABLE IF NOT EXISTS private_conversation_clears8 (
  owner_id VARBINARY(64) NOT NULL,
  peer_id VARBINARY(64) NOT NULL,
  clear_through_id BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (owner_id,peer_id),
  CONSTRAINT private_clear_other CHECK (owner_id <> peer_id)
) ENGINE=InnoDB;
