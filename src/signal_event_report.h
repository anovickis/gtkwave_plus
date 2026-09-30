#pragma once

// Verdi-style "Signal Event Report": for every currently highlighted
// signal, over a time range (the primary<->baseline marker span if the
// baseline marker is enabled, else the whole loaded time range), reports
// rising/falling edge counts, total transition count, duty cycle, and
// longest continuous high duration. Shows the result in a small table
// window. No-op (with a status message) if nothing is highlighted.
void signal_event_report_show(void);
