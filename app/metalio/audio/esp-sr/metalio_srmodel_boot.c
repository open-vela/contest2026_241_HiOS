/*
 * Boot-time srmodels.bin loader for Metalio / NuttX.
 * Embeds Claw4 wakeword/srmodels.bin and exposes LoadMetalioSrModels().
 */

#include "model_path.h"
#include "esp_log_shim.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Prebuilt libwakenet (ESP_PLATFORM ABI): model_data @ 20, num @ 16. */
_Static_assert(offsetof(srmodel_list_t, num) == 16,
               "srmodel_list_t.num ABI mismatch with libwakenet");
_Static_assert(offsetof(srmodel_list_t, model_data) == 20,
               "srmodel_list_t.model_data ABI mismatch with libwakenet");

#define TAG "SrBoot"

extern const unsigned char g_metalio_srmodels_bin[];
extern const unsigned char *g_metalio_srmodels_bin_end;
extern const unsigned int g_metalio_srmodels_bin_len;

static srmodel_list_t *s_models = NULL;

srmodel_list_t *LoadMetalioSrModels(void)
{
    if (s_models != NULL)
        return s_models;

    const size_t size = (size_t)g_metalio_srmodels_bin_len;
    if (size < 8)
    {
        ESP_LOGE(TAG, "embedded srmodels.bin empty");
        write(1, "SR_BIN0\n", 8);
        return NULL;
    }

    write(1, "SR_LOAD\n", 8);
    /* srmodel_load keeps pointers into the blob. Flash/XIP const data has
     * caused hard-locks inside WakeNet create; copy to RAM first. */
    {
        void *ram = malloc(size);
        if (ram == NULL)
        {
            write(1, "SR_NOMEM\n", 9);
            return NULL;
        }
        memcpy(ram, g_metalio_srmodels_bin, size);
        s_models = srmodel_load(ram);
        /* Keep ram for process lifetime (pointers into it). */
    }
    if (s_models == NULL || s_models->num <= 0)
    {
        ESP_LOGE(TAG, "srmodel_load failed size=%u", (unsigned)size);
        write(1, "SR_FAIL\n", 8);
        s_models = NULL;
        return NULL;
    }

    ESP_LOGI(TAG, "srmodels loaded num=%d size=%u", s_models->num,
             (unsigned)size);
    write(1, "SR_OK\n", 6);
    return s_models;
}

srmodel_list_t *GetMetalioSrModels(void)
{
    return s_models;
}
