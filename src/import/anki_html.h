/* anki_html.h */
#ifndef ANKI_HTML_H
#define ANKI_HTML_H

#include <stdbool.h>
#include <stddef.h>

/* Convert Anki HTML to plain-ish text, collecting <img src="..."> and
   [sound:...] references.

   Block-level tags (<p>, <div>, <li>, <tr>, headings, <br>, <hr>) become
   line breaks. Inline formatting tags (<b>, <i>, <em>, <u>, <strong>,
   <span>) are stripped, but their text content is preserved — StudyQuest's
   centered Japanese renderer has no italic font and renders a single style,
   so inline styling is flattened rather than shown as raw tags. HTML
   entities (including numeric ones) are decoded to UTF-8.

   media_refs: array of char[512] to fill with referenced filenames.
   media_count: in/out — current count on entry, updated on exit. */
void anki_html_to_text(const char *in, char *out, size_t cap);

void anki_html_process(const char *in, char *out, size_t cap,
                       char media_refs[][512], int *media_count,
                       int media_cap);

#endif
