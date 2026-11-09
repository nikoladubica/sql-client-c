/* SQL tips for the GUI's connecting screen, see sql_tips.h. Each one is
 * true for both MySQL and MariaDB unless it names one of them, ends with a
 * full stop and fits in SQL_TIP_MAX_LEN bytes (tests/sql_tips_test.c). */
#include "sql_tips.h"

static const char *const tips[] = {
    "EXPLAIN before a slow SELECT shows which indexes the server will use and how many rows it expects to read.",
    "SELECT only the columns you need: SELECT * reads and sends every column, even the big TEXT ones.",
    "A composite index on (a, b) also serves queries that filter on a alone, but not on b alone.",
    "Wrapping an indexed column in a function, like WHERE YEAR(created_at) = 2024, stops the server from using that index.",
    "A range can use the index: WHERE created_at >= '2024-01-01' AND created_at < '2025-01-01'.",
    "LIKE 'abc%' can use an index; LIKE '%abc' can't, because the start of the string is unknown.",
    "NULL never equals anything, not even NULL. Use IS NULL, or the NULL-safe operator <=>.",
    "COUNT(*) counts rows; COUNT(col) counts only the rows where col is not NULL.",
    "NOT IN (subquery) matches nothing if the subquery returns a NULL. NOT EXISTS doesn't have that trap.",
    "Run an UPDATE or DELETE as a SELECT with the same WHERE first, to see exactly which rows it will touch.",
    "START TRANSACTION before risky changes: check the result, then COMMIT or ROLLBACK.",
    "TRUNCATE TABLE can't be rolled back and resets AUTO_INCREMENT; DELETE FROM can be, inside a transaction.",
    "CREATE, ALTER and DROP commit the open transaction implicitly, so they can't be rolled back.",
    "LIMIT without ORDER BY returns rows in no guaranteed order; add ORDER BY for stable pages.",
    "A big OFFSET still reads every skipped row. Paging by key (WHERE id > last_id) stays fast.",
    "UNION removes duplicate rows, which costs a sort; UNION ALL keeps them and is faster.",
    "WHERE filters rows before grouping; HAVING filters the groups after GROUP BY.",
    "INSERT ... ON DUPLICATE KEY UPDATE inserts a row, or updates the one with the same unique key.",
    "INSERT IGNORE turns errors such as duplicate keys into warnings; check SHOW WARNINGS afterwards.",
    "One INSERT with many VALUES rows is much faster than many single-row INSERTs.",
    "Use utf8mb4, not utf8: the old utf8 (utf8mb3) can't store emoji or other 4-byte characters.",
    "DECIMAL stores exact values, so use it for money. FLOAT and DOUBLE are approximate.",
    "DATETIME stores the time as given; TIMESTAMP converts it to and from the session time zone.",
    "SHOW CREATE TABLE t prints the full definition of a table, with its indexes and foreign keys.",
    "SHOW PROCESSLIST lists running queries; KILL QUERY id stops one without closing its connection.",
    "SHOW INDEX FROM t lists a table's indexes and their cardinality, an estimate of distinct values.",
    "A covering index holds every column a query needs, so the server can answer from the index alone.",
    "Comparing a string column with a number, like WHERE code = 123, converts every row and skips the index.",
    "COALESCE(a, b, c) returns the first of its arguments that isn't NULL.",
    "IFNULL(x, 0) replaces NULL with 0, handy before adding up or displaying values.",
    "GROUP_CONCAT(name ORDER BY name SEPARATOR ', ') joins a group's values into one string.",
    "Window functions such as ROW_NUMBER() OVER (PARTITION BY ...) rank rows without a self-join.",
    "WITH name AS (SELECT ...) defines a common table expression: a named subquery that keeps long queries readable.",
    "WITH RECURSIVE walks trees and hierarchies, such as categories with a parent_id.",
    "A LEFT JOIN keeps every row of the left table; the right table's columns are NULL where nothing matches.",
    "A condition on the right table in WHERE turns a LEFT JOIN into an inner join. Put it in the ON clause instead.",
    "Foreign keys need an index on the referencing column; InnoDB creates one if it is missing.",
    "ANALYZE TABLE refreshes a table's index statistics, which helps the optimizer pick better plans.",
    "EXPLAIN ANALYZE (MySQL 8.0.18+) or ANALYZE SELECT (MariaDB) runs the query and shows the real row counts.",
    "The slow query log records every query that takes longer than long_query_time seconds.",
    "Backticks quote names: `order` is a table called order, while 'order' is just a string.",
    "Double quotes make strings, not names, unless sql_mode includes ANSI_QUOTES.",
    "ENUM columns sort by their position in the list of values, not alphabetically.",
    "SELECT ... FOR UPDATE locks the rows it reads until the transaction ends.",
    "Keep transactions short: long ones hold locks and make other sessions wait.",
    "A deadlock error means InnoDB rolled back one of the transactions; it is safe to retry it.",
    "DISTINCT applies to the whole selected row, not just to the first column.",
    "BETWEEN includes both ends: BETWEEN 1 AND 10 matches 1 and 10.",
    "CHAR_LENGTH counts characters and LENGTH counts bytes, so they differ for non-ASCII text.",
    "ORDER BY RAND() sorts the whole table, so it gets slow on large tables.",
};

size_t sql_tip_count(void) {
    return sizeof tips / sizeof tips[0];
}

const char *sql_tip(size_t i) {
    return i < sql_tip_count() ? tips[i] : NULL;
}

size_t sql_tip_pick(size_t last, unsigned long r) {
    size_t n = sql_tip_count();
    if (n <= 1) return 0;
    if (last >= n) return (size_t)(r % n);
    size_t i = (size_t)(r % (n - 1)); /* one of the other n - 1 ... */
    return i >= last ? i + 1 : i;     /* ... skipping last */
}
