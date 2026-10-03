#include "sdkconfig.h"

#if CONFIG_SYNTH_PROJECT_STORE

#include "project/project_templates.h"
#include "project/project_snapshot.h"
#include "project_store.h"
#include "esp_log.h"

static const char *TAG = "project_templates";

/* Generated table (project_templates_data.c, in the build directory). */
extern const project_template_t project_templates[];
extern const size_t project_templates_n;

size_t project_templates_count(void)
{
    return project_templates_n;
}

const char *project_templates_name(size_t i)
{
    return i < project_templates_n ? project_templates[i].name : NULL;
}

bool project_templates_load(size_t i)
{
    if (i >= project_templates_n) {
        ESP_LOGW(TAG, "template %u: no such template", (unsigned)i);
        return false;
    }
    const project_template_t *t = &project_templates[i];
    const uint8_t *payload = NULL;
    size_t len = 0;
    if (!project_store_check_image(t->data, t->len, &payload, &len, NULL)) {
        ESP_LOGW(TAG, "template '%s': invalid image - refused", t->name);
        return false;
    }
    return project_snapshot_load_buffer(payload, len, t->name);   /* logs its own failure */
}

#endif /* CONFIG_SYNTH_PROJECT_STORE */
