#include "bwg_core_dump.h"

#include "bwg_core_dump_receipt.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "sdkconfig.h"

// Survives the reset that follows a panic, and the current copy stays readable with the flash caches off.
RTC_NOINIT_ATTR static volatile uint32_t s_rtc[BWG_CORE_DUMP_WORDS];
static DRAM_ATTR volatile uint32_t s_current[BWG_CORE_DUMP_WORDS];
static uint32_t s_previous[BWG_CORE_DUMP_WORDS];
static uint64_t s_source_hash;
static uint64_t s_boot_ordinal;

// Runs on the fatal path as well, and an interrupted write mustn't read as complete: the commit word goes last.
static inline __attribute__((always_inline)) void commit_words(const uint32_t *words)
{
    s_current[0] = 0;
    s_rtc[0] = 0;
    for (uint32_t index = 1; index < BWG_CORE_DUMP_WORDS; index++) {
        s_current[index] = words[index];
        s_rtc[index] = words[index];
    }
    s_current[0] = BWG_CORE_DUMP_COMMIT;
    s_rtc[0] = BWG_CORE_DUMP_COMMIT;
}

void bwg_core_dump_init(uint64_t source_hash, uint64_t boot_ordinal, bool cold_boot)
{
    for (uint32_t index = 0; index < BWG_CORE_DUMP_WORDS; index++) {
        s_previous[index] = cold_boot ? 0 : s_rtc[index];
    }
    s_source_hash = source_hash;
    s_boot_ordinal = boot_ordinal;
    const esp_partition_t *maybe_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, source_hash, boot_ordinal, maybe_partition == NULL ? 0 : maybe_partition->size);
    commit_words(words);
}

bool bwg_core_dump_is_ready(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    return s_current[0] == BWG_CORE_DUMP_COMMIT && s_current[7] != 0;
#else
    return false;
#endif
}

static void IRAM_ATTR receipt_update(uint32_t stage, uint32_t field, uint32_t value, int32_t result)
{
    if (s_current[0] != BWG_CORE_DUMP_COMMIT || stage > BWG_CORE_DUMP_LAST_STAGE) {
        return;
    }
    uint32_t words[BWG_CORE_DUMP_WORDS];
    for (uint32_t index = 0; index < BWG_CORE_DUMP_WORDS; index++) {
        words[index] = s_current[index];
    }
    bwg_core_dump_checkpoint(words, stage, field, value, result);
    commit_words(words);
}

void bwg_core_dump_mark_self_test(void)
{
    receipt_update(0, BWG_CORE_DUMP_FIELD_SELF_TEST, 1, 0);
}

bool bwg_core_dump_format_line(char *out, size_t size, bool previous_boot)
{
    if (previous_boot) {
        return bwg_core_dump_format(out, size, s_previous, s_source_hash, s_boot_ordinal - 1, true);
    }
    uint32_t words[BWG_CORE_DUMP_WORDS];
    for (uint32_t index = 0; index < BWG_CORE_DUMP_WORDS; index++) {
        words[index] = s_current[index];
    }
    return bwg_core_dump_format(out, size, words, s_source_hash, s_boot_ordinal, false);
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

// The store's own seams, so a receipt tells how far it got. The wrapped functions take IDF's private write context,
// which passes through untouched.
esp_err_t __real_esp_core_dump_write_elf(void);
esp_err_t __real_esp_core_dump_write_init(void);
esp_err_t __real_esp_core_dump_write_prepare(void *data, uint32_t *length);
esp_err_t __real_esp_core_dump_write_start(void *data);
esp_err_t __real_esp_core_dump_write_end(void *data);
esp_err_t __wrap_esp_core_dump_write_elf(void);
esp_err_t __wrap_esp_core_dump_write_init(void);
esp_err_t __wrap_esp_core_dump_write_prepare(void *data, uint32_t *length);
esp_err_t __wrap_esp_core_dump_write_start(void *data);
esp_err_t __wrap_esp_core_dump_write_end(void *data);

esp_err_t IRAM_ATTR __wrap_esp_core_dump_write_elf(void)
{
    receipt_update(1, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    const esp_err_t result = __real_esp_core_dump_write_elf();
    receipt_update(10, BWG_CORE_DUMP_FIELD_NONE, 0, result);
    return result;
}

esp_err_t IRAM_ATTR __wrap_esp_core_dump_write_init(void)
{
    receipt_update(2, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    const esp_err_t result = __real_esp_core_dump_write_init();
    receipt_update(3, BWG_CORE_DUMP_FIELD_NONE, 0, result);
    return result;
}

esp_err_t IRAM_ATTR __wrap_esp_core_dump_write_prepare(void *data, uint32_t *length)
{
    receipt_update(4, BWG_CORE_DUMP_FIELD_REQUESTED, *length, 0);
    const esp_err_t result = __real_esp_core_dump_write_prepare(data, length);
    receipt_update(5, BWG_CORE_DUMP_FIELD_PREPARED, *length, result);
    return result;
}

esp_err_t IRAM_ATTR __wrap_esp_core_dump_write_start(void *data)
{
    receipt_update(6, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    const esp_err_t result = __real_esp_core_dump_write_start(data);
    receipt_update(7, BWG_CORE_DUMP_FIELD_NONE, 0, result);
    return result;
}

esp_err_t IRAM_ATTR __wrap_esp_core_dump_write_end(void *data)
{
    receipt_update(8, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    const esp_err_t result = __real_esp_core_dump_write_end(data);
    receipt_update(9, BWG_CORE_DUMP_FIELD_NONE, 0, result);
    return result;
}

#endif
