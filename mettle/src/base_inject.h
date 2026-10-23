#ifndef METTLE_BASE_INJECT_H
#define METTLE_BASE_INJECT_H

#include <stddef.h>

int migrate_support(void);
int migrate(int pid, char *migrate_stub, size_t migrate_stub_length,
	char *payload, size_t payload_length, const char *uuid, int fd);

#endif
