#ifndef SAMOSA_COMMAND_H
#define SAMOSA_COMMAND_H
#include <stddef.h>
/* Trusted host argv JSON, bounded process response, owned-group deadline.
 * Errors refer only to transport; caller validates public component envelopes. */
/* Executable entrypoints must dispatch the private native self-exec mode first. */
int command_guardian_dispatch(int,char **);
const char *command_call(const char *argv_json,const char *request,size_t length,
                         double timeout,char **response,size_t *response_length);
const char *command_call_controlled(const char *,const char *,size_t,double,
                                   char **,size_t *,int (*)(void *),void *);
#endif
