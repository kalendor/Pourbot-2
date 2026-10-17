#include "recipes.h"

#include <string.h>
#include "nvs.h"

static pourbot_recipe_t recipes[POURBOT_RECIPE_COUNT] = {
    {"My Pour", 20, 150, 45, 30, 5},
};

uint16_t pourbot_recipe_target_g(const pourbot_recipe_t *recipe)
{
    return (uint16_t)(((uint32_t)recipe->dose_g * recipe->ratio_x10 + 5) / 10);
}

void pourbot_recipes_load(void)
{
    nvs_handle_t handle;
    if (nvs_open("recipes", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(recipes);
    pourbot_recipe_t stored[POURBOT_RECIPE_COUNT];
    if (nvs_get_blob(handle, "custom_v1", stored, &size) == ESP_OK &&
        size == sizeof(stored)) {
        memcpy(recipes, stored, sizeof(recipes));
    } else {
        /* Migrate the selected recipe from the previous three-slot builder. */
        pourbot_recipe_t legacy_three[3];
        uint8_t selected = 0;
        size = sizeof(legacy_three);
        if (nvs_get_blob(handle, "presets_v2", legacy_three, &size) == ESP_OK &&
            size == sizeof(legacy_three)) {
            nvs_get_u8(handle, "active_v2", &selected);
            if (selected >= 3) selected = 0;
            recipes[0] = legacy_three[selected];
        } else {
            /* Also retain compatibility with the original four presets. */
            pourbot_recipe_t legacy_four[4];
            uint8_t previous = 0;
            size = sizeof(legacy_four);
            if (nvs_get_blob(handle, "presets", legacy_four, &size) == ESP_OK &&
                size == sizeof(legacy_four)) {
                nvs_get_u8(handle, "active", &previous);
                if (previous >= 4) previous = 0;
                recipes[0] = legacy_four[previous];
            }
        }
    }
    nvs_close(handle);
}

static void persist(void)
{
    nvs_handle_t handle;
    if (nvs_open("recipes", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_blob(handle, "custom_v1", recipes, sizeof(recipes));
    nvs_commit(handle);
    nvs_close(handle);
}

const pourbot_recipe_t *pourbot_recipe_active(void) { return &recipes[0]; }
uint8_t pourbot_recipe_active_index(void) { return 0; }

void pourbot_recipe_save_and_select(uint8_t index, const pourbot_recipe_t *recipe)
{
    if (index >= POURBOT_RECIPE_COUNT || !recipe) return;
    recipes[index] = *recipe;
    recipes[index].name[sizeof(recipes[index].name) - 1] = 0;
    persist();
}
