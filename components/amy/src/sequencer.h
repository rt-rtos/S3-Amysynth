//  sequencer.h
#ifndef __SEQUENCERH
#define __SEQUENCERH

#include "amy.h"
#define MIDI_SEQUENCER_PPQ 24  // MIDI clocks per quarter note
uint32_t sequencer_ticks();
void sequencer_init(int max_num_sequences);
void sequencer_deinit();
void sequencer_reset();
void sequencer_debug();

void sequencer_recompute();
void sequencer_check_and_fill();  // called once per block from amy_execute_deltas()
#ifdef __EMSCRIPTEN__
void sequencer_check_and_call_js_hook();  // called from the browser main loop
#endif
// Store a wire message (with its leading 'H' already stripped) in the
// sequencer.  If has_tag is true, it's ADDED to tag: events on one tag
// accumulate, each with its own tick and period, so a tag can name a whole
// pattern rather than a single event.  tick and period both 0 is the
// historical cancel form and clears the tag outright.  If has_tag is false,
// it's stored anonymously (round-robin in a small reserved pool) and can't
// be addressed or cancelled by any tag.  Takes ownership of wire.
uint8_t sequencer_add_wire(uint32_t tick, uint32_t period, uint32_t tag, bool has_tag, char *wire);
// Drop every event stored under `tag`.  Since adds accumulate, this is how a
// tag is taken back as a whole; on the wire and from amy_add_event() it is
// spelled ticks="0,0,<tag>", which routes here through sequencer_add_wire().
void sequencer_clear_tag(uint32_t tag);

// LOCAL EDIT (S3-Amysynth): periodic-entry horizon. While set, repeating
// entries do not fire at or after tick; one-shots still do. Clear to resume.
void sequencer_set_periodic_horizon(uint32_t tick);
void sequencer_clear_periodic_horizon(void);
// LOCAL EDIT (S3-Amysynth): periodic-entry start. While set, repeating entries
// do not fire before tick; one-shots still do.
void sequencer_set_periodic_start(uint32_t tick);
void sequencer_clear_periodic_start(void);
// LOCAL EDIT (S3-Amysynth): events stored under `tag` (0 if out of range).
// Takes amy_queue_lock; not from the render path.
uint16_t sequencer_tag_count(uint32_t tag);
void sequencer_midi_clock_tick();
void sequencer_midi_start();
void sequencer_midi_stop();
void sequencer_external_clock_disable();  // drop external-clock mode, resume internal clock

#endif
