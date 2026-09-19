#ifndef METALIO_SRMODEL_BOOT_H
#define METALIO_SRMODEL_BOOT_H

#include "model_path.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load embedded srmodels.bin once; returns NULL on failure. */
srmodel_list_t *LoadMetalioSrModels(void);
srmodel_list_t *GetMetalioSrModels(void);

#ifdef __cplusplus
}
#endif

#endif
