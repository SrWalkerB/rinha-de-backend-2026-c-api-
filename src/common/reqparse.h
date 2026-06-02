/* reqparse.h — branchy-but-cheap hand parser for the fraud-score payload.
 * No JSON library. Operates on a NUL-terminated buffer. Shared by api + verify. */
#ifndef REQPARSE_H
#define REQPARSE_H
#include "fraud.h"

/* Parse a NUL-terminated request payload into req. Returns 0 ok, -1 malformed.
 * `body` must be NUL-terminated and scoped to a single request object. */
int req_parse(const char *body, Request *req);

#endif
