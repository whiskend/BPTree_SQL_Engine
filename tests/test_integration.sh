#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN_PATH="${SQL_PROCESSOR_BIN:-$ROOT_DIR/sql_processor}"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sql-processor.XXXXXX")

cleanup() {
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

mkdir -p "$TMP_DIR/db"

cat > "$TMP_DIR/db/users.schema" <<'EOF'
id
name
age
EOF

"$BIN_PATH" -d "$TMP_DIR/db" -f "$ROOT_DIR/queries/insert_users.sql" > "$TMP_DIR/insert1.out"

cat > "$TMP_DIR/batch.sql" <<'EOF'
INSERT INTO users VALUES ('Bob', 25);
SELECT * FROM users;
SELECT id, name FROM users WHERE id = 2;
EOF

"$BIN_PATH" -d "$TMP_DIR/db" -f "$TMP_DIR/batch.sql" > "$TMP_DIR/batch.out"

cat > "$TMP_DIR/expected.data" <<'EOF'
1|Alice|20
2|Bob|25
EOF

cmp "$TMP_DIR/db/users.data" "$TMP_DIR/expected.data"
grep -q "INSERT 1" "$TMP_DIR/insert1.out"
grep -q "INSERT 1" "$TMP_DIR/batch.out"
grep -q "Alice" "$TMP_DIR/batch.out"
grep -q "Bob" "$TMP_DIR/batch.out"
grep -q "2 rows selected" "$TMP_DIR/batch.out"
grep -q "1 rows selected" "$TMP_DIR/batch.out"

echo "integration: OK"
