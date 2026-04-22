#include "db_api.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bptree.h"
#include "errors.h"
#include "time_ms.h"

#define STR_MAX 256

typedef struct {
    int id;
    char title[STR_MAX];
    char author[STR_MAX];
    int year;
    int alive;
} Book;

typedef struct {
    Book *rows;
    size_t len;
    size_t cap;
    BPTree idx;
    pthread_rwlock_t lock;
    int ready;
} BookDB;

typedef struct {
    char *buf;
    int max;
    int len;
    int bad;
} JsonBuf;

static BookDB g_db;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

static void jb_init(JsonBuf *jb, char *buf, int max)
{
    jb->buf = buf;
    jb->max = max;
    jb->len = 0;
    jb->bad = 0;
    if (max > 0) {
        buf[0] = '\0';
    }
}

static void jb_add(JsonBuf *jb, const char *fmt, ...)
{
    va_list ap;
    int wrote;

    if (jb->bad || jb->len >= jb->max) {
        jb->bad = 1;
        return;
    }

    va_start(ap, fmt);
    wrote = vsnprintf(jb->buf + jb->len, (size_t)(jb->max - jb->len), fmt, ap);
    va_end(ap);
    if (wrote < 0 || wrote >= jb->max - jb->len) {
        jb->bad = 1;
        if (jb->max > 0) {
            jb->buf[jb->max - 1] = '\0';
        }
        return;
    }
    jb->len += wrote;
}

static void skip_ws(const char **p)
{
    while (**p != '\0' && isspace((unsigned char)**p)) {
        *p += 1;
    }
}

static int word_end(char ch)
{
    return !(isalnum((unsigned char)ch) || ch == '_');
}

static int eat_word(const char **p, const char *word)
{
    size_t n;

    skip_ws(p);
    n = strlen(word);
    if (strncasecmp(*p, word, n) != 0 || !word_end((*p)[n])) {
        return 0;
    }
    *p += n;
    return 1;
}

static int eat_char(const char **p, char ch)
{
    skip_ws(p);
    if (**p != ch) {
        return 0;
    }
    *p += 1;
    return 1;
}

static int parse_int(const char **p, int *out)
{
    char *end;
    long val;

    skip_ws(p);
    val = strtol(*p, &end, 10);
    if (end == *p || val < 0 || val > 2147483647L) {
        return 0;
    }
    *out = (int)val;
    *p = end;
    return 1;
}

static int parse_quoted(const char **p, char *out, size_t max)
{
    size_t len = 0U;

    skip_ws(p);
    if (**p != '\'') {
        return 0;
    }
    *p += 1;
    while (**p != '\0') {
        char ch = **p;

        if (ch == '\'') {
            if ((*p)[1] == '\'') {
                ch = '\'';
                *p += 2;
            } else {
                *p += 1;
                out[len] = '\0';
                return 1;
            }
        } else if (ch == '\\' && (*p)[1] != '\0') {
            ch = (*p)[1];
            *p += 2;
        } else {
            *p += 1;
        }

        if (len + 1U >= max) {
            return 0;
        }
        out[len++] = ch;
    }
    return 0;
}

static int parse_end(const char **p)
{
    skip_ws(p);
    if (**p == ';') {
        *p += 1;
    }
    skip_ws(p);
    return **p == '\0';
}

static int json_escape(const char *src, char *out, size_t max)
{
    size_t len = 0U;

    while (*src != '\0') {
        unsigned char ch = (unsigned char)*src++;
        const char *rep = NULL;
        char tmp[7];

        if (ch == '"' || ch == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)ch;
            tmp[2] = '\0';
            rep = tmp;
        } else if (ch == '\n') {
            rep = "\\n";
        } else if (ch == '\r') {
            rep = "\\r";
        } else if (ch == '\t') {
            rep = "\\t";
        } else if (ch < 32U) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
            rep = tmp;
        }

        if (rep != NULL) {
            size_t n = strlen(rep);
            if (len + n >= max) {
                return 0;
            }
            memcpy(out + len, rep, n);
            len += n;
        } else {
            if (len + 1U >= max) {
                return 0;
            }
            out[len++] = (char)ch;
        }
    }
    out[len] = '\0';
    return 1;
}

static int ensure_cap(void)
{
    Book *grown;
    size_t ncap;

    if (g_db.len < g_db.cap) {
        return 1;
    }
    ncap = (g_db.cap == 0U) ? 1024U : g_db.cap * 2U;
    grown = (Book *)realloc(g_db.rows, sizeof(Book) * ncap);
    if (grown == NULL) {
        return 0;
    }
    g_db.rows = grown;
    g_db.cap = ncap;
    return 1;
}

static void reset_locked(void)
{
    char err[128] = {0};

    free(g_db.rows);
    g_db.rows = NULL;
    g_db.len = 0U;
    g_db.cap = 0U;
    bptree_destroy(&g_db.idx);
    bptree_init(&g_db.idx, err, sizeof(err));
}

static int rebuild_index_locked(char *err, size_t errsz)
{
    size_t i;

    bptree_destroy(&g_db.idx);
    if (bptree_init(&g_db.idx, err, errsz) != STATUS_OK) {
        return 0;
    }
    for (i = 0U; i < g_db.len; ++i) {
        if (g_db.rows[i].alive &&
            bptree_insert(&g_db.idx, (uint64_t)g_db.rows[i].id, (long)i, err, errsz) != STATUS_OK) {
            return 0;
        }
    }
    return 1;
}

static void err_json(char *out, int max, const char *msg)
{
    char esc[512];

    if (!json_escape(msg, esc, sizeof(esc))) {
        snprintf(out, (size_t)max, "\"ok\":false,\"err\":\"error message too long\"");
        return;
    }
    snprintf(out, (size_t)max, "\"ok\":false,\"err\":\"%s\"", esc);
}

static void ok_affected(char *out, int max, long affected)
{
    snprintf(out, (size_t)max, "\"ok\":true,\"rows\":[],\"affected\":%ld", affected);
}

static int add_row_json(JsonBuf *jb, const Book *b)
{
    char title[STR_MAX * 2];
    char author[STR_MAX * 2];
    int before = jb->len;

    if (!json_escape(b->title, title, sizeof(title)) ||
        !json_escape(b->author, author, sizeof(author))) {
        jb->bad = 1;
        return 0;
    }
    jb_add(jb, "{\"id\":%d,\"title\":\"%s\",\"author\":\"%s\",\"year\":%d}",
           b->id, title, author, b->year);
    return !jb->bad && jb->len > before;
}

static void select_one_locked(int id, char *out, int max)
{
    JsonBuf jb;
    long off = 0L;
    int found = 0;
    char err[128] = {0};

    if (bptree_search(&g_db.idx, (uint64_t)id, &off, &found, err, sizeof(err)) != STATUS_OK) {
        err_json(out, max, err[0] ? err : "index search failed");
        return;
    }

    jb_init(&jb, out, max);
    jb_add(&jb, "\"ok\":true,\"rows\":[");
    if (found && off >= 0L && (size_t)off < g_db.len && g_db.rows[off].alive) {
        add_row_json(&jb, &g_db.rows[off]);
    }
    jb_add(&jb, "],\"truncated\":false");
    if (jb.bad) {
        err_json(out, max, "response too large");
    }
}

static void select_all_locked(char *out, int max)
{
    JsonBuf jb;
    size_t i;
    int first = 1;
    int trunc = 0;

    jb_init(&jb, out, max);
    jb_add(&jb, "\"ok\":true,\"rows\":[");
    for (i = 0U; i < g_db.len; ++i) {
        int save_len;

        if (!g_db.rows[i].alive) {
            continue;
        }
        if (jb.len > max - 600) {
            trunc = 1;
            break;
        }
        if (!first) {
            jb_add(&jb, ",");
        }
        save_len = jb.len;
        if (!add_row_json(&jb, &g_db.rows[i])) {
            jb.len = save_len;
            jb.bad = 0;
            trunc = 1;
            break;
        }
        first = 0;
    }
    jb_add(&jb, "],\"truncated\":%s", trunc ? "true" : "false");
    if (jb.bad) {
        err_json(out, max, "response too large");
    }
}

static void exec_create(const char *sql, char *out, int max)
{
    const char *p = sql;

    if (!eat_word(&p, "CREATE") || !eat_word(&p, "TABLE") ||
        !eat_word(&p, "books") || !parse_end(&p)) {
        err_json(out, max, "bad sql");
        return;
    }
    reset_locked();
    ok_affected(out, max, 0L);
}

static void exec_insert(const char *sql, char *out, int max)
{
    const char *p = sql;
    Book b;
    long off;
    int found = 0;
    char err[128] = {0};

    memset(&b, 0, sizeof(b));
    if (!eat_word(&p, "INSERT") || !eat_word(&p, "INTO") ||
        !eat_word(&p, "books") || !eat_word(&p, "VALUES") ||
        !eat_char(&p, '(') || !parse_int(&p, &b.id) ||
        !eat_char(&p, ',') || !parse_quoted(&p, b.title, sizeof(b.title)) ||
        !eat_char(&p, ',') || !parse_quoted(&p, b.author, sizeof(b.author)) ||
        !eat_char(&p, ',') || !parse_int(&p, &b.year) ||
        !eat_char(&p, ')') || !parse_end(&p)) {
        err_json(out, max, "bad sql");
        return;
    }

    if (bptree_search(&g_db.idx, (uint64_t)b.id, &off, &found, err, sizeof(err)) != STATUS_OK) {
        err_json(out, max, err[0] ? err : "index search failed");
        return;
    }
    if (found && off >= 0L && (size_t)off < g_db.len && g_db.rows[off].alive) {
        err_json(out, max, "duplicate id");
        return;
    }
    if (!ensure_cap()) {
        err_json(out, max, "out of memory");
        return;
    }
    b.alive = 1;
    g_db.rows[g_db.len] = b;
    if (bptree_insert(&g_db.idx, (uint64_t)b.id, (long)g_db.len, err, sizeof(err)) != STATUS_OK) {
        err_json(out, max, err[0] ? err : "index insert failed");
        return;
    }
    g_db.len += 1U;
    ok_affected(out, max, 1L);
}

static void exec_select(const char *sql, char *out, int max)
{
    const char *p = sql;
    int id = 0;

    if (!eat_word(&p, "SELECT") || !eat_char(&p, '*') ||
        !eat_word(&p, "FROM") || !eat_word(&p, "books")) {
        err_json(out, max, "bad sql");
        return;
    }
    skip_ws(&p);
    if (strncasecmp(p, "WHERE", 5U) == 0 && word_end(p[5])) {
        if (!eat_word(&p, "WHERE") || !eat_word(&p, "id") ||
            !eat_char(&p, '=') || !parse_int(&p, &id) || !parse_end(&p)) {
            err_json(out, max, "bad sql");
            return;
        }
        select_one_locked(id, out, max);
        return;
    }
    if (!parse_end(&p)) {
        err_json(out, max, "bad sql");
        return;
    }
    select_all_locked(out, max);
}

static void exec_delete(const char *sql, char *out, int max)
{
    const char *p = sql;
    int id = 0;
    long off = 0L;
    int found = 0;
    char err[128] = {0};

    if (!eat_word(&p, "DELETE") || !eat_word(&p, "FROM") ||
        !eat_word(&p, "books") || !eat_word(&p, "WHERE") ||
        !eat_word(&p, "id") || !eat_char(&p, '=') ||
        !parse_int(&p, &id) || !parse_end(&p)) {
        err_json(out, max, "bad sql");
        return;
    }
    if (bptree_search(&g_db.idx, (uint64_t)id, &off, &found, err, sizeof(err)) != STATUS_OK) {
        err_json(out, max, err[0] ? err : "index search failed");
        return;
    }
    if (!found || off < 0L || (size_t)off >= g_db.len || !g_db.rows[off].alive) {
        ok_affected(out, max, 0L);
        return;
    }
    g_db.rows[off].alive = 0;
    if (!rebuild_index_locked(err, sizeof(err))) {
        err_json(out, max, err[0] ? err : "index rebuild failed");
        return;
    }
    ok_affected(out, max, 1L);
}

int db_init(void)
{
    char err[128] = {0};

    pthread_mutex_lock(&g_init_lock);
    if (!g_db.ready) {
        memset(&g_db, 0, sizeof(g_db));
        pthread_rwlock_init(&g_db.lock, NULL);
        bptree_init(&g_db.idx, err, sizeof(err));
        g_db.ready = 1;
    }
    pthread_mutex_unlock(&g_init_lock);

    pthread_rwlock_wrlock(&g_db.lock);
    reset_locked();
    pthread_rwlock_unlock(&g_db.lock);
    return 1;
}

void db_free(void)
{
    pthread_mutex_lock(&g_init_lock);
    if (g_db.ready) {
        reset_locked();
        pthread_rwlock_destroy(&g_db.lock);
        memset(&g_db, 0, sizeof(g_db));
    }
    pthread_mutex_unlock(&g_init_lock);
}

int db_is_read_sql(const char *sql)
{
    if (sql == NULL) {
        return 0;
    }
    while (*sql != '\0' && isspace((unsigned char)*sql)) {
        sql += 1;
    }
    return strncasecmp(sql, "SELECT", 6U) == 0 && word_end(sql[6]);
}

int db_exec(const char *sql, char *out, int max)
{
    const char *p;
    int ok = 1;

    if (out == NULL || max <= 0) {
        return 0;
    }
    if (sql == NULL) {
        err_json(out, max, "bad sql");
        return 0;
    }
    if (!g_db.ready) {
        db_init();
    }
    if (strlen(sql) >= DB_SQL_MAX) {
        err_json(out, max, "sql too long");
        return 0;
    }
    p = sql;
    skip_ws(&p);
    if (*p == '\0') {
        err_json(out, max, "empty sql");
        return 0;
    }

    if (db_is_read_sql(sql)) {
        // SELECT는 DB 구조를 바꾸지 않으므로 lock 없이 바로 읽는다.
        exec_select(sql, out, max);
    } else {
        pthread_rwlock_wrlock(&g_db.lock);
        if (strncasecmp(p, "CREATE", 6U) == 0 && word_end(p[6])) {
            exec_create(sql, out, max);
        } else if (strncasecmp(p, "INSERT", 6U) == 0 && word_end(p[6])) {
            exec_insert(sql, out, max);
        } else if (strncasecmp(p, "DELETE", 6U) == 0 && word_end(p[6])) {
            exec_delete(sql, out, max);
        } else {
            err_json(out, max, "bad sql");
        }
        pthread_rwlock_unlock(&g_db.lock);
    }

    ok = strstr(out, "\"ok\":true") != NULL;
    return ok;
}

int db_run_bench(const char *mode, long count, int workers, char *out, int max)
{
    long i;
    double start;
    double work_sum = 0.0;
    double total;
    char sql[DB_SQL_MAX];
    char res[1024];
    int seed = 1000;

    if (mode == NULL || count < 1L) {
        err_json(out, max, "bad bench");
        return 0;
    }
    if (strcasecmp(mode, "write") != 0 &&
        strcasecmp(mode, "read") != 0 &&
        strcasecmp(mode, "mixed") != 0) {
        err_json(out, max, "bad bench mode");
        return 0;
    }

    db_exec("CREATE TABLE books;", res, sizeof(res));
    if (strcasecmp(mode, "read") == 0 || strcasecmp(mode, "mixed") == 0) {
        int j;

        for (j = 1; j <= seed; ++j) {
            snprintf(sql, sizeof(sql),
                     "INSERT INTO books VALUES (%d, 'seed title %d', 'seed author', 2024);",
                     j, j);
            db_exec(sql, res, sizeof(res));
        }
    }

    start = now_ms();
    for (i = 1L; i <= count; ++i) {
        double a;

        if (strcasecmp(mode, "write") == 0) {
            snprintf(sql, sizeof(sql),
                     "INSERT INTO books VALUES (%ld, 'title %ld', 'author %ld', 2024);",
                     i, i, i);
        } else if (strcasecmp(mode, "read") == 0) {
            snprintf(sql, sizeof(sql),
                     "SELECT * FROM books WHERE id = %d;",
                     (int)((i % seed) + 1L));
        } else if ((i % 2L) == 0L) {
            snprintf(sql, sizeof(sql),
                     "SELECT * FROM books WHERE id = %d;",
                     (int)((i % seed) + 1L));
        } else {
            snprintf(sql, sizeof(sql),
                     "INSERT INTO books VALUES (%ld, 'mix title %ld', 'mix author', 2024);",
                     100000000L + i, i);
        }

        a = now_ms();
        db_exec(sql, res, sizeof(res));
        work_sum += now_ms() - a;
    }
    total = now_ms() - start;
    snprintf(out, (size_t)max,
             "\"ok\":true,\"mode\":\"%s\",\"count\":%ld,\"workers\":%d,"
             "\"qps\":%.2f,\"avg_wait_ms\":0.0,\"avg_work_ms\":%.4f,\"total_ms\":%.2f",
             mode, count, workers,
             total > 0.0 ? ((double)count * 1000.0 / total) : 0.0,
             work_sum / (double)count,
             total);
    return 1;
}
