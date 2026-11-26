/* test.h -- the least a C test needs. */
#ifndef NINESHARE_TEST_H
#define NINESHARE_TEST_H
#include <stdio.h>
#include <stdlib.h>
static int t_failed, t_run;
#define CHECK(cond) do { t_run++; if (!(cond)) { t_failed++; fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define DONE() do { printf("%d checks, %d failed\n", t_run, t_failed); return t_failed ? 1 : 0; } while (0)
#endif
