/* engine.c -- !Write's document layer without the desktop: the calls the
 * front end makes, in order, each reported on the serial line, ending in
 * "engine: OK".  test/engine.py runs it in a headless box. */
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "swis.h"

#include "../src/doc.h"
#include "../src/spell.h"

static void say(const char *s)
{
    _swix(OS_Write0, _IN(0), s);
    _swix(OS_NewLine, 0);
}

int main(void)
{
    struct doc *d;
    const char *e;
    const char *text = "Hello world, this is Write.";
    char word[64], line[128];
    long h, top, bottom, b, en;
    int i;

    doc_engine_init();
    say("engine: up");
    d = doc_new();
    say(d ? "engine: new document" : "engine: FAIL no document");
    if (!d)
        return 1;
    sprintf(line, "engine: empty height %ld", doc_height(d));
    say(line);
    for (i = 0; text[i]; i++)
        doc_key(d, (unsigned char)text[i]);
    say("engine: typed");
    doc_undo(d);
    say("engine: typing undone");
    doc_undo(d);
    say("engine: typing redone");

    sprintf(line, "engine: height %ld", doc_height(d));
    say(line);
    if (doc_caret(d, &h, &top, &bottom)) {
        sprintf(line, "engine: caret %ld %ld..%ld", h, top, bottom);
        say(line);
    }
    doc_key(d, 13);
    for (i = 0; i < 40; i++)
        doc_key(d, "Another paragraph that wraps round. "[i % 36]);
    sprintf(line, "engine: height %ld after a second paragraph", doc_height(d));
    say(line);
    doc_key(d, 8);
    doc_key(d, 0x18C);
    doc_key(d, 0x19C);
    doc_select_all(d);
    doc_style(d, DOC_BOLD);
    doc_size(d, 18);
    doc_font(d, "Homerton.Medium");
    say("engine: styled");
    doc_undo(d);
    say("engine: undone");
    {   /* the toolbar's: a heading and an alignment, read back, undone */
        int bits, align, level;
        doc_click(d, 10, 5, DOC_DOWN, 0);
        doc_click(d, 10, 5, DOC_UP, 0);
        doc_format(d, &bits, &align, &level);
        sprintf(line, "engine: format bits %d align %d level %d", bits, align, level);
        say(line);
        doc_heading(d, 1);
        doc_align(d, DOC_CENTRE);
        doc_format(d, &bits, &align, &level);
        sprintf(line, "engine: after H1, centre: bits %d align %d level %d height %ld", bits, align, level,
                doc_height(d));
        say(line);
        if (!(bits & DOC_BOLD) || align != DOC_CENTRE || level != 1)
            say("engine: FAIL heading or alignment");
        doc_undo(d);
        doc_format(d, &bits, &align, &level);
        if (align != DOC_LEFT)
            say("engine: FAIL alignment not undone");
        doc_align(d, DOC_RIGHT);
        doc_align(d, DOC_FULL);
        doc_align(d, DOC_LEFT);
        doc_heading(d, 0);
        doc_format(d, &bits, &align, &level);
        sprintf(line, "engine: back to normal: bits %d align %d level %d", bits, align, level);
        say(line);
    }
    doc_select_all(d);
    doc_copy(d);
    doc_cut(d);
    doc_paste(d);
    doc_paste(d);
    say("engine: cut, pasted twice");
    doc_click(d, 10, 5, DOC_DOWN, 0);
    doc_click(d, 10, 5, DOC_UP, 0);
    if (doc_word(d, word, sizeof word, &b, &en)) {
        sprintf(line, "engine: word '%s' %ld..%ld", word, b, en);
        say(line);
    }
    e = doc_save(d, "HostFS::Host.$.Out", FILETYPE_WRITE);
    say(e ? e : "engine: saved");
    e = doc_save(d, "HostFS::Host.$.OutText", FILETYPE_TEXT);
    say(e ? e : "engine: exported");
    e = doc_save(d, "HostFS::Host.$.OutRTF", FILETYPE_RTF);
    say(e ? e : "engine: RTF exported");
    e = doc_save(d, "HostFS::Host.$.OutHTML", FILETYPE_HTML);
    say(e ? e : "engine: HTML exported");
    doc_free(d);
    d = doc_new();
    e = doc_load(d, "HostFS::Host.$.Out", FILETYPE_WRITE);
    say(e ? e : "engine: loaded");
    sprintf(line, "engine: loaded height %ld", doc_height(d));
    say(line);
    {
        /* text: two paragraphs, the second wrapping once, near the page's
         * width (a measure out by the font's would wrap it at half) */
        struct doc *t = doc_new();
        long starts[8];
        int n;
        e = doc_load(t, "HostFS::Host.$.Notes", FILETYPE_TEXT);
        say(e ? e : "engine: text loaded");
        n = doc_lines(t, starts, 8);
        sprintf(line, "engine: text lines %d, starting %ld %ld %ld %ld", n, starts[0],
                n > 1 ? starts[1] : -1L, n > 2 ? starts[2] : -1L, n > 3 ? starts[3] : -1L);
        say(line);
        if (n != 4 || starts[1] != 45 || starts[2] < 125)
            say("engine: FAIL the text's lines");
        doc_free(t);
    }
    doc_free(d);

    /* RTF: Write's own read back, and one written elsewhere */
    d = doc_new();
    e = doc_load(d, "HostFS::Host.$.OutRTF", FILETYPE_RTF);
    say(e ? e : "engine: RTF read back");
    sprintf(line, "engine: RTF read back, height %ld", doc_height(d));
    say(line);
    doc_free(d);
    d = doc_new();
    e = doc_load(d, "HostFS::Host.$.In", FILETYPE_RTF);
    say(e ? e : "engine: foreign RTF read");
    e = doc_save(d, "HostFS::Host.$.InText", FILETYPE_TEXT);
    say(e ? e : "engine: foreign RTF as text");
    doc_free(d);
    /* HTML: Write's own read back, and a page written elsewhere */
    d = doc_new();
    e = doc_load(d, "HostFS::Host.$.OutHTML", FILETYPE_HTML);
    say(e ? e : "engine: HTML read back");
    e = doc_save(d, "HostFS::Host.$.HTMLBackText", FILETYPE_TEXT);
    doc_free(d);
    d = doc_new();
    e = doc_load(d, "HostFS::Host.$.Page", FILETYPE_HTML);
    say(e ? e : "engine: a page read");
    e = doc_save(d, "HostFS::Host.$.PageText", FILETYPE_TEXT);
    e = doc_save(d, "HostFS::Host.$.PageRTF", FILETYPE_RTF);
    doc_free(d);

    /* macOS's RTF (textutil): braceless font table, \ for \par, \ulnone */
    d = doc_new();
    e = doc_load(d, "HostFS::Host.$.Cocoa", FILETYPE_RTF);
    say(e ? e : "engine: Cocoa's RTF read");
    e = doc_save(d, "HostFS::Host.$.CocoaText", FILETYPE_TEXT);
    doc_free(d);

    /* spelling, over the box disc's dictionary */
    if (spell_open("HostFS::Host.$.Resources.Dictionaries.spelldict")) {
        say("engine: FAIL no dictionary");
    } else {
        static const char *const right[] = { "Hello", "the", "don't", "Write's", "1984", "receive" };
        static const char *const wrong[] = { "teh", "recieve", "wrold", "speling" };
        char sugg[SPELL_MAX][SPELL_WORD];
        unsigned i;
        int n;
        for (i = 0; i < sizeof right / sizeof right[0]; i++)
            if (!spell_check(right[i])) {
                sprintf(line, "engine: FAIL '%s' called wrong", right[i]);
                say(line);
            }
        for (i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
            if (spell_check(wrong[i])) {
                sprintf(line, "engine: FAIL '%s' called right", wrong[i]);
                say(line);
            }
            n = spell_suggest(wrong[i], sugg, SPELL_MAX);
            sprintf(line, "engine: '%s' -> %d: %s %s %s", wrong[i], n, n > 0 ? sugg[0] : "",
                    n > 1 ? sugg[1] : "", n > 2 ? sugg[2] : "");
            say(line);
        }
        {
            /* the same answer whatever was asked before (#: a page misread) */
            static const char *const seq[] = { "Ths", "is", "a", "tset", "of", "the", "speling", "chekcer",
                                               "in", "Write", "Words", "liek", "these", "are", "underlined" };
            spell_close();
            spell_open("HostFS::Host.$.Resources.Dictionaries.spelldict");
            for (i = 0; i < sizeof seq / sizeof seq[0]; i++) {
                sprintf(line, "engine: seq '%s' %d", seq[i], spell_check(seq[i]));
                say(line);
            }
        }
        /* the user's words: one read, one learnt, one forgotten */
        spell_user_load("HostFS::Host.$.Words", "HostFS::Host.$.Words");
        if (!spell_check("ROSGD") || spell_check_cached("liek"))
            say("engine: FAIL the user's words read");
        if (spell_user_learn("liek") || !spell_check_cached("liek") || !spell_user_has("Liek"))
            say("engine: FAIL Learn");
        if (spell_user_forget("ROSGD") || spell_check("ROSGD"))
            say("engine: FAIL Forget");
        say("engine: user words learnt and forgotten");
        n = spell_suggest("Teh", sugg, SPELL_MAX);
        if (n < 1 || strcmp(sugg[0], "The"))
            say("engine: FAIL 'Teh' does not suggest 'The' first");
        /* the next misspelling in a document, and its replacement */
        d = doc_new();
        e = doc_load(d, "HostFS::Host.$.Notes", FILETYPE_TEXT);
        {
            char w[SPELL_WORD];
            long from = 0, b2, e2;
            int found = 0;
            while (doc_next_word(d, from, w, sizeof w, &b2, &e2)) {
                if (!spell_check(w)) {
                    sprintf(line, "engine: next misspelling '%s' at %ld..%ld", w, b2, e2);
                    say(line);
                    n = spell_suggest(w, sugg, SPELL_MAX);
                    if (n)
                        doc_replace(d, b2, e2, sugg[0]);
                    found = 1;
                    break;
                }
                from = e2;
            }
            if (!found)
                say("engine: FAIL no misspelling found");
            n = doc_word(d, w, sizeof w, &b2, &e2);
            sprintf(line, "engine: replaced; the word at the caret now '%s'", n ? w : "");
            say(line);
        }
        doc_free(d);
        spell_close();
    }
    doc_engine_final();
    say("engine: OK");
    return 0;
}
