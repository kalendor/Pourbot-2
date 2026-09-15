#pragma once

#include <stdint.h>

#define POURBOT_RECIPE_COUNT 4

typedef struct {
    char name[24];
    uint16_t dose_g;
    uint16_t ratio_x10;
    uint16_t bloom_hold_s;
    uint16_t pour_hold_s;
    uint8_t pour_count;
} pourbot_recipe_t;

void pourbot_recipes_load(void);
const pourbot_recipe_t *pourbot_recipe_get(uint8_t index);
const pourbot_recipe_t *pourbot_recipe_active(void);
uint8_t pourbot_recipe_active_index(void);
void pourbot_recipe_save_and_select(uint8_t index, const pourbot_recipe_t *recipe);
uint16_t pourbot_recipe_target_g(const pourbot_recipe_t *recipe);
