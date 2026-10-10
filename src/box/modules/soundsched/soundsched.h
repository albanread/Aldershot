/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a translation into C of RISC OS Open's Sound scheduler
 * (Sources/HWSupport/Sound/Sound2: s.Sound2).
 */

/* soundsched.h: the sound queue (Sound2's role), native. */
#ifndef ROSGD_SOUNDSCHED_H
#define ROSGD_SOUNDSCHED_H

#include "rosgd/module.h"

extern struct ros_module soundsched_module;

/* Advances the queue by the given centiseconds and fires what comes due.
 * The Sound module's fill calls this for every buffer. On RISC OS the
 * DMA interrupt's dispatch does this job. It is safe to call from any
 * thread, because a spinlock holds the queue while it works. */
void ros_soundq_tick(unsigned centisecs);

/* The queue's current beat, as BASIC's BEAT reads it (Sound_QBeat 0). */
uint32_t ros_soundq_beat(void);

/* The queue as it stands, for the test harness (platform/agent_vsock.c's
 * "sound"). It holds these values:
 *   beat: the queue's clock, the beats counted since Sound_QInit.
 *   qbeat: the beat counter (BASIC's BEAT).
 *   bar: the bar length.
 *   tempo: the tempo (&1000 is a beat a centisecond).
 *   depth: the events waiting.
 *   scheduled, fired_notes, fired_other: the counts of events queued and
 *     fired.
 *   bar_events: the count of bars come round (Event_Sound).
 * It is read without the lock, so it is a snapshot with each word whole. */
struct ros_soundq_state {
    uint32_t beat, qbeat, bar, tempo, depth;
    uint32_t scheduled, fired_notes, fired_other;
    uint32_t bar_events;                /* Event_Sound raised, a bar each */
};
void ros_soundq_state(struct ros_soundq_state *st);

#endif
