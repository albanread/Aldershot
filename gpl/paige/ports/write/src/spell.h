/* spell.h -- !Write's spelling over the prepared dictionary (spell.c) */
#ifndef SPELL_H
#define SPELL_H

#define SPELL_MAX 8                 /* suggestions offered */
#define SPELL_WORD 41               /* a word's room, its terminator too */

int spell_open(const char *db_path);        /* 0, or -1: no dictionary */
void spell_close(void);
int spell_ready(void);
int spell_check(const char *word);          /* 1 if right (or unjudged) */
int spell_check_cached(const char *word);   /* the same, each word asked once */
/* the user's own words: read from read_path, written to write_path */
void spell_user_load(const char *read_path, const char *write_path);
int spell_user_has(const char *word);
int spell_user_learn(const char *word);     /* 0, or -1: not written */
int spell_user_forget(const char *word);
/* up to max suggestions, best first, the word's capital kept: how many */
int spell_suggest(const char *word, char out[][SPELL_WORD], int max);

#endif
