#ifndef _DBGUTILS_H_
#define _DBGUTILS_H_

#include <errno.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include "defs.h"
#include "dbgcmd.h"
#include "machthread.h"

extern boolean_t mach_exc_server(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

pid_t pid_of_program(char *);
const char *get_exception_code(exception_type_t);
void setup_exceptions(void);
void setup_initial_debuggee(void);

#endif
