#include "ai_db.h"

#include <stdio.h>
#include <string.h>

#include "db.h"
#include "json.h"

static const char COLUMNS_SQL[] =
    "SELECT c.TABLE_NAME, t.TABLE_TYPE, c.COLUMN_NAME, c.COLUMN_TYPE, c.IS_NULLABLE, "
    "c.COLUMN_KEY, c.EXTRA "
    "FROM information_schema.COLUMNS c "
    "JOIN information_schema.TABLES t "
    "ON t.TABLE_SCHEMA = c.TABLE_SCHEMA AND t.TABLE_NAME = c.TABLE_NAME "
    "WHERE c.TABLE_SCHEMA = DATABASE() "
    "ORDER BY c.TABLE_NAME, c.ORDINAL_POSITION";

static const char FKEYS_SQL[] =
    "SELECT TABLE_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME "
    "FROM information_schema.KEY_COLUMN_USAGE "
    "WHERE TABLE_SCHEMA = DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL "
    "ORDER BY TABLE_NAME, COLUMN_NAME";

static const char *cell(const db_set *s, size_t r, unsigned c) {
    const char *v = db_cell(s, r, c);
    return v ? v : "";
}

char *ai_schema(MYSQL *conn, size_t max_bytes) {
    db_exec_opts eo = { 0 };
    db_outcome cols, fks;
    if (db_execute(conn, COLUMNS_SQL, &eo, &cols) != 0 || !cols.nsets) {
        db_outcome_free(&cols);
        return NULL;
    }
    int have_fks = db_execute(conn, FKEYS_SQL, &eo, &fks) == 0 && fks.nsets;

    strbuf b = { 0 };
    const db_set *s = &cols.sets[0];
    size_t left_out = 0;
    const char *cur = NULL;
    int skipping = 0;
    for (size_t r = 0; r < s->nrows; r++) {
        const char *table = cell(s, r, 0);
        if (!cur || strcmp(cur, table) != 0) {
            cur = table;
            if (!skipping && b.len >= max_bytes) skipping = 1;
            if (skipping) { left_out++; continue; }
            const char *type = cell(s, r, 1);
            sb_add(&b, strstr(type, "VIEW") ? "VIEW " : "TABLE ");
            sb_add(&b, table);
            sb_addc(&b, '\n');
        } else if (skipping) {
            continue;
        }
        sb_add(&b, "  ");
        sb_add(&b, cell(s, r, 2));
        sb_addc(&b, ' ');
        sb_add(&b, cell(s, r, 3));
        if (strcmp(cell(s, r, 4), "NO") == 0) sb_add(&b, " NOT NULL");
        const char *key = cell(s, r, 5);
        if (*key) { sb_addc(&b, ' '); sb_add(&b, key); }
        const char *extra = cell(s, r, 6);
        if (*extra) { sb_addc(&b, ' '); sb_add(&b, extra); }
        sb_addc(&b, '\n');
    }

    if (have_fks && fks.sets[0].nrows) {
        const db_set *f = &fks.sets[0];
        sb_add(&b, "FOREIGN KEYS\n");
        for (size_t r = 0; r < f->nrows; r++) {
            sb_add(&b, "  ");
            sb_add(&b, cell(f, r, 0)); sb_addc(&b, '.'); sb_add(&b, cell(f, r, 1));
            sb_add(&b, " -> ");
            sb_add(&b, cell(f, r, 2)); sb_addc(&b, '.'); sb_add(&b, cell(f, r, 3));
            sb_addc(&b, '\n');
        }
    }
    if (left_out) {
        char note[128];
        snprintf(note, sizeof note,
                 "(schema shortened: %zu more table%s not listed)\n", left_out,
                 left_out == 1 ? "" : "s");
        sb_add(&b, note);
    }
    db_outcome_free(&cols);
    db_outcome_free(&fks);
    return sb_take(&b);
}
