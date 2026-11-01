#include "bwg_panic_cutoff.h"

#include "bwg_cutoff.h"
#include "bwg_revocation.h"
#include "esp_attr.h"
#include "sdkconfig.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

_Static_assert(CONFIG_GPIO_ASIC_RESET < 32 && CONFIG_GPIO_ASIC_ENABLE < 32, "the fatal path writes only the low output bank");

#define RECEIPT_MAGIC 0x50434f32u
#define SELF_TEST_MARKER 0x53544631u
#define RECEIPT_WORDS 6u

// Internal memory, readable while a fault has the flash caches off. Before the board's config is read, the
// enable pin's polarity isn't known and nothing drives it, so only the reset line is cut.
static DRAM_ATTR uint32_t s_set_mask;
static DRAM_ATTR uint32_t s_clear_mask = 1u << CONFIG_GPIO_ASIC_RESET;
static DRAM_ATTR uint32_t s_self_test_marker;

// Read back from a core dump: magic, pins cut, output enables, output latches, authority, self-test marker.
// The magic goes last, so a value here is evidence only once the cutoff above it has run.
COREDUMP_DRAM_ATTR uint32_t bwg_panic_cutoff_receipt[RECEIPT_WORDS];

void __real_esp_panic_handler(void *info);
void __wrap_esp_panic_handler(void *info);

// The other CPU is stalled by now. No scheduler, allocator, lock or driver is usable here. Separate set and clear
// stores can't disturb other pins, and power goes before the reset line, with nothing that can fail ahead of them.
void IRAM_ATTR __wrap_esp_panic_handler(void *info)
{
    REG_WRITE(GPIO_OUT_W1TS_REG, s_set_mask);
    REG_WRITE(GPIO_OUT_W1TC_REG, s_clear_mask);
    bwg_panic_cutoff_receipt[1] = s_set_mask | s_clear_mask;
    bwg_panic_cutoff_receipt[2] = REG_READ(GPIO_ENABLE_REG);
    bwg_panic_cutoff_receipt[3] = REG_READ(GPIO_OUT_REG);
    bwg_panic_cutoff_receipt[4] = bwg_revocation_panic_word();
    bwg_panic_cutoff_receipt[5] = s_self_test_marker;
    bwg_panic_cutoff_receipt[0] = RECEIPT_MAGIC;
    __real_esp_panic_handler(info);
}

void bwg_panic_cutoff_configure(const DeviceConfig *config)
{
    // Both flags leave the pin an output: vcore.c configures it for either.
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(CONFIG_GPIO_ASIC_RESET, config->asic_enable || config->plug_sense,
                                                   CONFIG_GPIO_ASIC_ENABLE, config->asic_enable_active_high);
    s_set_mask = plan.set_mask;
    s_clear_mask = plan.clear_mask;
}

bool bwg_panic_cutoff_outputs_off(void)
{
    const bwg_cutoff_plan_t plan = {.set_mask = s_set_mask, .clear_mask = s_clear_mask};
    return bwg_cutoff_outputs_off(plan, REG_READ(GPIO_ENABLE_REG), REG_READ(GPIO_OUT_REG));
}

void bwg_panic_cutoff_mark_self_test(void)
{
    s_self_test_marker = SELF_TEST_MARKER;
}
