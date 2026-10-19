#pragma once

// "Find Condition" window: type a derived-signal expression such as
// `valid && !ready` (same language as Derived Signals, signals by full name
// or by the short names of the highlighted ones), optionally a minimum
// duration, and list every interval where it is true. Previous/Next step the
// primary marker through them; "Add as Signal" turns the expression into a
// derived signal so the condition can be seen on the waveform.
void condition_search_view_show(void);
