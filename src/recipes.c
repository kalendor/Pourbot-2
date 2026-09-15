#include "recipes.h"

#include <string.h>
#include "nvs.h"

static pourbot_recipe_t recipes[POURBOT_RECIPE_COUNT] = {
    {"Kasuya 4:6 V60",     20, 150, 45, 45, 5},
    {"Onyx V60",           20, 150, 30, 20, 5},
    {"Chemex Classic",      30, 167, 30, 45, 3},
};
static uint8_t active_index;

uint16_t pourbot_recipe_target_g(const pourbot_recipe_t *recipe)
{
    return (uint16_t)(((uint32_t)recipe->dose_g * recipe->ratio_x10 + 5) / 10);
}

void pourbot_recipes_load(void)
{
    nvs_handle_t handle;
    if (nvs_open("recipes", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(recipes);
    uint8_t selected = 0;
    pourbot_recipe_t stored[POURBOT_RECIPE_COUNT];
    if (nvs_get_blob(handle, "presets_v2", stored, &size) == ESP_OK && size == sizeof(stored)) {
        memcpy(recipes, stored, sizeof(recipes));
        nvs_get_u8(handle, "active_v2", &selected);
    } else {
        /* Original four-preset layout: drop slot zero while preserving edits
         * and the selected identity of the remaining three recipes. */
        pourbot_recipe_t legacy[4];
        size = sizeof(legacy);
        if (nvs_get_blob(handle, "presets", legacy, &size) == ESP_OK && size == sizeof(legacy))
            memcpy(recipes, &legacy[1], sizeof(recipes));
        uint8_t previous = 0;
        if (nvs_get_u8(handle, "active", &previous) == ESP_OK && previous > 0 && previous < 4)
            selected = previous - 1;
    }
    if (selected < POURBOT_RECIPE_COUNT) {
        active_index = selected;
    }
    nvs_close(handle);
}

static void persist(void)
{
    nvs_handle_t handle;
    if (nvs_open("recipes", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_blob(handle, "presets_v2", recipes, sizeof(recipes));
    nvs_set_u8(handle, "active_v2", active_index);
    nvs_commit(handle);
    nvs_close(handle);
}

const pourbot_recipe_t *pourbot_recipe_get(uint8_t index)
{
    return index < POURBOT_RECIPE_COUNT ? &recipes[index] : &recipes[0];
}

const pourbot_recipe_t *pourbot_recipe_active(void) { return &recipes[active_index]; }
uint8_t pourbot_recipe_active_index(void) { return active_index; }

void pourbot_recipe_save_and_select(uint8_t index, const pourbot_recipe_t *recipe)
{
    if (index >= POURBOT_RECIPE_COUNT || !recipe) return;
    recipes[index] = *recipe;
    recipes[index].name[sizeof(recipes[index].name) - 1] = 0;
    active_index = index;
    persist();
}
