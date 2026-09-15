#include "pour_archive.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define ARCHIVE_DIR "/sdcard/pours"
typedef struct {
    pour_operation_t operation;
    pour_entry_t entry;
    pour_sample_t samples[POUR_ARCHIVE_SAMPLES];
} archive_command_t;
static QueueHandle_t commands;
static SemaphoreHandle_t result_mutex;
static pour_archive_result_t *shared;
static uint32_t pending;
static bool bus_ready;
static sdmmc_card_t *card;
static uint64_t sequence_floor;

static bool mount_card(void)
{
    if (card) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST; /* LCD remains on SPI2. */
    host.max_freq_khz = 10000;
    if (!bus_ready) {
        const spi_bus_config_t bus = {
            .mosi_io_num = GPIO_NUM_11, .miso_io_num = GPIO_NUM_13,
            .sclk_io_num = GPIO_NUM_12, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
        bus_ready = true;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI3_HOST;
    slot.gpio_cs = GPIO_NUM_10;
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16384,
    };
    if (esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &config, &card) != ESP_OK) {
        card = NULL;
        return false;
    }
    return mkdir(ARCHIVE_DIR, 0775) == 0 || errno == EEXIST;
}

static bool safe_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || n >= POUR_ARCHIVE_NAME_SIZE || strcmp(name + n - 4, ".csv")) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'z') ||
              name[i] == '-' || name[i] == '_' || name[i] == '.')) return false;
    return strstr(name, "..") == NULL;
}

static bool read_header(FILE *file, pour_entry_t *entry)
{
    char line[192];
    if (!fgets(line, sizeof(line), file)) return false;
    unsigned dose, target, count;
    if (sscanf(line, "POURBOT,1,%" SCNu64 ",%" SCNd64 ",%u,%u,%u,%23[^\r\n]",
        &entry->sequence, &entry->epoch, &dose, &target, &count, entry->recipe) != 6 ||
        count == 0 || count > POUR_ARCHIVE_SAMPLES || dose > 5000 || target > 65535) return false;
    entry->dose = dose; entry->target = target; entry->count = count;
    return true;
}

static void list_files(pour_archive_result_t *result)
{
    result->entry_count = 0;
    DIR *dir = opendir(ARCHIVE_DIR);
    if (!dir) { result->ok = false; snprintf(result->message, sizeof(result->message), "Could not read SD archive"); return; }
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (!safe_name(item->d_name)) continue;
        char path[96];
        snprintf(path, sizeof(path), ARCHIVE_DIR "/%.47s", item->d_name);
        FILE *file = fopen(path, "r");
        pour_entry_t entry = {0};
        bool valid = file && read_header(file, &entry);
        if (file) fclose(file);
        if (!valid) continue;
        if (entry.sequence > sequence_floor) sequence_floor = entry.sequence;
        strlcpy(entry.filename, item->d_name, sizeof(entry.filename));
        unsigned pos = 0;
        while (pos < result->entry_count && result->entries[pos].sequence >= entry.sequence) pos++;
        if (pos >= POUR_ARCHIVE_LIMIT) continue;
        unsigned last = result->entry_count < POUR_ARCHIVE_LIMIT ? result->entry_count++ : POUR_ARCHIVE_LIMIT - 1;
        for (unsigned i = last; i > pos; --i) result->entries[i] = result->entries[i - 1];
        result->entries[pos] = entry;
    }
    closedir(dir);
}

static bool next_sequence(uint64_t *sequence)
{
    nvs_handle_t nvs;
    if (nvs_open("pour_history", NVS_READWRITE, &nvs) != ESP_OK) return false;
    uint64_t previous = 0;
    esp_err_t err = nvs_get_u64(nvs, "sequence", &previous);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        if (previous < sequence_floor) previous = sequence_floor;
        if (previous == UINT64_MAX) { nvs_close(nvs); return false; }
        *sequence = previous + 1;
        err = nvs_set_u64(nvs, "sequence", *sequence);
        if (err == ESP_OK) err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) sequence_floor = *sequence;
    return err == ESP_OK;
}

static bool save_file(archive_command_t *command, pour_archive_result_t *result)
{
    if (!next_sequence(&command->entry.sequence)) return false;
    char base[40];
    if (command->entry.epoch > 1700000000) {
        time_t epoch = command->entry.epoch;
        struct tm local;
        localtime_r(&epoch, &local);
        snprintf(base, sizeof(base), "%d%d%d-%02d_%02d", local.tm_mon + 1,
            local.tm_mday, local.tm_year + 1900, local.tm_hour, local.tm_min);
    } else {
        snprintf(base, sizeof(base), "unsynced-%" PRIu64, command->entry.sequence);
    }
    char path[96], temp[96];
    struct stat st;
    unsigned suffix = 0;
    do {
        if (suffix == 0) snprintf(command->entry.filename, sizeof(command->entry.filename), "%s.csv", base);
        else snprintf(command->entry.filename, sizeof(command->entry.filename), "%.31s-%02u.csv", base, suffix);
        snprintf(path, sizeof(path), ARCHIVE_DIR "/%s", command->entry.filename);
        suffix++;
    } while (stat(path, &st) == 0 && suffix < 10000);
    if (stat(path, &st) == 0) return false;
    snprintf(temp, sizeof(temp), ARCHIVE_DIR "/pending-%" PRIu64 ".tmp", command->entry.sequence);
    FILE *file = fopen(temp, "w");
    if (!file) return false;
    bool ok = fprintf(file, "POURBOT,1,%" PRIu64 ",%" PRId64 ",%u,%u,%u,%s\n"
        "time_s,weight_g,flow_g_s\n", command->entry.sequence, command->entry.epoch,
        command->entry.dose, command->entry.target, command->entry.count, command->entry.recipe) > 0;
    for (unsigned i = 0; ok && i < command->entry.count; ++i) {
        const pour_sample_t *s = &command->samples[i];
        ok = fprintf(file, "%" PRIu32 ",%" PRId32 ".%" PRId32 ",%" PRId32 ".%" PRId32 "\n",
            s->seconds, s->weight_tenths / 10, s->weight_tenths % 10,
            s->flow_tenths / 10, s->flow_tenths % 10) > 0;
    }
    if (fflush(file) != 0) ok = false;
    if (ok && fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) { unlink(temp); return false; } /* Only our incomplete temporary file. */
    result->selected = command->entry;
    snprintf(result->message, sizeof(result->message), "Saved %s", command->entry.filename);
    return true;
}

static bool load_file(const char *name, pour_archive_result_t *result)
{
    if (!safe_name(name)) return false;
    char path[96], line[96];
    snprintf(path, sizeof(path), ARCHIVE_DIR "/%s", name);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool ok = read_header(file, &result->selected) && fgets(line, sizeof(line), file);
    strlcpy(result->selected.filename, name, sizeof(result->selected.filename));
    uint32_t previous = 0;
    for (unsigned i = 0; ok && i < result->selected.count; ++i) {
        pour_sample_t *s = &result->samples[i];
        int32_t w, wd, f, fd;
        ok = fgets(line, sizeof(line), file) && sscanf(line,
            "%" SCNu32 ",%" SCNd32 ".%" SCNd32 ",%" SCNd32 ".%" SCNd32,
            &s->seconds, &w, &wd, &f, &fd) == 5 && s->seconds >= previous &&
            w >= 0 && w <= 30000 && f >= 0 && f <= 3000 && wd >= 0 && wd <= 9 && fd >= 0 && fd <= 9;
        if (ok) { s->weight_tenths = w * 10 + wd; s->flow_tenths = f * 10 + fd; previous = s->seconds; }
    }
    fclose(file);
    return ok;
}

static bool delete_file(const char *name, pour_archive_result_t *result)
{
    if (!safe_name(name)) return false;
    char path[96];
    snprintf(path, sizeof(path), ARCHIVE_DIR "/%s", name);
    if (unlink(path) != 0) return false;
    snprintf(result->message, sizeof(result->message), "Deleted %s", name);
    return true;
}

static void archive_task(void *arg)
{
    pour_archive_result_t *result = arg;
    archive_command_t *command;
    while (xQueueReceive(commands, &command, portMAX_DELAY)) {
        memset(result, 0, sizeof(*result));
        result->operation = command->operation;
        result->ok = mount_card();
        snprintf(result->message, sizeof(result->message), result->ok ? "Latest 25 saved pours" : "SD unavailable - insert a FAT32 card, then refresh");
        if (result->ok && command->operation == POUR_SAVE) {
            /* Recover ordering if NVS was erased but this card still has history. */
            list_files(result);
            if (result->ok) result->ok = save_file(command, result);
            if (!result->ok) snprintf(result->message, sizeof(result->message), "Save failed - check SD card and free space");
        } else if (result->ok && command->operation == POUR_LOAD) {
            result->ok = load_file(command->entry.filename, result);
            if (!result->ok) snprintf(result->message, sizeof(result->message), "Could not read pour file");
        } else if (result->ok && command->operation == POUR_DELETE) {
            result->ok = delete_file(command->entry.filename, result);
            if (!result->ok) snprintf(result->message, sizeof(result->message), "Could not delete pour file");
        }
        if (result->ok && command->operation != POUR_LOAD) list_files(result);
        xSemaphoreTake(result_mutex, portMAX_DELAY);
        result->generation = shared->generation + 1;
        *shared = *result;
        pending--;
        xSemaphoreGive(result_mutex);
        free(command);
    }
}

bool pour_archive_start(void)
{
    shared = heap_caps_calloc(1, sizeof(*shared), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pour_archive_result_t *worker = heap_caps_calloc(1, sizeof(*worker), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    result_mutex = xSemaphoreCreateMutex();
    commands = xQueueCreate(2, sizeof(archive_command_t *));
    if (shared && worker && result_mutex && commands &&
        xTaskCreate(archive_task, "pour_sd", 6144, worker, 2, NULL) == pdPASS) return true;
    free(shared); shared = NULL;
    free(worker);
    if (commands) { vQueueDelete(commands); commands = NULL; }
    if (result_mutex) { vSemaphoreDelete(result_mutex); result_mutex = NULL; }
    return false;
}

static bool send_command(archive_command_t *command)
{
    if (!command || !commands || !shared) { free(command); return false; }
    xSemaphoreTake(result_mutex, portMAX_DELAY);
    pending++;
    bool ok = xQueueSend(commands, &command, 0) == pdTRUE;
    if (!ok) pending--;
    xSemaphoreGive(result_mutex);
    if (!ok) free(command);
    return ok;
}

bool pour_archive_list(void)
{
    archive_command_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) c->operation = POUR_LIST;
    return send_command(c);
}
bool pour_archive_load(const char *name)
{
    if (!name || !safe_name(name)) return false;
    archive_command_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) { c->operation = POUR_LOAD; strlcpy(c->entry.filename, name, sizeof(c->entry.filename)); }
    return send_command(c);
}
bool pour_archive_delete(const char *name)
{
    if (!name || !safe_name(name)) return false;
    archive_command_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) { c->operation = POUR_DELETE; strlcpy(c->entry.filename, name, sizeof(c->entry.filename)); }
    return send_command(c);
}
bool pour_archive_save(const pourbot_recipe_t *recipe, time_t epoch, uint16_t count,
    const uint32_t *seconds, const int32_t *weights, const int32_t *flows)
{
    if (!recipe || !count || count > POUR_ARCHIVE_SAMPLES) return false;
    archive_command_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) return false;
    c->operation = POUR_SAVE;
    c->entry.epoch = epoch; c->entry.count = count;
    c->entry.dose = recipe->dose_g; c->entry.target = pourbot_recipe_target_g(recipe);
    strlcpy(c->entry.recipe, recipe->name, sizeof(c->entry.recipe));
    for (char *p = c->entry.recipe; *p; ++p) if (*p == ',' || *p == '\n' || *p == '\r') *p = '_';
    for (unsigned i = 0; i < count; ++i) c->samples[i] = (pour_sample_t){seconds[i], weights[i] * 10, flows[i]};
    return send_command(c);
}
bool pour_archive_result(uint32_t after, pour_archive_result_t *result)
{
    if (!shared || !result || !result_mutex) return false;
    xSemaphoreTake(result_mutex, portMAX_DELAY);
    bool changed = shared->generation != after;
    if (changed) *result = *shared;
    xSemaphoreGive(result_mutex);
    return changed;
}
bool pour_archive_wait_idle(uint32_t timeout_ms)
{
    if (!result_mutex) return true;
    TickType_t start = xTaskGetTickCount();
    do {
        xSemaphoreTake(result_mutex, portMAX_DELAY);
        bool idle = pending == 0;
        xSemaphoreGive(result_mutex);
        if (idle) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout_ms));
    return false;
}
