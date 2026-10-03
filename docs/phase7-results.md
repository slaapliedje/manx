# Phase 7 results: pages that look right (in progress)

## What style sheets hide

Pages hide menus, dialogs, drop-downs, editors' notes and the like with
CSS. Manx followed only `hidden` and an inline `style="display:none"`,
so the rest showed as text. In the corpus (54 pages): 31 have `<style>`
(1.4 MB of CSS in all, mostly generated utility classes), 38 link 178
style sheets, and 235 rules hide something. Of their selectors, 98 use
pseudo-classes, 63 descendants, 55 a plain class, id or tag, and 33
attributes; `@media` conditions are nearly all widths, in px and rem.

| Module | What it does |
|---|---|
| `style/css` | reads style sheets as they stream in, a run of ordinary characters at a time, comments, strings and escapes followed across pieces. A rule whose declarations don't mention display or visibility is dropped after a quick scan; the rest are compiled: each selector a few compound selectors (tag, `*`, `#id`, `.class`, `[attr]`, `[attr=v]`, `[attr~=v]` for the attributes Manx keeps), descendant and child combinators, the subject first, names hashed. A selector with anything else (pseudo-classes, sibling combinators, unknown or custom tags, attributes Manx doesn't keep) is dropped: in doubt, content shows. Rules are filed by their subject's id, else a class, else the tag. `@media` keeps width ranges (px, em, rem at 16 px; screen, not print; a few other features), checked against the window at each layout; `@supports` and `@layer` are read through; `@font-face`, `@keyframes` and nested rules are skipped. The cascade is followed for display and for visibility: `!important`, specificity, order. Each element's tag, id and classes are hashed once and kept on a stack of the current element's ancestors (the layout goes through the tree in order), and the answer per element is kept until the rules or the window's width change |
| `html` | the tokenizer hands `<style>` text to the tree builder, which reads it into the document's sheet (`<style media=print>` is skipped). `stylesheets = off` in the config leaves sheets unread |
| `layout` | an element the sheet hides is laid out as `display:none`, unless its own `style=""` sets another display. Never `<html>` or `<body>`: some pages hide themselves until their script runs. The window's width: pixels in xmanx, a terminal's columns at 8 pixels (80 columns: 640 px, the narrow layout pages make for phones) |

Not hidden, deliberately: text "visually hidden" for screen readers
(absolutely placed and clipped to nothing). It is there because sighted
readers get the same from an icon or a picture, which Manx doesn't
draw: "Posted 9 minutes ago", "Search", a logo's name.

### Tests

| Test | Cases |
|---|---|
| `test_css` | 41 pages, each also loaded a byte at a time: every selector form, selector lists, the cascade (order, specificity, `!important`, an inline style), display and visibility apart, what's not followed (pseudo-classes, siblings, custom tags, unknown attributes, nested rules), escapes, comments, strings with braces, `@media` widths in px and rem, `print`, `not print`, unknown features, nested `@media`, `@supports`/`@layer`, skipped blocks, `@import`, the body never hidden, a rule left open, `<style media=print>`, `<style>` in the body, many identical `@media` blocks |
| corpus | with `<style>` only: 6 pages change. Wikipedia hides its editors' "CS1 maint" notices; Google its apps menu's label; Slashdot a form's hidden widgets; BBC its menu drawer, drop-downs and loading placeholders; the Guardian its "Hide" buttons, edition menu, ad slots, and the standfirsts it leaves out at narrow widths |
| `fuzz_html` | CSS fragments added to what it splices into pages; 60,000 iterations clean |

### Speed on the TT

`uparse -s -w 80`, before and after:

| Page | CSS inline | before | after |
|---|---|---|---|
| Wikipedia article | 10 KB | 2.3 s | 2.8 s |
| BBC News | 179 KB | 8.6 s | 10.2 s |
| the Guardian | 634 KB | 13.7 s | 18.1 s |

The first version, a character at a time through the reader and
matching on attributes read and hashed again for each rule, cost twice
as much (the Guardian 28.4 s). Reading now runs at about 3 us a byte of
CSS; matching costs about a second on the first layout of a big page and
next to nothing after (the answer per element is kept).

## Linked style sheets

Most pages keep their hiding rules in linked sheets (38 of the 54 corpus
pages link 178 of them).

| Module | What it does |
|---|---|
| `src/pagecss` | when a page is complete, its `<link rel=stylesheet>` (not alternate ones, not print-only), up to 16. While no key waits, each comes from the disk cache if fresh, else is fetched (with the page as Referer), kept whole up to 384 KB (one cut short by a key is fetched again later, not read in part), put in the cache, and read into the page's sheet. Sites share their sheets between pages: from the cache a sheet costs only its reading |
| `style/css` | `css_begin` takes the sheet's place in the page (its element's node, which grows in document order): rule order is that times 65536 plus the rule's number, so a sheet linked early ranks below a later `<style>` whatever order they arrive in |
| `src/manx.c` | sheets before images (they say what is laid out, images included); the page laid out again as they come, keeping the text at the top of the window (or going back to the page's `#fragment`), at most every 2 s. `z` stops both; the status line counts them; `stylesheets = off` skips them too |

Tested with the terminal manx on local pages: a linked sheet hides, a
print-only and an alternate sheet don't, and with equal specificity a
`<style>` before the `<link>` loses to the sheet while one after it wins.
Over HTTP (a local server sending `Cache-Control: max-age=3600`) the
second visit took the sheet from the disk cache, with no request for it.
On Wikipedia the first screen was the collapsed main menu's contents
(Navigation, Contribute and their lists) and the search box's parts;
with the sheets the article starts on the first screen. On the TT (a
resumed session) the page showed within 20 s and its sheets were read
and applied within 40 s.

On the way: the 1997 SVR4 assembler dies on one of `manx.c`'s switch
jump tables ("Can't extend frag 100. chars"), though the file's nine
others like it assemble. `manx.c` is now compiled with
`-fno-jump-tables`: its switches are keys, so compare chains cost
nothing that matters.

## Tables as grids

Tables were laid out a row a line, cells side by side and wrapping as
one text. Now a table of data is a grid, its columns side by side; one
that frames a page stays as rows, which read better on a narrow screen.

| | |
|---|---|
| which | at least two columns; no table inside it; no cell with more than 800 bytes of text or 20 lines; the columns' narrowest widths fit. Else rows, as before. A grid can sit in a cell of a table laid out as rows (Hacker News' story list in its page's frame) |
| cells | each laid out on its own, as a block, by the same engine (the layout's walk now takes any subtree), into a small page of its own; a cell's own `id` is an anchor. Measured at the table's width: its widest line, and its widest word, field or picture. `colspan` and `rowspan` |
| columns | each at its widest if all fit; else its narrowest plus a share of what is left, as much as it would take more (the usual automatic table layout). A cell narrower than it was measured is laid out again at its column's width |
| rows | put together a line at a time, each cell's line at its column: links, form fields, pictures and anchors take their numbers in the page; captions above. Between columns 2 spaces on a terminal; with proportional fonts a spacer of the exact width (4 bytes in a picture's run, like an image's, but drawn as nothing), so the columns line up to the pixel |

In the corpus 15 pages change: Wikipedia's infoboxes (labels and values
in columns), Hacker News (rank, vote, title, and the points line under
the title), Google's search box and buttons, IANA's site map, Berkshire
Hathaway's two columns of links, NetBSD's, GitHub's tables; the
Space Jam page of 1996 (an image map in a table) becomes a sparse grid
of its links, as its pictures were laid out.

Tests: `test_layout` gains 11 cases (a grid, wrapping in a column, spans
both ways, a caption, links and anchors in cells where they show, one
column, a table holding a table, a cell framing a page, too wide, and
with a made-up proportional font every line exactly as wide as the
columns say). `fuzz_html` splices in `colspan`/`rowspan`/`<th>`/
`<caption>`; it found two bugs on the way (a row without cells read
past the end of the placement array; cells not freed when a table
turned out not to be a grid), and with them fixed ran 60,000 iterations
clean. Scrolling Hacker News in xmanx, painted incrementally, matches a
full repaint at every step.

On the TT (`uparse -s`, layout time and the heap's peak):

| page | before | as grids |
|---|---|---|
| Hacker News | 0.51 s, 79 KB | 0.88 s, 221 KB |
| Wikipedia, Motorola 68030 | 1.13 s, 178 KB | 1.20 s, 197 KB |
| Wikipedia, Atari TT030 | 1.50 s, 305 KB | 1.60 s, 319 KB |

The first version took 1.84 s and 1.4 MB for Hacker News: each cell's
page started with a whole page's arrays, and each cell's layout cleared
an 8 KB state (AMIX's memset is slow). Cell pages now start small, and
one state serves a table's cells, its saved-element stack left as it
is (written before it's read).

## list-style

`list-style: none` and `list-style-type: none` (from sheets or an element's
own `style=""`) drop list markers. It is inherited, as in CSS: set on a
`<ul>` it holds for its items and lists inside them, until a rule gives
them a marker again (`list-style: square`). An ordered list without
numbers still counts. The cascade is the same as display's. Nine corpus
pages lose bullets this way (Wikipedia's infobox values, portal and
category boxes; BBC's skip links). `test_css`: 6 more cases.

## Images in the disk cache

Pictures were fetched again on every visit, Back included. Now an
image's file (up to 256 KB: the cache, 2 MB by default, is pages' too)
goes into the disk cache with the freshness its response gave it. On
the next visit a fresh copy is read from disk with no request; a stale
one is checked with the server (`If-None-Match`, else
`If-Modified-Since`), and a 304 means the cached copy, renewed.
Missing images (404) aren't kept.

Tested with xmanx against a local server, the image page visited twice
in two runs: with `Cache-Control: max-age=3600` on the images the second
visit asked for none of them (only the missing one again); with no
cache headers it asked for all five conditionally, the server answered
304 each time, and they were shown from the cache.

## Left for 7

- Progressive JPEG (from Phase 6).
