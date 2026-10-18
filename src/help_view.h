#pragma once

// Help > Features and Usage: a window with the topic index, full-text
// search (see help_doc.h) and the rendered text of the selected topic,
// read from the help document embedded in the program
// (share/help/features.md).
//
// Shows the window, creating it on first use. `topic` (may be NULL) is the
// title of a topic to open on; an unknown title just opens the index.
void help_view_show(const char *topic);
