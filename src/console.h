#pragma once
#include <stddef.h>
void console_start(void);
// Run one command line, capturing its output into `out` (web UI terminal).
int console_exec(const char *line, char *out, size_t out_len);
