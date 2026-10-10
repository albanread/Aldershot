#!/usr/bin/env python3
"""mkarticles.py -- the BOX articles, Markdown to HTML for NetSurf in the box.

    tools/mkarticles.py ARTICLES_DIR OUT_DIR

ARTICLES_DIR is the articles directory, rosgd/docs/articles (until
6 Oct 2026 they were the Aldershot repository's articles/): README.md, the index,
and one .md file per article.  OUT_DIR (disc/Docs/Articles) gets one
".html" file per article and "index.html", the index -- plain HTML 4 with
a little CSS, which NetSurf draws.  In the box they are "name/html", typed
&FAF by HostFS's extension map; NetSurf, a POSIX program over the Unix
bridge, reads them by their host names, so links between articles are
"name.html".  An article's title in italics, as the articles refer to
each other ("see *BOX Architecture*"), becomes a link to it.

Only the Markdown the articles use is understood: headings, paragraphs,
"---" rules, bullet and numbered lists (one level, with continuation
lines), tables, fenced code, and inline code, bold, italics and links.
"""
import html
import os
import re
import sys

CSS = """
body { font-family: sans-serif; margin: 1em 2em; max-width: 46em; color: #222; background: #fff; }
h1 { font-size: 160%; border-bottom: 2px solid #36c; padding-bottom: 0.2em; }
h2 { font-size: 130%; margin-top: 1.4em; color: #135; }
h3 { font-size: 110%; color: #135; }
p.date { color: #666; font-style: italic; }
code { font-family: monospace; background: #eef; }
pre { font-family: monospace; background: #eef; padding: 0.5em; border: 1px solid #ccd; }
table { border-collapse: collapse; margin: 0.5em 0; }
th, td { border: 1px solid #aab; padding: 0.2em 0.5em; vertical-align: top; text-align: left; }
th { background: #dde; }
hr { border: none; border-top: 1px solid #ccd; margin: 1.5em 0; }
p.nav { font-size: 90%; }
"""


def inline(text, titles, here):
    """One line or cell's inline Markdown: code, links, bold, italics"""
    parts = re.split(r"(`[^`]*`)", text)
    out = []
    for part in parts:
        if part.startswith("`") and part.endswith("`") and len(part) >= 2:
            out.append("<code>%s</code>" % html.escape(part[1:-1], quote=False))
            continue
        s = html.escape(part, quote=False)

        def link(m):
            label, href = m.group(1), m.group(2)
            if href.endswith(".md") and "/" not in href:
                href = href[:-3] + ".html"
            return '<a href="%s">%s</a>' % (html.escape(href), label)
        s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, s)
        s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)

        def em(m):
            body = m.group(1)
            name = titles.get(body)
            if name and name != here:
                return '<em><a href="%s.html">%s</a></em>' % (name, body)
            return "<em>%s</em>" % body
        s = re.sub(r"(?<![\w*])\*(?!\s)([^*]+?)(?<!\s)\*(?![\w*])", em, s)
        out.append(s)
    return "".join(out)


def convert(md, titles, here):
    """An article's Markdown: (its title, its HTML body)"""
    lines = md.split("\n")
    out, title, i = [], None, 0
    para = []

    def flush():
        if para:
            out.append("<p>%s</p>" % inline(" ".join(para), titles, here))
            para.clear()

    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if s.startswith("```"):
            flush()
            i += 1
            code = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            out.append("<pre>%s</pre>" % html.escape("\n".join(code), quote=False))
            i += 1
            continue
        m = re.match(r"^(#{1,6})\s+(.*)$", s)
        if m:
            flush()
            n = len(m.group(1))
            if n == 1 and title is None:
                title = m.group(2)
            out.append("<h%d>%s</h%d>" % (n, inline(m.group(2), titles, here), n))
            i += 1
            continue
        if re.match(r"^-{3,}$", s):
            flush()
            out.append("<hr>")
            i += 1
            continue
        if s.startswith("|"):
            flush()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(lines[i].strip())
                i += 1
            # a cell's "\|" is a "|" (GitHub's escape for one in a table)
            cells = [[c.strip().replace("\\|", "|") for c in r.strip("|").split(" | ")] for r in rows]
            body = [c for c in cells if not all(re.match(r"^:?-+:?$", x) for x in c)]
            t = ["<table>"]
            for n, row in enumerate(body):
                tag = "th" if n == 0 else "td"
                t.append("<tr>" + "".join("<%s>%s</%s>" % (tag, inline(c, titles, here), tag)
                                          for c in row) + "</tr>")
            t.append("</table>")
            out.append("\n".join(t))
            continue
        m = re.match(r"^(\*|-|\d+\.)\s+(.*)$", s)
        if m and not line.startswith("  "):
            flush()
            ordered = m.group(1)[0].isdigit()
            items = []
            while i < len(lines):
                l = lines[i]
                mm = re.match(r"^(\*|-|\d+\.)\s+(.*)$", l.strip())
                if mm and not l.startswith("  "):
                    items.append([mm.group(2)])
                elif l.startswith("  ") and l.strip() and items:
                    items[-1].append(l.strip())
                else:
                    break
                i += 1
            tag = "ol" if ordered else "ul"
            out.append("<%s>%s</%s>" % (tag, "".join(
                "<li>%s</li>" % inline(" ".join(it), titles, here) for it in items), tag))
            continue
        if not s:
            flush()
        else:
            para.append(s)
        i += 1
    flush()
    return title, "\n".join(out)


REDACTED = "[*redacted*]"


def load_redactions(path):
    """The names to redact (tools/articles-redact.txt): (name regexes, link slugs)."""
    names, slugs = [], []
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("url:"):
                slugs.append(line[4:].strip().lower())
            else:
                names.append(re.compile(r"\b" + r"\s+".join(re.escape(w) for w in line.split()) + r"\b"))
    return names, slugs


def redact(text, rules):
    """text with each named person, and each link to their site, redacted."""
    names, slugs = rules
    for slug in slugs:
        text = re.sub(r'<a\s[^>]*href="[^"]*%s[^"]*"[^>]*>.*?</a>' % re.escape(slug), REDACTED, text,
                      flags=re.S | re.I)
        text = re.sub(r"\S*%s\S*" % re.escape(slug), REDACTED, text, flags=re.I)
    for rx in names:
        text = rx.sub(REDACTED, text)
    return text


def page(title, body, nav):
    return ("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\">\n"
            "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n"
            "<title>%s</title>\n<style type=\"text/css\">%s</style></head>\n<body>\n%s\n%s\n%s\n</body></html>\n"
            % (html.escape(title), CSS, nav, body, nav))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    rules = load_redactions(os.path.join(os.path.dirname(os.path.abspath(__file__)), "articles-redact.txt"))
    names = sorted(f[:-3] for f in os.listdir(src) if f.endswith(".md") and f != "README.md")
    texts = {n: open(os.path.join(src, n + ".md"), encoding="utf-8").read() for n in names}
    titles = {}
    for n, t in texts.items():
        m = re.search(r"^#\s+(.*)$", t, re.M)
        if m:
            titles[m.group(1).strip()] = n
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(dst):
        if f.endswith(".html"):
            os.remove(os.path.join(dst, f))
    nav = '<p class="nav"><a href="index.html">All articles</a></p>'
    for n in names:
        title, body = convert(texts[n], titles, n)
        with open(os.path.join(dst, n + ".html"), "w", encoding="utf-8") as fh:
            fh.write(redact(page(title or n, body, nav), rules))
    # The index: a welcome, then the articles as README.md's table lists
    # them (newest first), each its title linked, what it covers, and when
    readme = open(os.path.join(src, "README.md"), encoding="utf-8").read()
    items = []
    for row in readme.split("\n"):
        cells = [c.strip() for c in row.strip().strip("|").split(" | ")]
        if len(cells) != 4 or not cells[1].startswith("**"):
            continue
        m = re.search(r"\(([^)]+)\.md\)", cells[3])
        if not m or m.group(1) not in texts:
            continue
        items.append('<li><a href="%s.html"><strong>%s</strong></a><br>%s <em>(%s)</em></li>'
                     % (m.group(1), html.escape(cells[1].strip("*")),
                        inline(cells[2], titles, "index"), html.escape(cells[0])))
    body = ("<h1>Welcome to RISC OS 5.30 on BOX</h1>\n"
            "<p>BOX is RISC OS translated to C and running on a Linux kernel. "
            "These articles describe how it works. Each can be read on its own; "
            "the newest is first.</p>\n"
            "<h2>Articles</h2>\n<ul>\n%s\n</ul>" % "\n".join(items))
    with open(os.path.join(dst, "index.html"), "w", encoding="utf-8") as fh:
        fh.write(redact(page("Welcome to RISC OS 5.30 on BOX", body, ""), rules))
    print("mkarticles: %d articles and the index in %s" % (len(names), dst))


if __name__ == "__main__":
    main()
