#include "bwg_unsafe_detail.h"

#include <stdatomic.h>

#include "bwg_evidence_lines.h"
#include "bwg_gate_ops.h"
#include "bwg_revocation.h"

// The power task writes and the writer task reads. A change of the word around a read discards that read.
static bwg_unsafe_detail_t s_detail;
static _Atomic uint32_t s_published;
static _Atomic uint32_t s_writes;

static void record(const char *trigger, const char *fact, bool has_value, int32_t value_milli, uint32_t since_safe_ms)
{
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    // The heartbeat or the lease's end can get there first, and that generation's reason is theirs.
    if (snapshot.lease_reason != BWG_STOP_REASON_UNSAFE_OBSERVATION) {
        return;
    }
    atomic_store(&s_published, 0);
    atomic_fetch_add(&s_writes, 1);
    s_detail = (bwg_unsafe_detail_t) {
        .generation = bwg_gate_ops_lease_generation(),
        .trigger = trigger,
        .fact = fact,
        .has_value = has_value,
        .value_milli = value_milli,
        .since_safe_ms = since_safe_ms,
        .closed_ms = snapshot.lease_closed_ms,
    };
    atomic_store(&s_published, 1);
}

void bwg_unsafe_detail_note_sample(const bwg_safety_sample_t *sample, bwg_safety_fact_t fact, uint32_t since_safe_ms)
{
    // Rust reports a stopped fan as its own trigger, which the Gate reads as naming no fact.
    if (fact == BWG_SAFETY_FACT_FAN_RPM) {
        record("zero_fan", "none", false, 0, since_safe_ms);
        return;
    }
    int32_t value_milli = 0;
    const bool has_value = bwg_limits_fact_value_milli(sample, fact, &value_milli);
    record("unsafe_sample", bwg_limits_fact_label(fact), has_value, value_milli, since_safe_ms);
}

void bwg_unsafe_detail_note_overheat(bool chip, float celsius, uint32_t since_safe_ms)
{
    const bwg_safety_fact_t fact = chip ? BWG_SAFETY_FACT_CHIP_TEMPERATURE : BWG_SAFETY_FACT_VR_TEMPERATURE;
    const bwg_safety_sample_t sample = {.chip_celsius = celsius, .regulator_celsius = celsius};
    bwg_unsafe_detail_note_sample(&sample, fact, since_safe_ms);
}

bool bwg_unsafe_detail_format(char *line, size_t capacity)
{
    const uint32_t writes = atomic_load(&s_writes);
    if (atomic_load(&s_published) == 0) {
        return false;
    }
    const bwg_unsafe_detail_t copy = s_detail;
    if (atomic_load(&s_writes) != writes || atomic_load(&s_published) == 0) {
        return false;
    }
    return bwg_line_unsafe_detail(line, capacity, &copy);
}
