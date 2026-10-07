#include "import/anki_html.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Strip all HTML. Block-level closes and <br> become \n.
   [sound:...] and <img src="..."> are extracted as media refs and
   removed from the visible text. */
void anki_html_process(const char *in, char *out, size_t cap,
                       char media_refs[][256], int *media_count,
                       int media_cap) {
    size_t w = 0;
    const char *p = in;

    #define EMIT(ch) do { if (w + 1 < cap) out[w++] = (ch); } while (0)
    #define EMIT_NL() do { \
        if (w > 0 && out[w-1] != '\n') EMIT('\n'); \
    } while (0)

    while (*p && w + 1 < cap) {
        /* [sound:audio.mp3] — extract filename, register, emit nothing. */
        if (p[0] == '[' && strncasecmp(p, "[sound:", 7) == 0) {
            const char *close = strchr(p + 7, ']');
            if (close) {
                char fname[256];
                size_t len = (size_t)(close - (p + 7));
                if (len >= sizeof(fname)) len = sizeof(fname) - 1;
                memcpy(fname, p + 7, len);
                fname[len] = 0;
                if (*media_count < media_cap) {
                    bool dup = false;
                    for (int i = 0; i < *media_count; i++)
                        if (strcmp(media_refs[i], fname) == 0) { dup = true; break; }
                    if (!dup) {
                        snprintf(media_refs[*media_count], 256, "%s", fname);
                        (*media_count)++;
                    }
                }
                p = close + 1;
                continue;
            }
        }

        /* <img src="..."> — extract src, register, emit nothing. */
        if (p[0] == '<' && strncasecmp(p, "<img", 4) == 0) {
            const char *close = strchr(p, '>');
            if (close) {
                char src[256] = {0};
                const char *sp = p;
                while ((sp = strcasestr(sp, "src")) != NULL && sp < close) {
                    const char *q = sp + 3;
                    while (*q == ' ' || *q == '\t' || *q == '=') q++;
                    char quote = 0;
                    if (*q == '"' || *q == '\'') { quote = *q; q++; }
                    const char *start = q;
                    while (q < close && *q &&
                           (quote ? *q != quote :
                                    *q != ' ' && *q != '>' && *q != '/'))
                        q++;
                    size_t len = (size_t)(q - start);
                    if (len > 0 && len < sizeof(src)) {
                        memcpy(src, start, len);
                        src[len] = 0;
                    }
                    break;
                }
                if (src[0] && *media_count < media_cap) {
                    bool dup = false;
                    for (int i = 0; i < *media_count; i++)
                        if (strcmp(media_refs[i], src) == 0) { dup = true; break; }
                    if (!dup) {
                        snprintf(media_refs[*media_count], 256, "%s", src);
                        (*media_count)++;
                    }
                }
                p = close + 1;
                continue;
            }
        }

        /* Any other tag: strip it. Block-closing tags become newlines. */
        if (p[0] == '<') {
            const char *close = strchr(p, '>');
            if (close) {
                const char *name = p + 1;
                bool closing = false;
                if (name[0] == '/') { closing = true; name++; }

                char tname[16];
                size_t nlen = 0;
                while (name[nlen] && !isspace((unsigned char)name[nlen]) &&
                       name[nlen] != '>' && name[nlen] != '/' &&
                       nlen < sizeof(tname) - 1) {
                    tname[nlen] = (char)tolower((unsigned char)name[nlen]);
                    nlen++;
                }
                tname[nlen] = 0;

                bool is_break =
                    (nlen == 2 && strcmp(tname, "br") == 0) ||
                    (closing && nlen == 1 && strcmp(tname, "p") == 0) ||
                    (closing && nlen == 3 && strcmp(tname, "div") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "li") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "tr") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "h1") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "h2") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "h3") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "h4") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "h5") == 0) ||
                    (closing && nlen == 2 && strcmp(tname, "h6") == 0);

                if (is_break) EMIT_NL();
                p = close + 1;
                continue;
            }
        }

        /* HTML entities. */
        if (p[0] == '&') {
            if (strncmp(p, "&amp;",   5) == 0) { EMIT('&');  p += 5; continue; }
            if (strncmp(p, "&lt;",    4) == 0) { EMIT('<');  p += 4; continue; }
            if (strncmp(p, "&gt;",    4) == 0) { EMIT('>');  p += 4; continue; }
            if (strncmp(p, "&quot;",  6) == 0) { EMIT('"');  p += 6; continue; }
            if (strncmp(p, "&apos;",  6) == 0) { EMIT('\''); p += 6; continue; }
            if (strncmp(p, "&#39;",   5) == 0) { EMIT('\''); p += 5; continue; }
            if (strncmp(p, "&nbsp;",  6) == 0) { EMIT(' ');  p += 6; continue; }
            if (strncmp(p, "&mdash;", 7) == 0) {
                const char *m = "\xE2\x80\x94";
                for (const char *q = m; *q && w + 1 < cap; q++) out[w++] = *q;
                p += 7; continue;
            }
            if (strncmp(p, "&ndash;", 7) == 0) {
                const char *m = "\xE2\x80\x93";
                for (const char *q = m; *q && w + 1 < cap; q++) out[w++] = *q;
                p += 7; continue;
            }
            if (strncmp(p, "&hellip;", 8) == 0) {
                const char *m = "\xE2\x80\xA6";
                for (const char *q = m; *q && w + 1 < cap; q++) out[w++] = *q;
                p += 8; continue;
            }
        }

        /* Default: copy byte-for-byte (UTF-8 passes through unchanged). */
        EMIT(*p);
        p++;
    }

    /* Trim trailing whitespace-only lines. */
    while (w > 0 && (out[w-1] == '\n' || out[w-1] == ' ' || out[w-1] == '\t'))
        w--;
    out[w] = 0;

    #undef EMIT
    #undef EMIT_NL
}

void anki_html_to_text(const char *in, char *out, size_t cap) {
    char dummy_refs[1][256];
    int dummy_count = 0;
    anki_html_process(in, out, cap, dummy_refs, &dummy_count, 0);
}
