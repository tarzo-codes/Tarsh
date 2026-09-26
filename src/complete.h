#ifndef COMPLETE_H
#define COMPLETE_H

#include "lineedit.h"

// Classic Tab completion (used when fzf isn't available): completes the
// word before the cursor as a command (first word) or a path. One match
// is filled in; several fill in their common start and are listed.
void complete_basic(EditBuffer *line);

#endif
