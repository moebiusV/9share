/* auth.c -- the SASL mechanisms of the link.  EXTERNAL: the carrier is trusted (an ssh session, a user's own socket). */
#include "auth.h"
#include <stdlib.h>
void lk_auth_free(struct lk_auth *a) { free(a); }
