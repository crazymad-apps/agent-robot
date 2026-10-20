/* Runs the production audio_io worker with a deterministic clock and I2S adapter.
 * Notifications inject wire events at the exact underrun boundary; no sleeps,
 * network, provider calls or test branches in firmware production code. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_io.h"
#include "audio_hardware.h"
#include "audio_playback_queue_memory.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct audio_test_mutex { bool held; };
struct audio_hardware { bool playing; };
static TaskFunction_t worker;
static void *worker_context;
static audio_io_t *audio;
static int64_t now_us;
static jmp_buf worker_return;
static unsigned int starts, stops, writes, event_count, injections;
static audio_io_event_t events[8];
static bool delayed_end, cancel_at_underrun;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1U, sizeof(struct audio_test_mutex)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t ticks)
{ (void)ticks; assert(!lock->held); lock->held = true; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock)
{ assert(lock->held); lock->held = false; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t lock) { assert(!lock->held); free(lock); }
EventGroupHandle_t xEventGroupCreate(void) { return calloc(1U, sizeof(uint32_t)); }
uint32_t xEventGroupClearBits(EventGroupHandle_t group, uint32_t bits)
{ *group &= ~bits; return *group; }
uint32_t xEventGroupSetBits(EventGroupHandle_t group, uint32_t bits)
{ *group |= bits; return *group; }
uint32_t xEventGroupWaitBits(EventGroupHandle_t group, uint32_t bits,
                           BaseType_t clear, BaseType_t all, TickType_t ticks)
{
    (void)clear; (void)all; (void)ticks;
    if ((*group & bits) != bits && setjmp(worker_return) == 0) { worker(worker_context); }
    assert((*group & bits) == bits);
    return *group;
}
void vEventGroupDelete(EventGroupHandle_t group) { free(group); }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *context, unsigned int priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority;
    worker = fn; worker_context = context; *handle = context;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
void vTaskDelete(TaskHandle_t task) { (void)task; longjmp(worker_return, 1); }
int64_t esp_timer_get_time(void) { return now_us; }
uint8_t *audio_playback_queue_memory_allocate(size_t size) { return calloc(1U, size); }
void audio_playback_queue_memory_release(uint8_t *memory) { free(memory); }

audio_hardware_result_t audio_hardware_create(audio_hardware_t **hardware)
{ *hardware = calloc(1U, sizeof(**hardware)); return AUDIO_HARDWARE_RESULT_OK; }
void audio_hardware_destroy(audio_hardware_t *hardware) { free(hardware); }
audio_hardware_result_t audio_hardware_playback_start(audio_hardware_t *hardware)
{ assert(!hardware->playing); hardware->playing = true; ++starts; return AUDIO_HARDWARE_RESULT_OK; }
audio_hardware_result_t audio_hardware_playback_stop(audio_hardware_t *hardware)
{ assert(hardware->playing); hardware->playing = false; ++stops; return AUDIO_HARDWARE_RESULT_OK; }
audio_hardware_result_t audio_hardware_playback_mute(audio_hardware_t *hardware)
{ (void)hardware; return AUDIO_HARDWARE_RESULT_OK; }
audio_hardware_result_t audio_hardware_playback_unmute(audio_hardware_t *hardware)
{ assert(hardware->playing); return AUDIO_HARDWARE_RESULT_OK; }
audio_hardware_result_t audio_hardware_playback_write(audio_hardware_t *hardware,
                                                      const int32_t *words,
                                                      size_t count, uint32_t timeout)
{
    assert(hardware->playing); assert(words != NULL); assert(count > 0U); (void)timeout;
    ++writes; now_us += 20000;
    return AUDIO_HARDWARE_RESULT_OK;
}
audio_hardware_result_t audio_hardware_capture_start(audio_hardware_t *hardware)
{ (void)hardware; assert(!"unexpected capture"); return AUDIO_HARDWARE_RESULT_INVALID_STATE; }
audio_hardware_result_t audio_hardware_capture_stop(audio_hardware_t *hardware)
{ (void)hardware; assert(!"unexpected capture"); return AUDIO_HARDWARE_RESULT_INVALID_STATE; }
audio_hardware_result_t audio_hardware_capture_read(audio_hardware_t *hardware,
    int32_t *words, size_t capacity, size_t *count, uint32_t timeout)
{
    (void)hardware; (void)words; (void)capacity; (void)count; (void)timeout;
    assert(!"unexpected capture"); return AUDIO_HARDWARE_RESULT_INVALID_STATE;
}

static void end_stream(uint32_t stream_id, const char *reason)
{
    device_media_playback_end_t end = {.stream_id = stream_id};
    snprintf(end.output_id, sizeof(end.output_id), "output-%u", stream_id);
    snprintf(end.reason, sizeof(end.reason), "%s", reason);
    assert(audio_io_playback_end(audio, 1U, &end) == AUDIO_IO_RESULT_OK);
}

static void queue_stream(uint32_t stream_id, uint32_t frames, bool ended)
{
    device_media_playback_start_t start = {
        .stream_id = stream_id, .sample_count = frames * 320U,
    };
    snprintf(start.output_id, sizeof(start.output_id), "output-%u", stream_id);
    assert(audio_io_playback_begin(audio, 1U, &start) == AUDIO_IO_RESULT_OK);
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES] = {0};
    for (uint32_t sequence = 0; sequence < frames; ++sequence) {
        const device_pcm_frame_view_t frame = {
            .direction = DEVICE_PCM_DIRECTION_DOWNLINK, .stream_id = stream_id,
            .sequence = sequence, .payload = payload, .payload_size = sizeof(payload),
        };
        assert(audio_io_playback_push(audio, 1U, &frame) == AUDIO_IO_RESULT_OK);
    }
    if (ended) { end_stream(stream_id, "completed"); }
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    (void)clear;
    if (ticks != portMAX_DELAY) { now_us += (int64_t)ticks * 10000; return 0U; }
    audio_io_status_t status = {0};
    audio_io_get_status(audio, &status);
    if (status.playback_state == AUDIO_IO_PLAYBACK_STATE_IDLE) {
        longjmp(worker_return, 1);
    }
    /* The old worker reaches this branch a second time after accepting end,
     * waiting forever on an empty head despite the next stream being ready. */
    assert(delayed_end && injections == 0U);
    assert(status.playback_state == AUDIO_IO_PLAYBACK_STATE_BUFFERING);
    assert(status.playback_rebuffer_count == 1U);
    assert(status.consumed_sample_count == status.sample_count);
    assert(status.queued_frame_count == 0U);
    ++injections;
    end_stream(1U, "completed");
    queue_stream(2U, 1U, true);
    if (cancel_at_underrun) {
        assert(audio_io_playback_cancel(audio, 1U) == AUDIO_IO_RESULT_OK);
    }
    return 1U;
}

static void on_event(const audio_io_event_t *event, void *context)
{
    (void)context;
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = *event;
    assert(event->type != AUDIO_IO_EVENT_PLAYBACK_FAILED);
    if (event->type == AUDIO_IO_EVENT_PLAYBACK_COMPLETED) {
        assert(event->consumed_sample_count == event->sample_count);
        assert(event->queued_frame_count == 0U);
        assert(event->received_frame_count * 320U == event->sample_count);
    }
}

static void verify_playback(bool delay, bool cancel)
{
    delayed_end = delay; cancel_at_underrun = cancel;
    now_us = 0; starts = stops = writes = event_count = injections = 0;
    const audio_io_config_t config = {.playback_gain_percent = 30U, .event_callback = on_event};
    assert(audio_io_create(&config, &audio) == AUDIO_IO_RESULT_OK);
    assert(audio_io_start(audio) == AUDIO_IO_RESULT_OK);
    queue_stream(1U, 100U, !delay);
    if (!delay) { queue_stream(2U, 1U, true); }
    if (setjmp(worker_return) == 0) { worker(worker_context); }

    audio_io_status_t status = {0};
    audio_io_get_status(audio, &status);
    assert(status.playback_state == AUDIO_IO_PLAYBACK_STATE_IDLE);
    assert(status.queued_playback_count == 0U);
    assert(status.failed_playback_count == 0U);
    if (cancel) {
        assert(status.cancelled_playback_count == 1U);
        assert(status.completed_playback_count == 0U);
        assert(starts == 1U && stops == 1U && writes == 100U);
    } else {
        assert(status.completed_playback_count == 2U);
        assert(status.cancelled_playback_count == 0U);
        assert(starts == 2U && stops == 2U && writes == 101U);
        assert(event_count == 4U);
        assert(events[1].type == AUDIO_IO_EVENT_PLAYBACK_COMPLETED && events[1].stream_id == 1U);
        assert(events[1].playback_rebuffer_count == (delay ? 1U : 0U));
        assert(events[3].type == AUDIO_IO_EVENT_PLAYBACK_COMPLETED && events[3].stream_id == 2U);
    }
    /* A subsequent turn must still be able to play after completion/cancellation. */
    queue_stream(3U, 1U, true);
    if (setjmp(worker_return) == 0) { worker(worker_context); }
    audio_io_get_status(audio, &status);
    assert(status.completed_playback_count == (cancel ? 1U : 3U));
    audio_io_destroy(audio);
    audio = NULL;
}

int main(void)
{
    verify_playback(false, false);
    verify_playback(true, false);
    verify_playback(true, true);
    puts("audio worker late-end, ordered drain, cancellation and next-turn checks passed");
    return 0;
}
