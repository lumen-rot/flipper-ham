#pragma once

/* Development only. Keep 0 for releases and catalog submissions.
 * HAM takes over USB HID, preventing qFlipper screenshots while it is open.
 * This opt-in render-only mode captures the real device canvas without HID input,
 * as permitted by the catalog's HID screenshot exception. Its capture buffers,
 * launch arguments, and output code are compiled out when this flag is 0.
 */
#define HAM_ENABLE_PREVIEW 0
