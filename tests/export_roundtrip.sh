#!/bin/sh
# Export round trip against a real server (make export-test):
#   - SQL export re-imported into an empty database reproduces the table
#   - XML is well-formed and marks NULL vs empty string
#   - CSV parses back to the same values
# Creates and drops the databases test_sqlc_export_src and test_sqlc_export_dst.
set -eu

mysql() { command mysql --default-character-set=utf8mb4 "$@"; }

BIN=$1
OUT=$2
SRC=test_sqlc_export_src
DST=test_sqlc_export_dst
TABLE='odd `name` t'

rm -rf "$OUT"
mkdir -p "$OUT"
mysql -e "DROP DATABASE IF EXISTS $SRC; DROP DATABASE IF EXISTS $DST;
          CREATE DATABASE $SRC CHARACTER SET utf8mb4; CREATE DATABASE $DST CHARACTER SET utf8mb4;"
trap 'mysql -e "DROP DATABASE IF EXISTS $SRC; DROP DATABASE IF EXISTS $DST;"' EXIT

mysql "$SRC" <<'SQL'
SET SESSION sql_mode = CONCAT(@@sql_mode, ',NO_AUTO_VALUE_ON_ZERO');
CREATE TABLE `odd ``name`` t` (
  id INT PRIMARY KEY AUTO_INCREMENT,
  txt VARCHAR(100) NULL,
  big TEXT,
  bin VARBINARY(16),
  blb BLOB,
  flag BIT(3),
  amount DECIMAL(10,2),
  dbl DOUBLE,
  ts TIMESTAMP NULL DEFAULT NULL,
  dt DATETIME,
  gen INT AS (id * 2) VIRTUAL,
  `we``ird, col` VARCHAR(10)
) DEFAULT CHARSET=utf8mb4;
INSERT INTO `odd ``name`` t` (id, txt, big, bin, blb, flag, amount, dbl, ts, dt, `we``ird, col`) VALUES
  (0, '', 'zero id', '', '', b'000', 0, 0, NULL, NULL, ''),
  (1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
  (2, 'it''s "quoted", with commas', 'line1\nline2\r\nline3\ttab', 0x00FF10, 0x0000, b'101', -12.5, 1.5e-7,
      '2024-03-01 12:34:56', '1999-12-31 23:59:59', 'x'),
  (3, 'back\\slash and \Z ctrl-z', CONCAT('nul', CHAR(0), 'byte'), 0xDEADBEEF, REPEAT(0xAB, 300), b'111',
      99999999.99, -0, '2030-01-01 00:00:00', '2000-01-01', 'y'),
  (4, 'café 😀 <tag> & ''apos''', REPEAT('long ', 2000), 0x41, NULL, b'010', 1, 3.14159,
      NULL, NULL, 'é');
SQL
# bigger table for batching
mysql "$SRC" -e "INSERT INTO \`odd \`\`name\`\` t\` (id, txt) SELECT seq + 100, CONCAT('row ', seq) FROM seq_1_to_3000"

"$BIN" "$SRC" "$TABLE" "$OUT"

echo "--- SQL re-import"
mysql "$DST" < "$OUT/out.sql"
CK_SRC=$(mysql -N "$SRC" -e "CHECKSUM TABLE \`odd \`\`name\`\` t\`" | cut -f2)
CK_DST=$(mysql -N "$DST" -e "CHECKSUM TABLE \`odd \`\`name\`\` t\`" | cut -f2)
DEF_SRC=$(mysql -N "$SRC" -e "SHOW CREATE TABLE \`odd \`\`name\`\` t\`" | cut -f2)
DEF_DST=$(mysql -N "$DST" -e "SHOW CREATE TABLE \`odd \`\`name\`\` t\`" | cut -f2)
echo "checksum src=$CK_SRC dst=$CK_DST"
[ "$CK_SRC" = "$CK_DST" ] || { echo "FAIL: data differs after re-import"; exit 1; }
[ "$DEF_SRC" = "$DEF_DST" ] || { echo "FAIL: table definition differs after re-import"; exit 1; }
# the import is repeatable (DROP TABLE IF EXISTS)
mysql "$DST" < "$OUT/out.sql"
echo "re-import OK (twice)"

echo "--- XML"
xmllint --noout "$OUT/out.xml"
ROWS=$(grep -c '<row>' "$OUT/out.xml")
[ "$ROWS" = 3005 ] || { echo "FAIL: XML has $ROWS rows"; exit 1; }
grep -q '<field name="txt"></field>' "$OUT/out.xml" || { echo "FAIL: empty string"; exit 1; }
grep -q '<field name="txt" xsi:nil="true"/>' "$OUT/out.xml" || { echo "FAIL: NULL"; exit 1; }
echo "XML well-formed, NULL and empty string distinct"

echo "--- CSV"
python3 -I - "$OUT/out.csv" "$OUT/out-nullN.csv" <<'PY'
import csv, sys
def load(path):
    with open(path, newline='', encoding='utf-8-sig') as f:
        return list(csv.reader(f))
rows = load(sys.argv[1])
head, data = rows[0], rows[1:]
assert head == ['id', 'txt', 'big', 'bin', 'blb', 'flag', 'amount', 'dbl', 'ts', 'dt', 'gen', 'we`ird, col'], head
assert len(data) == 3005, len(data)
by_id = {r[0]: r for r in data}
r2 = by_id['2']
assert r2[1] == 'it\'s "quoted", with commas', r2[1]
assert r2[2] == 'line1\nline2\r\nline3\ttab', repr(r2[2])
assert r2[3] == '0x00FF10', r2[3]
assert by_id['4'][1] == 'café \U0001F600 <tag> & \'apos\'', by_id['4'][1]
assert by_id['3'][2] == 'nul\x00byte', repr(by_id['3'][2])
assert all(len(r) == len(head) for r in data)
raw = open(sys.argv[1], 'rb').read()
assert raw.startswith(b'\xef\xbb\xbf') and b'\r\n' in raw
n = load(sys.argv[2])
assert n[1 + [r[0] for r in n[1:]].index('1')][1] == '\\N'
print('CSV parses back: quotes, commas, newlines, UTF-8, NULL as \\N')
PY

echo "export round trip: all checks passed"
