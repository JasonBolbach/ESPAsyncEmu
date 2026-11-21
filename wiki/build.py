#!/usr/bin/env python3
"""Compile the markdown wiki in this folder to static HTML in wiki/html/.

No third-party Python packages needed. Mermaid diagrams (```mermaid blocks)
are rendered in the browser by wiki/html/assets/mermaid.min.js, which this
script downloads once (pinned version) so the output also works offline.

Usage:  python3 wiki/build.py
"""
import html
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "html")
ASSETS = os.path.join(OUT, "assets")
MERMAID_VERSION = "11.4.1"
MERMAID_URL = f"https://cdn.jsdelivr.net/npm/mermaid@{MERMAID_VERSION}/dist/mermaid.min.js"

# Sidebar order. Any other *.md file is appended alphabetically.
ORDER = ["Home", "Architecture", "Event-Loop", "ESPAsyncTCP", "ESPAsyncUDP", "ESPAsyncWebServer", "Realtime",
         "Arduino-Compat", "Examples", "Integrations", "Testing", "Roadmap"]


def slug(text):
    s = re.sub(r"[`*_]", "", text).strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return re.sub(r"\s+", "-", s)


def inline(text):
    """Inline markdown -> HTML (code spans, bold, italics, links)."""
    codes = []

    def stash(m):
        codes.append("<code>%s</code>" % html.escape(m.group(1)))
        return "\x00%d\x00" % (len(codes) - 1)

    text = re.sub(r"`([^`]+)`", stash, text)
    text = html.escape(text, quote=False)

    def link(m):
        label, url = m.group(1), m.group(2)
        url = re.sub(r"\.md(#|$)", r".html\1", url) if not re.match(r"^\w+://", url) else url
        return '<a href="%s">%s</a>' % (html.escape(url, quote=True), label)

    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"<em>\1</em>", text)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], text)


def split_row(line):
    """Split a table row on '|' that are not inside backticks."""
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    cells, cur, in_code = [], "", False
    for ch in line:
        if ch == "`":
            in_code = not in_code
        if ch == "|" and not in_code:
            cells.append(cur.strip())
            cur = ""
        else:
            cur += ch
    cells.append(cur.strip())
    return cells


def convert(md):
    lines = md.split("\n")
    out, toc = [], []
    i, title = 0, None
    para = []

    def flush_para():
        if para:
            out.append("<p>%s</p>" % inline(" ".join(para)))
            para.clear()

    while i < len(lines):
        line = lines[i]

        m = re.match(r"^```(\w*)\s*$", line)
        if m:
            flush_para()
            lang, body = m.group(1), []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i])
                i += 1
            i += 1
            code = "\n".join(body)
            if lang == "mermaid":
                out.append('<pre class="mermaid">%s</pre>' % html.escape(code, quote=False))
            else:
                out.append('<pre><code class="lang-%s">%s</code></pre>' % (lang, html.escape(code, quote=False)))
            continue

        m = re.match(r"^(#{1,4})\s+(.*)$", line)
        if m:
            flush_para()
            level, text = len(m.group(1)), m.group(2).strip()
            if level == 1 and title is None:
                title = re.sub(r"[`*]", "", text)
            sid = slug(text)
            if level in (2, 3):
                toc.append((level, text, sid))
            out.append('<h%d id="%s">%s</h%d>' % (level, sid, inline(text), level))
            i += 1
            continue

        if line.lstrip().startswith("|") and i + 1 < len(lines) and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            flush_para()
            head = split_row(line)
            i += 2
            rows = []
            while i < len(lines) and lines[i].lstrip().startswith("|"):
                rows.append(split_row(lines[i]))
                i += 1
            t = ["<table><thead><tr>%s</tr></thead><tbody>" % "".join("<th>%s</th>" % inline(c) for c in head)]
            for r in rows:
                t.append("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c) for c in r))
            t.append("</tbody></table>")
            out.append("".join(t))
            continue

        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", line)
        if m:
            flush_para()
            ordered = m.group(2)[0].isdigit()
            tag = "ol" if ordered else "ul"
            items = []
            while i < len(lines):
                mm = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", lines[i])
                if mm:
                    items.append(mm.group(3))
                    i += 1
                elif lines[i].startswith("  ") and lines[i].strip() and items:
                    items[-1] += " " + lines[i].strip()  # wrapped continuation
                    i += 1
                else:
                    break
            out.append("<%s>%s</%s>" % (tag, "".join("<li>%s</li>" % inline(x) for x in items), tag))
            continue

        if not line.strip():
            flush_para()
        else:
            para.append(line.strip())
        i += 1

    flush_para()
    return title, "\n".join(out), toc


CSS = """
:root { --bg:#ffffff; --fg:#1f2328; --muted:#59636e; --border:#d1d9e0; --side:#f6f8fa; --accent:#0969da; --code:#eff1f3; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#0d1117; --fg:#e6edf3; --muted:#9198a1; --border:#3d444d; --side:#151b23; --accent:#4493f8; --code:#212830; }
}
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg); font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif; display:flex; min-height:100vh; }
nav { width:240px; flex:none; background:var(--side); border-right:1px solid var(--border); padding:20px 16px; position:sticky; top:0; height:100vh; overflow:auto; }
nav .brand { font-weight:700; font-size:18px; margin-bottom:12px; }
nav a { display:block; padding:5px 10px; border-radius:6px; color:var(--fg); text-decoration:none; }
nav a:hover { background:var(--code); }
nav a.current { background:var(--accent); color:#fff; }
nav .toc { margin-top:16px; padding-top:12px; border-top:1px solid var(--border); font-size:14px; }
nav .toc a { color:var(--muted); padding:2px 10px; }
nav .toc a.l3 { padding-left:24px; }
main { flex:1; min-width:0; max-width:900px; padding:32px 40px 80px; }
h1,h2,h3 { line-height:1.25; }
h1 { border-bottom:1px solid var(--border); padding-bottom:.3em; }
h2 { border-bottom:1px solid var(--border); padding-bottom:.3em; margin-top:1.8em; }
a { color:var(--accent); }
code { background:var(--code); padding:.15em .35em; border-radius:4px; font:85% ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }
pre { background:var(--code); padding:14px 16px; border-radius:8px; overflow:auto; }
pre code { background:none; padding:0; font-size:13px; }
pre.mermaid { background:transparent; text-align:center; padding:8px 0; }
table { border-collapse:collapse; display:block; overflow:auto; margin:1em 0; }
th,td { border:1px solid var(--border); padding:6px 12px; text-align:left; vertical-align:top; }
th { background:var(--side); }
footer { margin-top:48px; color:var(--muted); font-size:13px; }
@media (max-width: 800px) { body { flex-direction:column; } nav { position:static; width:auto; height:auto; } main { padding:20px; } }
"""

PAGE = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} - ESPAsyncEmu</title>
<style>{css}</style>
</head><body>
<nav>
<div class="brand">ESPAsyncEmu</div>
{links}
{toc}
</nav>
<main>
{body}
<footer>Generated from <code>wiki/{src}</code> by <code>wiki/build.py</code>.</footer>
</main>
<script src="assets/mermaid.min.js"></script>
<script>
  var dark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
  mermaid.initialize({{ startOnLoad: true, theme: dark ? 'dark' : 'default', securityLevel: 'strict' }});
</script>
</body></html>
"""


def ensure_mermaid():
    path = os.path.join(ASSETS, "mermaid.min.js")
    if os.path.exists(path) and os.path.getsize(path) > 100_000:
        return
    os.makedirs(ASSETS, exist_ok=True)
    print("downloading mermaid %s ..." % MERMAID_VERSION)
    try:
        with urllib.request.urlopen(MERMAID_URL, timeout=60) as r, open(path, "wb") as f:
            f.write(r.read())
    except Exception as e:  # fall back to the CDN so the pages still render online
        print("warning: could not download mermaid (%s); pages will load it from the CDN" % e, file=sys.stderr)
        if os.path.exists(path):
            os.remove(path)


def main():
    names = [f[:-3] for f in os.listdir(HERE) if f.endswith(".md")]
    pages = [n for n in ORDER if n in names] + sorted(n for n in names if n not in ORDER)
    os.makedirs(OUT, exist_ok=True)
    ensure_mermaid()
    have_local_mermaid = os.path.exists(os.path.join(ASSETS, "mermaid.min.js"))

    parsed = {}
    for n in pages:
        with open(os.path.join(HERE, n + ".md"), encoding="utf-8") as f:
            parsed[n] = convert(f.read())

    for n in pages:
        title, body, toc = parsed[n]
        links = "\n".join(
            '<a href="%s.html"%s>%s</a>' % (p, ' class="current"' if p == n else "", html.escape(parsed[p][0] or p))
            for p in pages
        )
        toc_html = ""
        if toc:
            toc_html = '<div class="toc">%s</div>' % "".join(
                '<a class="l%d" href="#%s">%s</a>' % (lvl, sid, inline(text)) for lvl, text, sid in toc
            )
        page = PAGE.format(title=html.escape(title or n), css=CSS, links=links, toc=toc_html, body=body, src=n + ".md")
        if not have_local_mermaid:
            page = page.replace("assets/mermaid.min.js", MERMAID_URL)
        with open(os.path.join(OUT, n + ".html"), "w", encoding="utf-8") as f:
            f.write(page)

    # index.html -> Home
    with open(os.path.join(OUT, "index.html"), "w", encoding="utf-8") as f:
        f.write('<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0; url=Home.html">'
                '<a href="Home.html">Home</a>')
    print("wrote %d pages to %s" % (len(pages), OUT))


if __name__ == "__main__":
    main()
