#pragma once

#include <stddef.h>

typedef struct pack_paths {
	char dir[96];
	char so[160];
	char hud[160];
	char stealth[160];
	char kpatch[160];
} pack_paths;

/* Fresh /data/adb/.<rand>/ with random 8-hex names. Caller must pack_wipe. */
int pack_extract(pack_paths *out);
void pack_wipe(const char *dir);
