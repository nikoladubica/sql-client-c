#include "ai.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "json.h"

#define API_VERSION "2026-10-21"
/* fallbacks: "default" re-runs a request a safety classifier declined on the
 * model Anthropic recommends for that case, inside the same call. */
#define FALLBACK_BETA "server-side-fallback-2026-07-01"
#define MAX_TOKENS 16000
#define MAX_RESPONSE (8u << 20)

static const char SYSTEM_PROMPT[] =
    "You write SQL for sqlc, a MySQL/MariaDB client. The user describes what they "
    "need in plain language; you reply with SQL that does it on the connected server. "
    "The user reads your SQL in an editor and decides whether to run it, so it has to "
    "be correct and easy to check.\n"
    "\n"
    "- Use only the tables and columns in the schema below, with their exact names. "
    "Quote identifiers with backticks when they need it.\n"
    "- Write SQL that works on the given server and version: MySQL and MariaDB differ "
    "(for example JSON functions, window functions, RETURNING, sequences).\n"
    "- Prefer read-only queries. Write statements that change data or structure "
    "(INSERT, UPDATE, DELETE, DROP, ALTER, TRUNCATE, ...) only when the user asks for "
    "that change, and keep their scope exactly to what was asked (an UPDATE or DELETE "
    "always has the WHERE clause the request implies).\n"
    "- Give exploratory SELECTs a sensible LIMIT unless the user wants all rows.\n"
    "- If the request is ambiguous, pick the most likely reading and say which one in "
    "the explanation. If it can't be done with this schema, say why in the "
    "explanation and leave sql empty.\n"
    "\n"
    "Reply fields:\n"
    "- explanation: plain text, no Markdown. A few short sentences on what the SQL "
    "does and any assumptions you made. It is shown as SQL comments above the query.\n"
    "- sql: only the statement(s), each ending with a semicolon, formatted over "
    "several lines for readability. No comments and no Markdown code fences.";

static const char OUTPUT_SCHEMA[] =
    "{\"type\":\"object\","
    "\"properties\":{\"explanation\":{\"type\":\"string\"},\"sql\":{\"type\":\"string\"}},"
    "\"required\":[\"explanation\",\"sql\"],\"additionalProperties\":false}";

void ai_result_free(ai_result *r) {
    if (!r) return;
    free(r->explanation);
    free(r->sql);
    free(r->model);
    free(r->error);
    memset(r, 0, sizeof *r);
}

static void set_error(ai_result *out, const char *fmt, const char *a, const char *b) {
    char buf[1024];
    snprintf(buf, sizeof buf, fmt, a ? a : "", b ? b : "");
    free(out->error);
    out->error = strdup(buf);
}

/* ---------- request ---------- */

char *ai_build_request(const char *model, const char *server, const char *database,
                       const char *schema, const char *task) {
    strbuf ctx = { 0 };
    sb_add(&ctx, "Server: ");
    sb_add(&ctx, server && *server ? server : "unknown");
    sb_add(&ctx, "\nCurrent database: ");
    sb_add(&ctx, database && *database ? database : "(none selected)");
    sb_add(&ctx, "\n\nSchema of the current database:\n");
    sb_add(&ctx, schema && *schema ? schema : "(no tables)\n");
    char *context = sb_take(&ctx);
    if (!context) return NULL;

    strbuf b = { 0 };
    sb_add(&b, "{\"model\":");
    sb_add_json_string(&b, model ? model : AI_MODEL);
    sb_add(&b, ",\"max_tokens\":");
    char n[32];
    snprintf(n, sizeof n, "%d", MAX_TOKENS);
    sb_add(&b, n);
    /* Instructions first, then the schema, cached: asking again about the
     * same database reuses it. */
    sb_add(&b, ",\"system\":[{\"type\":\"text\",\"text\":");
    sb_add_json_string(&b, SYSTEM_PROMPT);
    sb_add(&b, "},{\"type\":\"text\",\"text\":");
    sb_add_json_string(&b, context);
    sb_add(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}}]");
    sb_add(&b, ",\"messages\":[{\"role\":\"user\",\"content\":");
    sb_add_json_string(&b, task ? task : "");
    sb_add(&b, "}]");
    sb_add(&b, ",\"output_config\":{\"effort\":\"medium\",\"format\":{\"type\":\"json_schema\",\"schema\":");
    sb_add(&b, OUTPUT_SCHEMA);
    sb_add(&b, "}}");
    sb_add(&b, ",\"fallbacks\":\"default\"}");
    free(context);
    return sb_take(&b);
}

/* ---------- response ---------- */

static const char *status_hint(long status) {
    switch (status) {
    case 400: return "The request was rejected";
    case 401: return "The API key was rejected - check it";
    case 403: return "This API key isn't allowed to do that";
    case 404: return "Not found";
    case 413: return "The request is too large (the schema may be too big)";
    case 429: return "Rate limited - wait a moment and try again";
    case 529: return "The API is overloaded - try again shortly";
    default:  return status >= 500 ? "The API had an internal error - try again" : "Request failed";
    }
}

int ai_parse_response(long http_status, const char *body, ai_result *out) {
    json_value *root = json_parse(body);

    if (http_status != 200) {
        const json_value *err = json_get(root, "error");
        const char *msg = json_get_str(err, "message");
        char status[96];
        snprintf(status, sizeof status, "%s (HTTP %ld)", status_hint(http_status), http_status);
        if (msg) set_error(out, "%s: %s", status, msg);
        else set_error(out, "%s%s", status, "");
        json_free(root);
        return -1;
    }
    if (!root || root->type != JSON_OBJECT) {
        set_error(out, "The API sent a reply that isn't valid JSON%s%s", "", "");
        json_free(root);
        return -1;
    }

    const char *model = json_get_str(root, "model");
    if (model) out->model = strdup(model);

    const char *stop = json_get_str(root, "stop_reason");
    if (stop && strcmp(stop, "refusal") == 0) {
        const char *why = json_get_str(json_get(root, "stop_details"), "explanation");
        set_error(out, "Claude declined this request.%s%s", why ? " " : "", why);
        json_free(root);
        return -1;
    }
    if (stop && strcmp(stop, "max_tokens") == 0) {
        set_error(out, "Claude's reply was cut off (max_tokens). Try a narrower request.%s%s", "", "");
        json_free(root);
        return -1;
    }

    /* Text blocks of the model that finished: anything before a fallback
     * block came from a model that declined. */
    strbuf text = { 0 };
    const json_value *content = json_get(root, "content");
    if (content && content->type == JSON_ARRAY) {
        for (size_t i = 0; i < content->n; i++) {
            const json_value *blk = &content->items[i];
            const char *type = json_get_str(blk, "type");
            if (!type) continue;
            if (strcmp(type, "fallback") == 0) {
                sb_free(&text);
            } else if (strcmp(type, "text") == 0) {
                const char *t = json_get_str(blk, "text");
                if (t) sb_add(&text, t);
            }
        }
    }
    char *reply = sb_take(&text);
    json_free(root);

    json_value *obj = json_parse(reply);
    const char *expl = json_get_str(obj, "explanation");
    const char *sql = json_get_str(obj, "sql");
    if (!expl || !sql) {
        set_error(out, "Claude's reply wasn't in the expected format%s%s", "", "");
        json_free(obj);
        free(reply);
        return -1;
    }
    out->explanation = strdup(expl);
    out->sql = strdup(sql);
    json_free(obj);
    free(reply);
    return 0;
}

/* ---------- HTTP ---------- */

static size_t on_body(char *data, size_t size, size_t nmemb, void *userp) {
    strbuf *b = userp;
    size_t n = size * nmemb;
    if (b->len + n > MAX_RESPONSE) return 0; /* aborts the transfer */
    sb_addn(b, data, n);
    return b->oom ? 0 : n;
}

int ai_generate(const char *api_key, const char *request_json, ai_result *out) {
    if (!api_key || !*api_key) {
        set_error(out, "No Claude API key is set.%s%s", "", "");
        return -1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) { set_error(out, "Could not start an HTTP request%s%s", "", ""); return -1; }

    size_t hlen = strlen(api_key) + 16;
    char *key_header = malloc(hlen);
    if (!key_header) { curl_easy_cleanup(curl); set_error(out, "Out of memory%s%s", "", ""); return -1; }
    snprintf(key_header, hlen, "x-api-key: %s", api_key);

    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "content-type: application/json");
    h = curl_slist_append(h, "anthropic-version: " API_VERSION);
    h = curl_slist_append(h, "anthropic-beta: " FALLBACK_BETA);
    h = curl_slist_append(h, key_header);

    strbuf body = { 0 };
    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, AI_API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sqlc");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); /* we're on a worker thread */
#ifdef _WIN32
    /* trust Windows' certificate store, not a CA bundle path from the build machine */
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(h);
    memset(key_header, 0, hlen);
    free(key_header);
    curl_easy_cleanup(curl);

    int result;
    if (rc != CURLE_OK) {
        set_error(out, "Could not reach the Claude API: %s%s",
                  errbuf[0] ? errbuf : curl_easy_strerror(rc), "");
        result = -1;
    } else {
        result = ai_parse_response(status, body.s ? body.s : "", out);
    }
    sb_free(&body);
    return result;
}

/* ---------- editor text ---------- */

/* Appends text as `-- ` comment lines wrapped at about width columns. */
static void add_comment(strbuf *b, const char *text, size_t width) {
    const char *p = text;
    for (;;) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        /* one input line, wrapped on spaces */
        const char *s = p, *end = p + linelen;
        do {
            while (s < end && *s == ' ') s++;
            const char *cut = end;
            if ((size_t)(end - s) > width) {
                cut = s + width;
                while (cut > s && *cut != ' ') cut--;
                if (cut == s) { /* one long word */
                    cut = s + width;
                    while (cut < end && *cut != ' ') cut++;
                }
            }
            const char *t = cut;
            while (t > s && (t[-1] == ' ' || t[-1] == '\r')) t--;
            sb_add(b, t > s ? "-- " : "--");
            sb_addn(b, s, (size_t)(t - s));
            sb_addc(b, '\n');
            s = cut;
        } while (s < end && *s);
        if (!eol) break;
        p = eol + 1;
    }
}

char *ai_format(const char *task, const ai_result *r) {
    strbuf b = { 0 };
    char *t = strdup(task ? task : "");
    if (t) {
        size_t n = strlen(t);
        while (n && isspace((unsigned char)t[n - 1])) t[--n] = '\0';
        size_t len = strlen(t) + 16;
        char *line = malloc(len);
        if (line) {
            snprintf(line, len, "Request: %s", t);
            add_comment(&b, line, 76);
            free(line);
        }
        free(t);
    }
    if (r->explanation && *r->explanation) {
        sb_add(&b, "--\n");
        add_comment(&b, r->explanation, 76);
    }
    if (r->sql && *r->sql) {
        sb_addc(&b, '\n');
        const char *s = r->sql;
        while (*s == '\n' || *s == '\r') s++;
        sb_add(&b, s);
        if (b.len && b.s[b.len - 1] != '\n') sb_addc(&b, '\n');
    } else {
        sb_add(&b, "--\n-- (Claude didn't write a query for this request.)\n");
    }
    return sb_take(&b);
}

/* ---------- destructive statements ---------- */

#define MAX_WORDS 8

typedef struct {
    char first[MAX_WORDS][24]; /* the first words of the statement, upper case */
    int nfirst;
    int has_where, has_drop, has_from_select;
    int nwords;
} stmt_info;

static void note(strbuf *found, const char *what) {
    if (found->s && strstr(found->s, what)) return; /* once each */
    if (found->len) sb_add(found, ", ");
    sb_add(found, what);
}

static void classify(const stmt_info *st, strbuf *found) {
    if (st->nfirst == 0) return; /* WITH ... DELETE/UPDATE is handled by the caller */
    const char *w = st->first[0];
    if (strcmp(w, "DROP") == 0) {
        char what[48] = "DROP";
        for (int i = 1; i < st->nfirst; i++) {
            const char *o = st->first[i];
            if (!strcmp(o, "TABLE") || !strcmp(o, "DATABASE") || !strcmp(o, "SCHEMA") ||
                !strcmp(o, "VIEW") || !strcmp(o, "INDEX") || !strcmp(o, "USER") ||
                !strcmp(o, "TRIGGER") || !strcmp(o, "PROCEDURE") || !strcmp(o, "FUNCTION") ||
                !strcmp(o, "EVENT") || !strcmp(o, "SEQUENCE")) {
                snprintf(what, sizeof what, "DROP %s", o);
                break;
            }
        }
        note(found, what);
    } else if (strcmp(w, "TRUNCATE") == 0) {
        note(found, "TRUNCATE");
    } else if (strcmp(w, "DELETE") == 0) {
        note(found, st->has_where ? "DELETE" : "DELETE without WHERE");
    } else if (strcmp(w, "UPDATE") == 0) {
        if (!st->has_where) note(found, "UPDATE without WHERE");
    } else if (strcmp(w, "ALTER") == 0) {
        if (st->has_drop) note(found, "ALTER ... DROP");
    }
}

char *ai_destructive(const char *sql) {
    strbuf found = { 0 };
    stmt_info st;
    memset(&st, 0, sizeof st);
    int with_dml = 0;          /* WITH ... then DELETE / UPDATE seen */
    int depth = 0;
    const char *p = sql ? sql : "";

    for (;;) {
        char c = *p;
        if (c == '\0' || c == ';') {
            if (with_dml == 'D') note(&found, st.has_where ? "DELETE" : "DELETE without WHERE");
            else if (with_dml == 'U' && !st.has_where) note(&found, "UPDATE without WHERE");
            classify(&st, &found);
            memset(&st, 0, sizeof st);
            with_dml = 0;
            depth = 0;
            if (c == '\0') break;
            p++;
            continue;
        }
        /* comments */
        if (c == '#' || (c == '-' && p[1] == '-' && (p[2] == ' ' || p[2] == '\t' ||
                                                      p[2] == '\n' || p[2] == '\r' || p[2] == '\0'))) {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            /* executable comment (MySQL "/" "*!", MariaDB "/" "*M!"): its body is SQL */
            if (p[2] == '!' || (p[2] == 'M' && p[3] == '!')) {
                p += p[2] == '!' ? 3 : 4;
                while (isdigit((unsigned char)*p)) p++;
                continue;
            }
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        if (c == '*' && p[1] == '/') { p += 2; continue; } /* end of an executable comment */
        /* quoted strings and identifiers */
        if (c == '\'' || c == '"' || c == '`') {
            p++;
            while (*p) {
                if (*p == '\\' && c != '`' && p[1]) { p += 2; continue; }
                if (*p == c) {
                    if (p[1] == c) { p += 2; continue; } /* doubled quote */
                    break;
                }
                p++;
            }
            if (*p) p++;
            st.nwords++;
            continue;
        }
        if (c == '(') { depth++; p++; continue; }
        if (c == ')') { if (depth) depth--; p++; continue; }
        if (isalpha((unsigned char)c) || c == '_') {
            char word[24];
            size_t n = 0;
            while (isalnum((unsigned char)*p) || *p == '_' || *p == '$') {
                if (n < sizeof word - 1) word[n++] = (char)toupper((unsigned char)*p);
                p++;
            }
            word[n] = '\0';
            if (st.nfirst < MAX_WORDS && st.nwords == st.nfirst)
                memcpy(st.first[st.nfirst++], word, n + 1);
            st.nwords++;
            if (!strcmp(word, "WHERE") && depth == 0) st.has_where = 1;
            if (!strcmp(word, "DROP")) st.has_drop = 1;
            if (st.nfirst && !strcmp(st.first[0], "WITH") && depth == 0 && !with_dml) {
                if (!strcmp(word, "DELETE")) with_dml = 'D';
                else if (!strcmp(word, "UPDATE")) with_dml = 'U';
            }
            continue;
        }
        if (!isspace((unsigned char)c)) st.nwords++;
        p++;
    }
    if (!found.len) { sb_free(&found); return NULL; }
    return sb_take(&found);
}
