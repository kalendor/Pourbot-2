#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "recipes.h"

#define POUR_ARCHIVE_LIMIT 25
#define POUR_ARCHIVE_SAMPLES 1200
#define POUR_ARCHIVE_NAME_SIZE 48
typedef struct { uint32_t seconds; int32_t weight_tenths; int32_t flow_tenths; } pour_sample_t;
typedef struct {
    char filename[POUR_ARCHIVE_NAME_SIZE];
    char recipe[24];
    uint64_t sequence;
    int64_t epoch;
    uint16_t dose, target, count;
} pour_entry_t;
typedef enum { POUR_LIST, POUR_LOAD, POUR_SAVE, POUR_DELETE } pour_operation_t;
typedef struct {
    uint32_t generation;
    pour_operation_t operation;
    bool ok;
    char message[96];
    uint8_t entry_count;
    pour_entry_t entries[POUR_ARCHIVE_LIMIT];
    pour_entry_t selected;
    pour_sample_t samples[POUR_ARCHIVE_SAMPLES];
} pour_archive_result_t;

bool pour_archive_start(void);
bool pour_archive_list(void);
bool pour_archive_load(const char *filename);
bool pour_archive_delete(const char *filename);
bool pour_archive_save(const pourbot_recipe_t *recipe, time_t epoch, uint16_t count,
    const uint32_t *seconds, const int32_t *weights_g, const int32_t *flows_tenths);
/* No disk IO in these functions. Result copies and command snapshots use PSRAM. */
bool pour_archive_result(uint32_t after, pour_archive_result_t *result);
bool pour_archive_wait_idle(uint32_t timeout_ms);
