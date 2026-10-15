"""Exercise the real migration scripts on a disposable local MySQL server.

Requires an explicitly named test user with an empty password and a non-default
TCP port. Every scenario creates a fresh chh8_test_media_* schema and preserves
it for inspection. No server/database configuration or production credentials are read.
Collection checks execute the reference-check SQL extracted from collect_media;
they do not call the C++ collector or its scheduling/asset-deletion code.
"""

import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PREFIX = "chh8_test_media_"
HASH_OLD = "11" * 32
HASH_CLEARED = "22" * 32
HASH_RECALLED = "33" * 32
HASH_NEW = "44" * 32


def source(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


class Runner:
    def __init__(self, args):
        self.command = [
            str(args.mysql), "--no-defaults", "--protocol=TCP",
            "--host=127.0.0.1", f"--port={args.port}", f"--user={args.user}",
            "--password=", "--batch", "--raw", "--skip-column-names",
            "--default-character-set=utf8mb4", "--connect-timeout=5",
        ]
        self.environment = {
            key: value for key, value in os.environ.items()
            if not key.upper().startswith("MYSQL")
        }
        self.created = set()
        self.passed = 0

    def sql(self, sql, schema=None, expect_failure=False):
        if schema is not None and schema not in self.created:
            raise RuntimeError("refusing a schema not created by this test run")
        command = self.command + ([f"--database={schema}"] if schema else [])
        result = subprocess.run(
            command, input=sql.encode("utf-8"), capture_output=True,
            env=self.environment, timeout=60, check=False,
        )
        if expect_failure:
            if result.returncode == 0:
                raise AssertionError("injected SQL failure was not reported")
        elif result.returncode != 0:
            raise RuntimeError(result.stderr.decode("utf-8", errors="replace").strip())
        return result.stdout.decode("utf-8", errors="strict").replace("\r\n", "\n").strip()

    def check(self, label, actual, expected):
        if actual != expected:
            raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")
        self.passed += 1
        print(f"PASS {label}", flush=True)

    @contextlib.contextmanager
    def schema(self):
        name = PREFIX + secrets.token_hex(8)
        if not re.fullmatch(r"chh8_test_media_[0-9a-f]{16}", name):
            raise RuntimeError("invalid generated test schema")
        # Deliberately omit IF NOT EXISTS: an existing database is never reused.
        self.sql(f"CREATE DATABASE `{name}` CHARACTER SET utf8mb4;")
        self.created.add(name)
        try:
            yield name
        finally:
            print(f"KEEP {name}", flush=True)

    def apply(self, schema, name):
        self.sql(source(name), schema)

    def base(self, schema):
        for name in ("friends_schema.sql", "recall_schema.sql", "media_schema.sql",
                     "temporary_chat_clear.sql"):
            self.apply(schema, name)


def insert_image(runner, schema, message_id, peer, image_hash, recalled=False):
    # All interpolated values are fixed synthetic fixtures defined in this file.
    runner.sql(f"""
        INSERT INTO media_assets8(hash,data,created_at)
        VALUES(UNHEX('{image_hash}'),X'01',NOW()-INTERVAL 2 HOUR);
        INSERT INTO media_uploads8(accountID,hash,created_at)
        VALUES('alice',UNHEX('{image_hash}'),NOW()-INTERVAL 2 HOUR);
        INSERT INTO private_messages8
          (id,account_low,account_high,sender_id,recipient_id,nonce,body,recalled_at)
        VALUES({message_id},'alice','{peer}','alice','{peer}',
          UNHEX('{message_id:032x}'),'synthetic image',{'NOW()' if recalled else 'NULL'});
        INSERT INTO private_message_media8(message_id,hash)
        VALUES({message_id},UNHEX('{image_hash}'));
    """, schema)


def seed_legacy(runner, schema):
    insert_image(runner, schema, 10, "bob", HASH_OLD)
    insert_image(runner, schema, 11, "carol", HASH_CLEARED)
    insert_image(runner, schema, 12, "dave", HASH_RECALLED, recalled=True)
    runner.sql("INSERT INTO private_conversation_clears8 VALUES('alice','carol',11);", schema)


def grants(runner, schema):
    return runner.sql(
        "SELECT CONCAT(CONVERT(owner_id USING utf8mb4),':',LOWER(HEX(hash)),':',"
        "CONVERT(peer_id USING utf8mb4),':',refs) FROM media_grants8 "
        "ORDER BY owner_id,hash,peer_id;", schema,
    )


def expected_legacy():
    return "\n".join((f"alice:{HASH_OLD}::1", f"bob:{HASH_OLD}::1", f"carol:{HASH_CLEARED}::1"))


def reference_state(runner, schema, image_hash):
    # Use production SQL, so a future change to the collector's reference rule
    # cannot silently leave this regression test testing an obsolete copy.
    match = re.search(
        r'media_rows\(connection,("SELECT \(EXISTS[\s\S]*?),\{hash,hash,hash,grace,hash\}',
        source("media_store.h"),
    )
    if not match:
        raise AssertionError("cannot locate collect_media reference-check SQL")
    fragments = re.findall(r'"(?:\\.|[^"\\])*"', match.group(1))
    query = "".join(json.loads(fragment) for fragment in fragments)
    for value in (f"'{image_hash}'", f"'{image_hash}'", f"'{image_hash}'", "3600", f"'{image_hash}'"):
        query = query.replace("?", value, 1)
    if "?" in query:
        raise AssertionError("unbound collector SQL parameter")
    fields = runner.sql(query + ";", schema).split("\t")
    if len(fields) != 2:
        raise AssertionError("unexpected collector reference result")
    return fields[0], int(fields[1])


def migration_order_case(runner, private_first):
    label = "private-first" if private_first else "local-first"
    with runner.schema() as schema:
        runner.base(schema)
        seed_legacy(runner, schema)
        order = ("private_media_migration.sql", "local_history_migration.sql") if private_first else (
            "local_history_migration.sql", "private_media_migration.sql")
        for name in order:
            runner.apply(schema, name)
        runner.check(f"{label}: initial backfill respects clear and recall", grants(runner, schema), expected_legacy())
        runner.check(f"{label}: frozen legacy boundary", runner.sql("SELECT legacy_max_id FROM private_legacy8 WHERE id=1;", schema), "12")
        before = grants(runner, schema)
        for name in order:
            runner.apply(schema, name)
        runner.check(f"{label}: immediate rerun unchanged", grants(runner, schema), before)

        # An acknowledged old image can still have its legacy message/link while
        # the other participant has not completed the history export.
        runner.sql(f"DELETE FROM media_grants8 WHERE owner_id='alice' AND hash=UNHEX('{HASH_OLD}');", schema)
        insert_image(runner, schema, 100, "bob", HASH_NEW)
        runner.sql(f"INSERT INTO media_grants8(owner_id,hash,peer_id) VALUES('bob',UNHEX('{HASH_NEW}'),'alice');", schema)
        before = grants(runner, schema)
        runner.apply(schema, "local_history_migration.sql")
        runner.apply(schema, "private_media_migration.sql")
        runner.check(f"{label}: old acknowledgements and new rights preserved", grants(runner, schema), before)
        runner.check(f"{label}: boundary stays frozen after new send", runner.sql("SELECT legacy_max_id FROM private_legacy8 WHERE id=1;", schema), "12")
        runner.check(f"{label}: sequence advances independently", runner.sql("SELECT next_id FROM private_sequence8 WHERE id=1;", schema), "100")
        runner.check(f"{label}: pending new image is retained", reference_state(runner, schema, HASH_NEW)[0], "1")

        # Model successful message and image acknowledgements, including deleting
        # the message link. The actual collector query must now permit collection.
        runner.sql(f"""
            DELETE FROM media_grants8 WHERE owner_id='bob' AND hash=UNHEX('{HASH_NEW}');
            DELETE FROM private_message_media8 WHERE message_id=100;
            DELETE FROM private_messages8 WHERE id=100;
        """, schema)
        runner.apply(schema, "local_history_migration.sql")
        retained, grace = reference_state(runner, schema, HASH_NEW)
        runner.check(f"{label}: acknowledged new image has no retaining reference", retained, "0")
        runner.check(f"{label}: acknowledged new image is outside upload grace", grace <= 0, True)


def rollback_case(runner, fail_after_grants):
    label = "failed-statement rollback" if fail_after_grants else "explicit rollback"
    with runner.schema() as schema:
        runner.base(schema)
        seed_legacy(runner, schema)
        runner.apply(schema, "private_media_migration.sql")
        migration = source("local_history_migration.sql")
        if fail_after_grants:
            if migration.count("\nCOMMIT;") != 1:
                raise AssertionError("cannot identify migration commit")
            injected = migration.replace("\nCOMMIT;", "\nSELECT * FROM chh8_intentionally_missing;\nCOMMIT;", 1)
            runner.sql(injected, schema, expect_failure=True)
        else:
            cut = migration.index("INSERT INTO media_grants8 (owner_id,hash)")
            runner.sql(migration[:cut] + "\nROLLBACK;", schema)
        for table in ("private_legacy8", "private_sequence8", "private_contacts8", "media_grants8"):
            runner.check(f"{label}: no partial {table}", runner.sql(f"SELECT COUNT(*) FROM {table};", schema), "0")
        runner.apply(schema, "local_history_migration.sql")
        runner.check(f"{label}: retry completes first backfill", grants(runner, schema), expected_legacy())
        runner.check(f"{label}: first-run session flag cleared", runner.sql(
            source("local_history_migration.sql") + "\nSELECT @local_history_first_migration IS NULL;", schema).splitlines()[-1], "1")


def existing_boundary_case(runner):
    with runner.schema() as schema:
        runner.base(schema)
        seed_legacy(runner, schema)
        runner.apply(schema, "private_media_migration.sql")
        # Recreate an installation produced by the former script: the original
        # two-column boundary table is its only durable migration marker.
        ddl = source("local_history_migration.sql").split("START TRANSACTION;", 1)[0]
        runner.sql(ddl, schema)
        runner.sql(f"""
            INSERT INTO private_legacy8(id,legacy_max_id) VALUES(1,12);
            INSERT INTO private_sequence8(id,next_id) VALUES(1,12);
            INSERT INTO media_grants8(owner_id,hash)
            VALUES('bob',UNHEX('{HASH_OLD}')),('carol',UNHEX('{HASH_CLEARED}'));
        """, schema)
        insert_image(runner, schema, 100, "bob", HASH_NEW)
        runner.sql(f"INSERT INTO media_grants8(owner_id,hash,peer_id) VALUES('bob',UNHEX('{HASH_NEW}'),'alice');", schema)
        before = grants(runner, schema)
        runner.apply(schema, "local_history_migration.sql")
        runner.check("old deployment: missing acknowledged grants stay absent", grants(runner, schema), before)
        runner.check("old deployment: no additional marker column required", runner.sql(
            "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='private_legacy8';", schema), "2")


def empty_first_migration_case(runner):
    with runner.schema() as schema:
        runner.base(schema)
        runner.apply(schema, "local_history_migration.sql")
        runner.apply(schema, "private_media_migration.sql")
        insert_image(runner, schema, 1, "bob", HASH_NEW)
        runner.sql(f"INSERT INTO media_grants8(owner_id,hash,peer_id) VALUES('bob',UNHEX('{HASH_NEW}'),'alice');", schema)
        runner.apply(schema, "local_history_migration.sql")
        runner.check("empty first migration: only the new recipient grant remains", grants(runner, schema), f"bob:{HASH_NEW}:alice:1")
        runner.check("empty first migration: zero boundary is still a completed migration", runner.sql("SELECT legacy_max_id FROM private_legacy8 WHERE id=1;", schema), "0")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysql", required=True, type=Path, help="absolute path to the MySQL CLI")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--user", required=True, help="explicit local test user with an empty password")
    args = parser.parse_args()
    if args.host not in ("127.0.0.1", "localhost") or not 1 <= args.port <= 65535 or args.port == 3306:
        parser.error("only loopback and an explicit non-3306 test port are allowed")
    if not args.mysql.is_absolute() or not args.mysql.is_file():
        parser.error("--mysql must name an existing absolute executable path")
    if not re.fullmatch(r"[A-Za-z0-9_]+", args.user) or args.user.lower() == "root":
        parser.error("an explicit non-root local test user is required")
    runner = Runner(args)
    try:
        migration_order_case(runner, private_first=False)
        migration_order_case(runner, private_first=True)
        rollback_case(runner, fail_after_grants=False)
        rollback_case(runner, fail_after_grants=True)
        existing_boundary_case(runner)
        empty_first_migration_case(runner)
    except (AssertionError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL {error}", file=sys.stderr)
        print(f"RESULT {runner.passed} passed, 1 failed")
        return 1
    print(f"RESULT {runner.passed} passed, 0 failed")
    print("Scope: real migration scripts and production collector reference SQL; no C++ collector invocation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
