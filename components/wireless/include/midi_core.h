#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport-agnostic MIDI byte-stream parser + router.
 *
 * Transports (ble_midi) strip their framing and feed
 * plain MIDI bytes here; complete note messages go to the application's sink.
 * Everything except note-on/note-off is parsed (keeping running status
 * coherent) and dropped.
 *
 * Threading: SINGLE PRODUCER, currently the NimBLE host task. A second
 * transport must share that task or add its own serialization before calling
 * midi_core_feed(). The sink runs in the producer's context, so it must be
 * cheap and cross-task safe (the live_play sink goes through the bounded
 * amy_helpers event mutex). */

typedef struct {
    void (*note_on)(uint8_t channel, uint8_t note, uint8_t velocity);
    void (*note_off)(uint8_t channel, uint8_t note);
} midi_sink_t;

/* Register the sink. `sink` must have static lifetime; NULL detaches. */
void midi_core_set_sink(const midi_sink_t *sink);

/* Drop parser state (running status, pending data). Call at session start
 * and whenever a transport (re)connects so a torn packet can't misparse. */
void midi_core_reset(void);

/* Feed de-framed MIDI bytes (any split: per byte, per message, per packet). */
void midi_core_feed(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
