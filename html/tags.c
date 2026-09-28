/*
 * tags.c - element and attribute tables (sorted by name for bsearch;
 * tests/test_html.c checks the order and the enum correspondence).
 */
#include <string.h>
#include "tags.h"

struct tagdef {
	const char *name;
	unsigned short flags;
};

#define B	TF_BLOCK
#define SP	TF_SPECIAL

/* in enum order, which is also name order */
static const struct tagdef tags[TAG_COUNT] = {
	{ "", 0 },
	{ "a", 0 }, { "abbr", 0 }, { "acronym", 0 },
	{ "address", B | SP }, { "applet", SP | TF_SCOPE }, { "area", TF_VOID | SP },
	{ "article", B | SP }, { "aside", B | SP }, { "audio", 0 },
	{ "b", 0 }, { "base", TF_VOID | TF_HEAD | SP }, { "basefont", TF_VOID | SP },
	{ "bdi", 0 }, { "bdo", 0 }, { "big", 0 }, { "blink", 0 },
	{ "blockquote", B | SP }, { "body", SP }, { "br", TF_VOID | SP },
	{ "button", SP }, { "canvas", 0 }, { "caption", SP | TF_SCOPE },
	{ "center", B | SP }, { "cite", 0 }, { "code", 0 },
	{ "col", TF_VOID | SP }, { "colgroup", SP | TF_ELEMONLY },
	{ "data", 0 }, { "datalist", TF_DROP }, { "dd", SP }, { "del", 0 },
	{ "details", B | SP }, { "dfn", 0 }, { "dialog", B | SP },
	{ "dir", B | SP | TF_ELEMONLY }, { "div", B | SP }, { "dl", B | SP | TF_ELEMONLY },
	{ "dt", SP }, { "em", 0 }, { "embed", TF_VOID | SP },
	{ "fieldset", B | SP }, { "figcaption", B | SP }, { "figure", B | SP },
	{ "font", 0 }, { "footer", B | SP }, { "form", B | SP },
	{ "frame", TF_VOID | SP }, { "frameset", SP | TF_ELEMONLY },
	{ "h1", B | SP | TF_HEADING }, { "h2", B | SP | TF_HEADING },
	{ "h3", B | SP | TF_HEADING }, { "h4", B | SP | TF_HEADING },
	{ "h5", B | SP | TF_HEADING }, { "h6", B | SP | TF_HEADING },
	{ "head", SP | TF_ELEMONLY }, { "header", B | SP }, { "hgroup", B | SP },
	{ "hr", TF_VOID | B | SP }, { "html", SP | TF_SCOPE | TF_ELEMONLY },
	{ "i", 0 }, { "iframe", TF_SKIPTEXT | SP }, { "image", TF_VOID },
	{ "img", TF_VOID | SP }, { "input", TF_VOID | SP }, { "ins", 0 },
	{ "kbd", 0 }, { "label", 0 }, { "legend", 0 }, { "li", SP },
	{ "link", TF_VOID | TF_HEAD | SP }, { "listing", B | SP | TF_PRE },
	{ "main", B | SP }, { "map", 0 }, { "mark", 0 },
	{ "marquee", SP | TF_SCOPE }, { "math", TF_DROP }, { "menu", B | SP | TF_ELEMONLY },
	{ "meta", TF_VOID | TF_HEAD | SP }, { "meter", 0 }, { "nav", B | SP },
	{ "nobr", 0 }, { "noembed", TF_SKIPTEXT | SP }, { "noframes", TF_SKIPTEXT | SP },
	/* no scripting here: <noscript> content is shown, as normal markup */
	{ "noscript", SP }, { "object", SP | TF_SCOPE }, { "ol", B | SP | TF_ELEMONLY },
	{ "optgroup", TF_ELEMONLY }, { "option", 0 }, { "output", 0 }, { "p", B | SP },
	{ "param", TF_VOID | SP }, { "picture", 0 }, { "plaintext", B | SP | TF_PRE },
	{ "pre", B | SP | TF_PRE }, { "progress", 0 }, { "q", 0 },
	{ "rb", 0 }, { "rp", 0 }, { "rt", 0 }, { "ruby", 0 }, { "s", 0 },
	{ "samp", 0 }, { "script", TF_SKIPTEXT | TF_HEAD | SP },
	{ "search", B | SP }, { "section", B | SP }, { "select", SP | TF_ELEMONLY },
	{ "small", 0 }, { "source", TF_VOID | SP }, { "span", 0 }, { "strike", 0 },
	{ "strong", 0 }, { "style", TF_SKIPTEXT | TF_HEAD | SP }, { "sub", 0 },
	{ "summary", B | SP }, { "sup", 0 }, { "svg", TF_DROP },
	{ "table", B | SP | TF_SCOPE | TF_ELEMONLY }, { "tbody", SP | TF_ELEMONLY },
	{ "td", SP | TF_SCOPE }, { "template", TF_DROP | SP | TF_SCOPE },
	{ "textarea", TF_RCDATA | SP | TF_PRE }, { "tfoot", SP | TF_ELEMONLY },
	{ "th", SP | TF_SCOPE }, { "thead", SP | TF_ELEMONLY }, { "time", 0 },
	{ "title", TF_RCDATA | TF_HEAD | SP }, { "tr", SP | TF_ELEMONLY },
	{ "track", TF_VOID | SP }, { "tt", 0 }, { "u", 0 },
	{ "ul", B | SP | TF_ELEMONLY }, { "var", 0 }, { "video", 0 },
	{ "wbr", TF_VOID | SP }, { "xmp", B | SP | TF_RAWTEXT | TF_PRE },
};

static const char *const attrs[ATTR_COUNT] = {
	"", "accept-charset", "action", "align", "alt", "aria-hidden",
	"bgcolor", "border", "cellpadding", "cellspacing", "charset", "checked",
	"class", "color", "cols", "colspan", "content", "datetime", "dir",
	"disabled", "enctype", "face", "for", "height", "hidden", "href",
	"http-equiv", "id", "label", "lang", "maxlength", "method", "multiple",
	"name", "nowrap", "placeholder", "readonly", "rel", "reversed", "rows",
	"rowspan", "selected", "size", "src", "start", "style", "target",
	"title", "type", "valign", "value", "width",
};

unsigned tag_flags(int tag)
{
	return tag > 0 && tag < TAG_COUNT ? tags[tag].flags : 0;
}

const char *tag_name(int tag)
{
	return tag > 0 && tag < TAG_COUNT ? tags[tag].name : "?";
}

/*
 * Lookups run for every tag and attribute of a page. On the TT, where a
 * library strcmp through a binary search cost ~55 us, the tables are
 * indexed by first letter and compared inline.
 */
static unsigned char tag_from[27], tag_to[27], attr_from[27], attr_to[27];

static void index_tables(void)
{
	static int done;
	int i;

	if (done)
		return;
	done = 1;
	for (i = TAG_COUNT - 1; i >= 1; i--) {
		int k = tags[i].name[0] - 'a';

		tag_from[k] = (unsigned char)i;
		if (!tag_to[k])
			tag_to[k] = (unsigned char)(i + 1);
	}
	for (i = ATTR_COUNT - 1; i >= 1; i--) {
		int k = attrs[i][0] - 'a';

		attr_from[k] = (unsigned char)i;
		if (!attr_to[k])
			attr_to[k] = (unsigned char)(i + 1);
	}
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int tag_lookup(const char *name)
{
	int k = name[0] - 'a', i;

	index_tables();
	if (k < 0 || k > 25)
		return TAG_UNKNOWN;
	for (i = tag_from[k]; i < tag_to[k]; i++)
		if (same(name, tags[i].name))
			return i;
	return TAG_UNKNOWN;
}

const char *attr_name(int attr)
{
	return attr > 0 && attr < ATTR_COUNT ? attrs[attr] : "?";
}

int attr_lookup(const char *name)
{
	int k = name[0] - 'a', i;

	index_tables();
	if (k < 0 || k > 25)
		return ATTR_NONE;
	for (i = attr_from[k]; i < attr_to[k]; i++)
		if (same(name, attrs[i]))
			return i;
	return ATTR_NONE;
}
