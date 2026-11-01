/* Refuse analysis gaps instead of inventing values for undefined inputs. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void revng_abort(void *reason)
{
    (void)reason;
    fputs("unresolved rev.ng control flow\n", stderr);
    abort();
}

_Noreturn const void *undef_value(size_t size)
{
    (void)size;
    fputs("unresolved rev.ng value\n", stderr);
    abort();
}
