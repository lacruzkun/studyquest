#include "import/anki_html.h"
#include "core/utf8.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

static bool ref_exists(char refs[][512], int count, const char *value) {
    for (int i = 0; i < count; i++)
        if (strcmp(refs[i], value) == 0) return true;
    return false;
}

static void add_ref(char refs[][512], int *count, int cap, const char *value) {
    if (!value || !*value || !count || *count >= cap) return;
    if (ref_exists(refs, *count, value)) return;
    snprintf(refs[*count], 512, "%s", value);
    (*count)++;
}

/* Decode the entities Anki content commonly contains. Numeric entities are
   decoded to UTF-8 so Japanese content encoded as &#x4E00; remains Unicode. */
static bool decode_entity(const char *p, char *out, size_t cap, size_t *consumed) {
    if (!p || p[0] != '&' || !out || cap < 2) return false;

    struct Named { const char *name; const char *utf8; };
    static const struct Named named[] = {
        { "amp;",   "&" }, { "lt;",    "<" }, { "gt;",    ">" },
        { "quot;",  "\"" }, { "apos;", "'" }, { "#39;", "'" },
        { "nbsp;",  " " }, { "mdash;", "\xE2\x80\x94" },
        { "ndash;", "\xE2\x80\x93" }, { "hellip;", "\xE2\x80\xA6" }
    };

    for (size_t i = 0; i < sizeof(named)/sizeof(named[0]); i++) {
        size_t n = strlen(named[i].name);
        if (strncmp(p + 1, named[i].name, n) == 0) {
            size_t len = strlen(named[i].utf8);
            if (len >= cap) return false;
            memcpy(out, named[i].utf8, len);
            out[len] = 0;
            if (consumed) *consumed = n + 1;
            return true;
        }
    }

    if (p[1] != '#') return false;
    const char *q = p + 2;
    int base = 10;
    if (*q == 'x' || *q == 'X') { base = 16; q++; }
    if (!isxdigit((unsigned char)*q)) return false;

    uint32_t cp = 0;
    const char *digits = q;
    while (isxdigit((unsigned char)*q)) {
        unsigned int digit;
        if (*q >= '0' && *q <= '9') digit = (unsigned int)(*q - '0');
        else if (*q >= 'a' && *q <= 'f') digit = (unsigned int)(*q - 'a' + 10);
        else digit = (unsigned int)(*q - 'A' + 10);
        if (digit >= (unsigned int)base) break;
        if (cp > 0x10FFFFu / (unsigned)base) return false;
        cp = cp * (unsigned)base + digit;
        if (cp > 0x10FFFFu) return false;
        q++;
    }
    if (q == digits || *q != ';' || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;

    char encoded[5];
    size_t len = 0;
    if (cp <= 0x7F) encoded[len++] = (char)cp;
    else if (cp <= 0x7FF) {
        encoded[len++] = (char)(0xC0 | (cp >> 6));
        encoded[len++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        encoded[len++] = (char)(0xE0 | (cp >> 12));
        encoded[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        encoded[len++] = (char)(0x80 | (cp & 0x3F));
    } else {
        encoded[len++] = (char)(0xF0 | (cp >> 18));
        encoded[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        encoded[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        encoded[len++] = (char)(0x80 | (cp & 0x3F));
    }
    if (len >= cap) return false;
    memcpy(out, encoded, len);
    out[len] = 0;
    if (consumed) *consumed = (size_t)(q - p + 1);
    return true;
}

/* Extract src from the opening portion of an <img ...> tag. */
static bool extract_img_src(const char *start, const char *close,
                            char *out, size_t cap) {
    const char *p = start + 4;
    while (p < close) {
        while (p < close && isspace((unsigned char)*p)) p++;
        if (p >= close || *p == '>') break;

        const char *name = p;
        while (p < close && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
        size_t nl = (size_t)(p - name);
        if (nl == 0) { p++; continue; }

        while (p < close && isspace((unsigned char)*p)) p++;
        if (p >= close || *p != '=') {
            while (p < close && !isspace((unsigned char)*p)) p++;
            continue;
        }
        p++;
        while (p < close && isspace((unsigned char)*p)) p++;

        char quote = 0;
        if (p < close && (*p == '\'' || *p == '"')) { quote = *p; p++; }
        const char *value_start = p;
        if (quote) {
            while (p < close && *p != quote) p++;
        } else {
            while (p < close && !isspace((unsigned char)*p) && *p != '>') p++;
        }
        size_t raw_len = (size_t)(p - value_start);

        if (nl == 3 && strncasecmp(name, "src", 3) == 0 && raw_len > 0) {
            size_t w = 0;
            const char *q = value_start;
            const char *end = value_start + raw_len;
            while (q < end && w + 1 < cap) {
                if (*q == '&') {
                    char entity[8] = {0};
                    size_t consumed = 0;
                    if (decode_entity(q, entity, sizeof(entity), &consumed) && q + consumed <= end) {
                        size_t elen = strlen(entity);
                        if (w + elen >= cap) break;
                        memcpy(out + w, entity, elen);
                        w += elen;
                        q += consumed;
                        continue;
                    }
                }
                out[w++] = *q++;
            }
            out[w] = 0;
            return w > 0;
        }
    }
    return false;
}

void anki_html_process(const char *in, char *out, size_t cap,
                       char media_refs[][512], int *media_count,
                       int media_cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    int local_media_count = 0;
    if (!media_count) media_count = &local_media_count;
    if (!in) return;

    size_t w = 0;
    const char *p = in;

    #define EMIT(ch) do { if (w + 1 < cap) out[w++] = (ch); } while (0)
    #define EMIT_STR(str) do { \
        const char *_q = (str); \
        while (*_q && w + 1 < cap) out[w++] = *_q++; \
    } while (0)
    #define EMIT_NL() do { if (w > 0 && out[w-1] != '\n') EMIT('\n'); } while (0)

    while (*p) {
        if (p[0] == '[' && strncasecmp(p, "[sound:", 7) == 0) {
            const char *close = strchr(p + 7, ']');
            if (close) {
                char fname[512];
                size_t len = (size_t)(close - (p + 7));
                if (len >= sizeof(fname)) len = sizeof(fname) - 1;
                memcpy(fname, p + 7, len);
                fname[len] = 0;
                add_ref(media_refs, media_count, media_cap, fname);
                p = close + 1;
                continue;
            }
        }

        if (p[0] == '<' && strncasecmp(p, "<img", 4) == 0) {
            const char *close = strchr(p, '>');
            if (close) {
                char src[512] = {0};
                if (extract_img_src(p, close, src, sizeof(src)))
                    add_ref(media_refs, media_count, media_cap, src);
                p = close + 1;
                continue;
            }
        }

        /* Do not leak comments, style/script payload, or other markup text. */
        if (p[0] == '<') {
            if (strncmp(p, "<!--", 4) == 0) {
                const char *end = strstr(p + 4, "-->");
                if (end) { p = end + 3; continue; }
            }
            const char *close = strchr(p, '>');
            if (close) {
                const char *name = p + 1;
                if (*name == '/') name++;   /* skip closing slash */
                while (*name && isspace((unsigned char)*name)) name++;
                char tname[16];
                size_t nlen = 0;
                while (name[nlen] && !isspace((unsigned char)name[nlen]) &&
                       name[nlen] != '>' && name[nlen] != '/' &&
                       nlen < sizeof(tname) - 1) {
                    tname[nlen] = (char)tolower((unsigned char)name[nlen]);
                    nlen++;
                }
                tname[nlen] = 0;

                /* Block-level tags become line breaks so paragraphs, list
                   items, and table rows read as separate lines. Both the
                   opening and closing forms break (EMIT_NL suppresses
                   consecutive newlines). */
                bool is_block =
                    (strcmp(tname, "br") == 0) ||
                    (strcmp(tname, "hr") == 0) ||
                    (strcmp(tname, "p") == 0 || strcmp(tname, "div") == 0 ||
                     strcmp(tname, "li") == 0 || strcmp(tname, "tr") == 0 ||
                     strcmp(tname, "ul") == 0 || strcmp(tname, "ol") == 0 ||
                     strcmp(tname, "table") == 0 ||
                     strcmp(tname, "blockquote") == 0 ||
                     strcmp(tname, "h1") == 0 || strcmp(tname, "h2") == 0 ||
                     strcmp(tname, "h3") == 0 || strcmp(tname, "h4") == 0 ||
                     strcmp(tname, "h5") == 0 || strcmp(tname, "h6") == 0);
                if (is_block) EMIT_NL();
                p = close + 1;
                continue;
            }
        }

        if (p[0] == '&') {
            char entity[8] = {0};
            size_t consumed = 0;
            if (decode_entity(p, entity, sizeof(entity), &consumed)) {
                EMIT_STR(entity);
                p += consumed;
                continue;
            }
        }

        /* Preserve UTF-8 bytes exactly. Anki stores its textual fields as UTF-8. */
        EMIT(*p);
        p++;
    }

    while (w > 0 && (out[w-1] == '\n' || out[w-1] == ' ' || out[w-1] == '\t')) w--;
    out[w] = 0;

    #undef EMIT
    #undef EMIT_STR
    #undef EMIT_NL
}

void anki_html_to_text(const char *in, char *out, size_t cap) {
    char dummy_refs[1][512];
    int dummy_count = 0;
    anki_html_process(in, out, cap, dummy_refs, &dummy_count, 0);
}
