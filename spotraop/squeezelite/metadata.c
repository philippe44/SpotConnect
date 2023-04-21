/*
 *  Metadata instance
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 *
 */

#include <stdlib.h>
#include <string.h>

#include "metadata.h"

#define __STRDUP__(s) ((s) ? strdup(s) : NULL)
#define __FREE__(s) do { \
	if (s) free(s);      \
	s = NULL;            \
} while (0)	

void metadata_free(struct metadata_s* const self) {
	if (!self) return;
	__FREE__(self->artist);
	__FREE__(self->album);
	__FREE__(self->title);
	__FREE__(self->remote_title);
	__FREE__(self->artwork);
	__FREE__(self->genre);
	metadata_init(self);
}

/*----------------------------------------------------------------------------*/
struct metadata_s* metadata_clone(struct metadata_s* const self) {
	struct metadata_s *clone = malloc(sizeof(*self));
	if (!clone) return clone;
	memcpy(clone, self, sizeof(*self));
	clone->artist  = __STRDUP__(self->artist);
	clone->album   = __STRDUP__(self->album);
	clone->title   = __STRDUP__(self->title);
	clone->remote_title = __STRDUP__(self->remote_title);
	clone->artwork = __STRDUP__(self->artwork);
	clone->genre = __STRDUP__(self->genre);
	return clone;
}

/*----------------------------------------------------------------------------*/
void metadata_defaults(struct metadata_s* const self) {
	if (!self->title) self->title = strdup("Streaming from LMS");
	if (!self->album) self->album = strdup("");
	if (!self->artist) self->artist = strdup("");
	if (!self->genre) self->genre = strdup("");
	if (!self->remote_title) self->remote_title = strdup("Streaming from LMS");
}

/*----------------------------------------------------------------------------*/
struct metadata_s* metadata_init(struct metadata_s* self) {
	if (!self) self = malloc(sizeof(*self));
	memset(self, 0, sizeof(*self));
	self->repeating = -1;
	return self;
}
