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

## Left for 7

- Tables as grids.
