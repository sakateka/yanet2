package ringtest

//#include <pthread.h>
//#include <stdatomic.h>
//#include <stdlib.h>
//#include <string.h>
//
//#include "objects/ring/api/ring_object.h"
//
//// ring_stress is one C writer thread committing records to a worker's
//// ring at full speed, independent of the Go scheduler.
//struct ring_stress {
//	struct ring_worker *worker;
//	uint8_t *data;
//	uint64_t records;
//	// Fixed payload of every record, or NULL to derive each from its seqno.
//	uint8_t *fixed;
//	uint32_t fixed_len;
//	_Atomic uint64_t written;
//	_Atomic int done;
//	pthread_t thread;
//};
//
//// ring_stress_len is the payload length for a seqno: 4..256 bytes, in
//// whole 32-bit words.
//static inline uint32_t
//ring_stress_len(uint32_t seqno) {
//	return 4 * (1 + ((seqno * 2654435761u) >> 26));
//}
//
//// ring_stress_word is payload word k of the record stamped with seqno.
//static inline uint32_t
//ring_stress_word(uint32_t seqno, uint32_t k) {
//	return seqno * 0x9E3779B1u + k;
//}
//
//static void *
//ring_stress_run(void *arg) {
//	struct ring_stress *s = arg;
//	uint32_t buf[64];
//
//	for (uint64_t idx = 0; idx < s->records; idx++) {
//		if (s->fixed != NULL) {
//			uint32_t total = RING_RECORD_FRAME_SIZE + s->fixed_len;
//			if (ring_worker_prepare(s->worker, s->data, total) != 0) {
//				break;
//			}
//			ring_worker_write(
//				s->worker, s->data, RING_RECORD_FRAME_SIZE,
//				s->fixed, s->fixed_len
//			);
//			ring_worker_commit(s->worker, s->data, total);
//			atomic_store_explicit(&s->written, idx + 1, memory_order_relaxed);
//			continue;
//		}
//		uint32_t seqno = s->worker->local.next_seqno;
//		uint32_t len = ring_stress_len(seqno);
//		for (uint32_t k = 0; k < len / 4; k++) {
//			buf[k] = ring_stress_word(seqno, k);
//		}
//		uint32_t total = RING_RECORD_FRAME_SIZE + len;
//		if (ring_worker_prepare(s->worker, s->data, total) != 0) {
//			break;
//		}
//		ring_worker_write(
//			s->worker, s->data, RING_RECORD_FRAME_SIZE,
//			(const uint8_t *)buf, len
//		);
//		ring_worker_commit(s->worker, s->data, total);
//		atomic_store_explicit(&s->written, idx + 1, memory_order_relaxed);
//	}
//	atomic_store(&s->done, 1);
//	return NULL;
//}
//
//// ring_stress_start starts the writer; a nonzero fixed length makes every
//// payload that many 0xAB bytes instead of the seqno-derived pattern.
//static struct ring_stress *
//ring_stress_start(
//	struct ring_worker *worker, uint8_t *data, uint64_t records,
//	uint32_t fixed_len
//) {
//	struct ring_stress *s = calloc(1, sizeof(*s));
//	if (s == NULL) {
//		return NULL;
//	}
//	s->worker = worker;
//	s->data = data;
//	s->records = records;
//	if (fixed_len != 0) {
//		s->fixed = malloc(fixed_len);
//		if (s->fixed == NULL) {
//			free(s);
//			return NULL;
//		}
//		memset(s->fixed, 0xAB, fixed_len);
//		s->fixed_len = fixed_len;
//	}
//	if (pthread_create(&s->thread, NULL, ring_stress_run, s) != 0) {
//		free(s->fixed);
//		free(s);
//		return NULL;
//	}
//	return s;
//}
//
//static int
//ring_stress_done(struct ring_stress *s) {
//	return atomic_load(&s->done);
//}
//
//static uint64_t
//ring_stress_written(struct ring_stress *s) {
//	return atomic_load(&s->written);
//}
//
//static void
//ring_stress_join(struct ring_stress *s) {
//	pthread_join(s->thread, NULL);
//	free(s->fixed);
//	free(s);
//}
import "C"

import (
	"encoding/binary"
	"errors"
)

// Stress runs the C writer on its own OS thread at full speed; unless fixed,
// each record's length and bytes derive from its seqno, so a reader can spot
// a torn one.
type Stress struct {
	s *C.struct_ring_stress
}

// StartStress starts committing records to the writer's worker ring. The
// writer stops early if a record is ever refused.
func (m *Writer) StartStress(records uint64) (*Stress, error) {
	return m.startStress(records, 0)
}

// StartStressFixed is StartStress with every payload the same given number
// of bytes, for a reader benchmark at a fixed record size; such records do
// not pass StressRecordValid.
func (m *Writer) StartStressFixed(records uint64, payloadLen uint32) (*Stress, error) {
	if payloadLen == 0 {
		return nil, errors.New("fixed payload length must be nonzero")
	}
	return m.startStress(records, payloadLen)
}

func (m *Writer) startStress(records uint64, fixedLen uint32) (*Stress, error) {
	s := C.ring_stress_start(m.worker, m.data, C.uint64_t(records), C.uint32_t(fixedLen))
	if s == nil {
		return nil, errors.New("failed to start the stress writer thread")
	}
	return &Stress{s: s}, nil
}

// Done reports whether the writer thread has finished.
func (m *Stress) Done() bool {
	return C.ring_stress_done(m.s) != 0
}

// Written returns the number of committed records.
func (m *Stress) Written() uint64 {
	return uint64(C.ring_stress_written(m.s))
}

// Wait joins the writer thread and releases its state.
func (m *Stress) Wait() {
	C.ring_stress_join(m.s)
	m.s = nil
}

// StressRecordValid reports whether a payload is exactly the record the
// stress writer stamped with the given sequence number.
func StressRecordValid(seqno uint32, payload []byte) bool {
	if uint32(len(payload)) != uint32(C.ring_stress_len(C.uint32_t(seqno))) {
		return false
	}
	for k := 0; k+4 <= len(payload); k += 4 {
		want := uint32(C.ring_stress_word(C.uint32_t(seqno), C.uint32_t(k/4)))
		if binary.LittleEndian.Uint32(payload[k:k+4]) != want {
			return false
		}
	}
	return true
}
