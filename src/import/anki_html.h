/* anki_html.h */
#ifndef ANKI_HTML_H
#define ANKI_HTML_H

#include <stdbool.h>
#include <stddef.h>

/* Convert Anki HTML to plain-ish text, collecting <img src="..."> and
   [sound:...] references. The output text preserves <b>, <i>, <br>
   as simple tags the card renderer already understands.

   media_refs: array of char[256] to fill with referenced filenames.
   media_count: in/out — current count on entry, updated on exit. */
void anki_html_to_text(const char *in, char *out, size_t cap);

void anki_html_process(const char *in, char *out, size_t cap,
                       char media_refs[][256], int *media_count,
                       int media_cap);

#endif
