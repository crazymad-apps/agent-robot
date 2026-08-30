#pragma once

#include <stddef.h>
#include <stdint.h>

uint8_t *audio_playback_ring_memory_allocate(size_t size);

void audio_playback_ring_memory_release(uint8_t *memory);
