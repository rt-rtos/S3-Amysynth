#pragma once

/* Built-in read-only project templates: whole-project images compiled into
 * the firmware, listed in a fixed order and loaded like a slot. They never
 * occupy storage and have no save path; a loaded template is saved like any
 * session (project_snapshot_save()). The image table is generated at build
 * time by templates/gen_templates.py from the template specs, after it has
 * checked its codec against this firmware's project format. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One embedded template: display name and full project image (32-byte
 * header + payload, the layout of a slot file). */
typedef struct {
    const char    *name;
    const uint8_t *data;
    size_t         len;
} project_template_t;

/* Number of built-in templates. Any task. */
size_t project_templates_count(void);

/* Display name of template i, or NULL when i >= project_templates_count().
 * Any task; the string is static. */
const char *project_templates_name(size_t i);

/* Validate template i's image (project_store_check_image()) and apply it
 * through project_snapshot_load_buffer(): same context and guarantees as
 * project_snapshot_load(). Returns false, logging once with the template
 * name, on an out-of-range index or an invalid image; the session is then
 * untouched. */
bool project_templates_load(size_t i);

#ifdef __cplusplus
}
#endif
