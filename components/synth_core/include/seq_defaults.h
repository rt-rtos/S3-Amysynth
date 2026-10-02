#pragma once

#include "seq_model.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_SEQ_MELODIC_ENV_EG0_TYPE
#define CONFIG_SEQ_MELODIC_ENV_EG0_TYPE 0
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_ATTACK_MS
#define CONFIG_SEQ_MELODIC_ENV_ATTACK_MS 4
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_DECAY_MS
#define CONFIG_SEQ_MELODIC_ENV_DECAY_MS 100
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT
#define CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT 60
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_RELEASE_MS
#define CONFIG_SEQ_MELODIC_ENV_RELEASE_MS 200
#endif

static inline seq_env_t seq_default_melodic_env(void)
{
    return (seq_env_t) {
        .eg_type     = CONFIG_SEQ_MELODIC_ENV_EG0_TYPE,
        .attack_ms   = CONFIG_SEQ_MELODIC_ENV_ATTACK_MS,
        .decay_ms    = CONFIG_SEQ_MELODIC_ENV_DECAY_MS,
        .sustain_pct = CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT,
        .release_ms  = CONFIG_SEQ_MELODIC_ENV_RELEASE_MS,
    };
}

/* Default EG1, deliberately not Kconfig-gated: EG1 stays dormant until a
 * patch's coefs reference it or the row's filter routes through it, so a
 * constant suffices. Slower decay/release than the amp env above - the
 * classic "filter sweep lags the amp" shape. */
static inline seq_env_t seq_default_melodic_env1(void)
{
    return (seq_env_t) {
        .eg_type     = 0,   /* ENVELOPE_NORMAL */
        .attack_ms   = 15,
        .decay_ms    = 450,
        .sustain_pct = 25,
        .release_ms  = 400,
    };
}

#ifdef __cplusplus
}
#endif
