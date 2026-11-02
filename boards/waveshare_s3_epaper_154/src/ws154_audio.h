/* Speaker of the Waveshare ESP32-S3-ePaper-1.54 V2 (PLAN D89): ES8311 codec on I2S, NS4150B amplifier. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epager/board_api.h"

/* Signature of board_profile_t.play_audio. Only 16 kHz is supported (the codec's clock dividers are set for it). */
bool ws154_audio_play(void *ctx, board_audio_fn sample, void *user, uint32_t n_samples, uint32_t rate_hz);

/* The amplifier enable pin as a low output that keeps its level in light sleep (board_init). */
bool ws154_audio_init_pins(void);
