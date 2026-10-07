/* spell.c -- !Write's spelling: the prepared dictionary of ports/rossqlite
 * (SymSpell's frequency list as two SQLite tables), read through SQLite
 * over the RISC OS VFS (ports/rossqlite/module/rosvfs.c).
 *
 *   words(term, freq)            the exact check
 *   deletes(del, term, freq)     each word under each of its one-letter
 *                                deletes: the distance-1 candidates
 *
 * A word is checked in lower case.  The dictionary has no apostrophes, so
 * a word with one is right when what comes before it is and what follows
 * is one of English's endings ('s 't 'll 're 've 'd 'm).  Suggestions are
 * the word's own deletes and the words filed under it and under each of
 * its deletes, kept when within Damerau-Levenshtein 1, best first by
 * frequency, as SymSpell ranks them.  (ROSWRITE's queries, 4 Oct 2026.)
 *
 * The user's own words are a text file, one word to a line, in lower case
 * (Choices:Write.Words, read at the start; Learn and Forget write it again
 * whole): a word in it is right, as a dictionary word is. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

#include "spell.h"

static sqlite3 *sdb;
static sqlite3_stmt *q_word;
static sqlite3_stmt *q_del;

#define MAX_TERM 40

int spell_open(const char *db_path)
{
    if (sdb)
        return 0;
    if (sqlite3_initialize() != SQLITE_OK)
        return -1;
    if (sqlite3_open_v2(db_path, &sdb, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK
        /* a small page cache: the default's 2000 pages would grow past the
         * application's memory, and a lookup then fails */
        || sqlite3_exec(sdb, "PRAGMA cache_size=-1024", NULL, NULL, NULL) != SQLITE_OK
        || sqlite3_prepare_v2(sdb, "SELECT freq FROM words WHERE term=?1", -1, &q_word, NULL) != SQLITE_OK
        || sqlite3_prepare_v2(sdb, "SELECT term, freq FROM deletes WHERE del=?1", -1, &q_del, NULL) != SQLITE_OK) {
        spell_close();
        return -1;
    }
    return 0;
}

void spell_close(void)
{
    if (q_word)
        sqlite3_finalize(q_word);
    if (q_del)
        sqlite3_finalize(q_del);
    if (sdb)
        sqlite3_close(sdb);
    q_word = q_del = NULL;
    sdb = NULL;
}

int spell_ready(void)
{
    return q_word != NULL;
}

/* lower case, Latin-1; 0 if too long or nothing */
static int lower(const char *w, char *out, int n)
{
    int i;
    for (i = 0; w[i]; i++) {
        unsigned char c = (unsigned char)w[i];
        if (i >= n - 1)
            return 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
            c = (unsigned char)(c + 32);
        out[i] = (char)c;
    }
    out[i] = 0;
    return i;
}

/* the word's frequency, 0 if it is not in the dictionary, -1 if the
 * dictionary could not say */
static long long known(const char *lc)
{
    long long freq = 0;
    sqlite3_reset(q_word);
    sqlite3_bind_text(q_word, 1, lc, -1, SQLITE_STATIC);
    {
        int rc = sqlite3_step(q_word), tries = 1;
        while (rc != SQLITE_ROW && rc != SQLITE_DONE && tries--) {
            sqlite3_reset(q_word);          /* a failure (memory, a read): once more */
            rc = sqlite3_step(q_word);
        }
        if (rc == SQLITE_ROW)
            freq = sqlite3_column_int64(q_word, 0) | 1;    /* never 0 when found */
        else if (rc != SQLITE_DONE)
            freq = -1;                      /* unjudged: not a misspelling */
    }
    sqlite3_reset(q_word);
    return freq;
}

/* ---- the user's words ------------------------------------------------------------ */

static char **uw;                   /* sorted, lower case */
static int nuw, capuw;
static char uw_path[256];           /* where they are written */

static int uw_find(const char *lc, int *at)
{
    int lo = 0, hi = nuw - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(uw[mid], lc);
        if (!c) {
            *at = mid;
            return 1;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    *at = lo;
    return 0;
}

static int uw_insert(const char *lc)
{
    int at;
    char *copy;
    if (uw_find(lc, &at))
        return 0;
    if (nuw == capuw) {
        char **more = realloc(uw, (size_t)(capuw ? capuw * 2 : 64) * sizeof *uw);
        if (!more)
            return -1;
        uw = more;
        capuw = capuw ? capuw * 2 : 64;
    }
    if (!(copy = malloc(strlen(lc) + 1)))
        return -1;
    strcpy(copy, lc);
    memmove(uw + at + 1, uw + at, (size_t)(nuw - at) * sizeof *uw);
    uw[at] = copy;
    nuw++;
    return 1;
}

static int uw_has(const char *lc)
{
    int at;
    return nuw && uw_find(lc, &at);
}

static int lower(const char *w, char *out, int n);
static void cache_clear(void);

void spell_user_load(const char *read_path, const char *write_path)
{
    char line[SPELL_WORD + 8], lc[SPELL_WORD];
    FILE *f;
    strncpy(uw_path, write_path, sizeof uw_path - 1);
    if (!(f = fopen(read_path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (n && lower(line, lc, sizeof lc))
            uw_insert(lc);
    }
    fclose(f);
}

static int uw_save(void)
{
    FILE *f;
    int i;
    if (!uw_path[0] || !(f = fopen(uw_path, "w")))
        return -1;
    for (i = 0; i < nuw; i++)
        fprintf(f, "%s\n", uw[i]);
    return fclose(f) ? -1 : 0;
}

int spell_user_has(const char *word)
{
    char lc[SPELL_WORD];
    return lower(word, lc, sizeof lc) && uw_has(lc);
}

int spell_user_learn(const char *word)
{
    char lc[SPELL_WORD];
    if (!lower(word, lc, sizeof lc) || uw_insert(lc) < 0)
        return -1;
    cache_clear();
    return uw_save();
}

int spell_user_forget(const char *word)
{
    char lc[SPELL_WORD];
    int at;
    if (!lower(word, lc, sizeof lc) || !nuw || !uw_find(lc, &at))
        return 0;
    free(uw[at]);
    memmove(uw + at, uw + at + 1, (size_t)(nuw - at - 1) * sizeof *uw);
    nuw--;
    cache_clear();
    return uw_save();
}

/* ---- checking -------------------------------------------------------------------- */

int spell_check(const char *word)
{
    static const char *const endings[] = { "s", "t", "ll", "re", "ve", "d", "m" };
    char lc[MAX_TERM + 1], *apos;
    unsigned i;
    if (!q_word)
        return 1;                       /* no dictionary: nothing is wrong */
    if (!lower(word, lc, sizeof lc))
        return 1;                       /* too long to judge */
    for (i = 0; lc[i]; i++)
        if (lc[i] >= '0' && lc[i] <= '9')
            return 1;                   /* numbers and the like */
    if (uw_has(lc) || known(lc))        /* the user's, found, or unjudged */
        return 1;
    if ((apos = strchr(lc, '\'')) != NULL) {
        *apos = 0;
        for (i = 0; i < sizeof endings / sizeof endings[0]; i++)
            if (!strcmp(apos + 1, endings[i]))
                return lc[0] && (uw_has(lc) || known(lc) != 0);
    }
    return 0;
}

/* Damerau-Levenshtein at most 1, as SymSpell verifies its candidates */
static int within1(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b), i;
    if (la == lb) {
        int diff = 0, first = -1;
        for (i = 0; i < la; i++)
            if (a[i] != b[i]) {
                if (first < 0)
                    first = i;
                diff++;
            }
        if (diff <= 1)
            return 1;
        /* a transposition of neighbours */
        return diff == 2 && first + 1 < la && a[first] == b[first + 1] && a[first + 1] == b[first];
    }
    if (la - lb == 1 || lb - la == 1) {
        const char *s = la < lb ? a : b, *l = la < lb ? b : a;
        int j = 0, skipped = 0;
        for (i = 0; s[i] && l[j];) {
            if (s[i] == l[j])
                i++, j++;
            else if (++skipped > 1)
                return 0;
            else
                j++;
        }
        return 1;
    }
    return 0;
}

struct cand {
    char term[MAX_TERM + 1];
    long long freq;
};

static void consider(struct cand *list, int *n, int max, const char *word, const char *t, long long f)
{
    int i, worst = -1;
    if (!strcmp(t, word) || strlen(t) > MAX_TERM || !within1(word, t))
        return;
    for (i = 0; i < *n; i++)
        if (!strcmp(list[i].term, t))
            return;
    if (*n < max) {
        strcpy(list[*n].term, t);
        list[(*n)++].freq = f;
        return;
    }
    for (i = 0; i < *n; i++)
        if (worst < 0 || list[i].freq < list[worst].freq)
            worst = i;
    if (f > list[worst].freq) {
        strcpy(list[worst].term, t);
        list[worst].freq = f;
    }
}

static void filed_under(struct cand *list, int *n, int max, const char *word, const char *key)
{
    sqlite3_reset(q_del);
    sqlite3_bind_text(q_del, 1, key, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(q_del) == SQLITE_ROW)
        consider(list, n, max, word, (const char *)sqlite3_column_text(q_del, 0),
                 sqlite3_column_int64(q_del, 1));
    sqlite3_reset(q_del);
}

int spell_suggest(const char *word, char out[][SPELL_WORD], int max)
{
    struct cand list[SPELL_MAX];
    char lc[MAX_TERM + 1], del[MAX_TERM + 1];
    int len, i, j, n = 0, k;
    int upper = (word[0] >= 'A' && word[0] <= 'Z') || ((unsigned char)word[0] >= 0xC0
                                                      && (unsigned char)word[0] <= 0xDE);
    if (!q_del || max <= 0 || !(len = lower(word, lc, sizeof lc)))
        return 0;
    if (max > SPELL_MAX)
        max = SPELL_MAX;
    filed_under(list, &n, max, lc, lc);             /* insertions, and lc's own deletes' words */
    for (i = 0; i < len; i++) {                     /* each of lc's deletes */
        for (j = k = 0; j < len; j++)
            if (j != i)
                del[k++] = lc[j];
        del[k] = 0;
        {
            long long f = known(del);
            if (f > 0)
                consider(list, &n, max, lc, del, f);  /* the delete is itself a word */
        }
        filed_under(list, &n, max, lc, del);
    }
    /* best first; the word's capital kept */
    for (i = 0; i < n; i++) {
        int best = i;
        struct cand t;
        for (j = i + 1; j < n; j++)
            if (list[j].freq > list[best].freq)
                best = j;
        t = list[i], list[i] = list[best], list[best] = t;
        strncpy(out[i], list[i].term, SPELL_WORD - 1);
        out[i][SPELL_WORD - 1] = 0;
        if (upper && out[i][0] >= 'a' && out[i][0] <= 'z')
            out[i][0] = (char)(out[i][0] - 32);
    }
    return n;
}

/* ---- the verdicts seen, for marking as one types ---------------------------------- */

/* every word on the screen is checked at each redraw: each word's verdict
 * is kept here (open addressing; emptied when three quarters full), so the
 * dictionary is asked once a word */
#define CACHE 4096
static struct {
    char word[SPELL_WORD];
    signed char right;              /* 1, 0; -1 an empty slot */
} cache[CACHE];
static int cached = -1;             /* -1 before the first use */

static void cache_clear(void)
{
    int i;
    for (i = 0; i < CACHE; i++)
        cache[i].right = -1;
    cached = 0;
}

int spell_check_cached(const char *word)
{
    unsigned h = 2166136261u, i;
    const char *p;
    if (!q_word || strlen(word) >= SPELL_WORD)
        return 1;
    if (cached < 0 || cached > CACHE * 3 / 4)
        cache_clear();
    for (p = word; *p; p++)
        h = (h ^ (unsigned char)*p) * 16777619u;
    for (i = h % CACHE;; i = (i + 1) % CACHE) {
        if (cache[i].right < 0) {
            strcpy(cache[i].word, word);
            cache[i].right = (signed char)spell_check(word);
            cached++;
            return cache[i].right;
        }
        if (!strcmp(cache[i].word, word))
            return cache[i].right;
    }
}
