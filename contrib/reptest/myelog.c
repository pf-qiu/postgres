#include "postgres_fe.h"
#include <signal.h>


/* Error level codes */
#define DEBUG5		10			/* Debugging messages, in categories of
								 * decreasing detail. */
#define DEBUG4		11
#define DEBUG3		12
#define DEBUG2		13
#define DEBUG1		14			/* used by GUC debug_* variables */
#define LOG			15			/* Server operational messages; sent only to
								 * server log by default. */
#define LOG_SERVER_ONLY 16		/* Same as LOG for server reporting, but never
								 * sent to client. */
#define COMMERROR	LOG_SERVER_ONLY /* Client communication problems; same as
									 * LOG for server reporting, but never
									 * sent to client. */
#define INFO		17			/* Messages specifically requested by user (eg
								 * VACUUM VERBOSE output); always sent to
								 * client regardless of client_min_messages,
								 * but by default not sent to server log. */
#define NOTICE		18			/* Helpful messages to users about query
								 * operation; sent to client and not to server
								 * log by default. */
#define WARNING		19			/* Warnings.  NOTICE is for expected messages
								 * like implicit sequence creation by SERIAL.
								 * WARNING is for unexpected messages. */
#define ERROR		20			/* user error - abort transaction; return to
								 * known state */

bool errstart(int elevel, const char *filename, int lineno,
              const char *funcname, const char *domain)
{
    if (elevel == ERROR)
    {
        raise(SIGINT);
    }
    return true;
}

void errfinish(int dummy, ...)
{
}

int
errcode(int sqlerrcode)
{
	return 0;					/* return value does not matter */
}

int
errdetail(const char *fmt,...)
{
	return 0;					/* return value does not matter */
}

void
elog_start(const char *filename, int lineno, const char *funcname)
{
}

void
elog_finish(int elevel, const char *fmt,...)
{

}

int errmsg(const char *fmt, ...)
{

    va_list args;
    int result;
    va_start(args, fmt);
    result = vprintf(fmt, args);
    va_end(args);

    return result; /* return value does not matter */
}