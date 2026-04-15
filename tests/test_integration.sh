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

cat > "$TMP_DIR/db/users.schema" <<'EOF'
id
name
age
city
EOF

cat > "$TMP_DIR/batch.sql" <<'EOF'
INSERT INTO users VALUES (2, 'Bob', 25, 'Seoul');
SELECT * FROM users;
SELECT name, city FROM users WHERE id = 2;
EOF

"$BIN_PATH" -d "$TMP_DIR/db" -f "$TMP_DIR/batch.sql" > "$TMP_DIR/batch.out"

cat > "$TMP_DIR/duplicate.sql" <<'EOF'
INSERT INTO users VALUES (2, 'Bobby', 30, 'Busan');
EOF

if "$BIN_PATH" -d "$TMP_DIR/db" -f "$TMP_DIR/duplicate.sql" > "$TMP_DIR/duplicate.out" 2> "$TMP_DIR/duplicate.err"; then
    echo "duplicate id insert should have failed" >&2
    exit 1
fi

cat > "$TMP_DIR/expected.data" <<'EOF'
1|Alice|20
2|Bob|25|Seoul
EOF

cmp "$TMP_DIR/db/users.data" "$TMP_DIR/expected.data"
grep -q "INSERT 1" "$TMP_DIR/insert1.out"
grep -q "INSERT 1" "$TMP_DIR/batch.out"
grep -q "Alice" "$TMP_DIR/batch.out"
grep -q "Bob" "$TMP_DIR/batch.out"
grep -q "Seoul" "$TMP_DIR/batch.out"
grep -q "2 rows selected" "$TMP_DIR/batch.out"
grep -q "1 rows selected" "$TMP_DIR/batch.out"
grep -q "duplicate id '2'" "$TMP_DIR/duplicate.err"

echo "integration: OK"
