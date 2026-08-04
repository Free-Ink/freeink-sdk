/* Compiles the vendored miniz with content's configuration
 * (public domain; see third_party/miniz/LICENSE). An embedding application
 * may provide the identical implementation under the configured external
 * symbols instead (CONTENT_EXTERNAL_MINIZ), avoiding two miniz copies in one
 * LTO image. */
#if !defined(CONTENT_EXTERNAL_MINIZ)
#include "ContentMinizConfig.h"
#include "../../third_party/miniz/miniz.c"
#endif
