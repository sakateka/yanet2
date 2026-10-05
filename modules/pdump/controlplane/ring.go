package pdump

import (
	"context"
	"encoding/binary"
	"time"

	"go.uber.org/zap"
	"golang.org/x/sync/errgroup"

	"github.com/yanet-platform/yanet2/modules/pdump/controlplane/pdumppb/v1"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
)

// recordOverhead is the ring's own 8-byte frame plus the fixed 32-byte
// pdump metadata block every record carries behind it.
//
// A bind-time fit check compares it plus the configured snaplen against
// the ring's largest record.
const recordOverhead = uint64(cring.RecordFrameSize) + uint64(pdumpRecordHdrSize)

// readChunkBytes bounds how many bytes a single read pulls from one
// worker's ring before its records are sent on.
//
// So one very busy worker cannot starve the others for long.
var readChunkBytes = defaultSnaplen * 32

// effectiveSnaplen is the snaplen a config captures at: the configured
// value, or the default when it is zero.
//
// It mirrors pdump_module_config_set_snaplen's own zero handling
// (modules/pdump/api/controlplane.c), so the fit check and the dataplane
// agree on what a config actually captures.
func effectiveSnaplen(snaplen uint32) uint32 {
	if snaplen == 0 {
		return defaultSnaplen
	}
	return snaplen
}

// wakerStartInterval is the waker's poll interval right after any reader
// found data, and what a poll that finds data resets it to.
const wakerStartInterval = 100 * time.Microsecond

// wakerMaxInterval caps the exponential backoff an empty poll grows the
// waker's interval to, so an idle capture polls at most this often.
const wakerMaxInterval = 10 * time.Millisecond

// wakerIntervalKey is the context key a test uses to replace the wait
// spawnWakers does between polls.
//
// A context value, not a package variable, keeps the override scoped to
// the one session that carries it: a goroutine a previous, already
// returned test leaked still uses that test's own, unmodified context.
type wakerIntervalKey struct{}

// withWakerInterval returns a context that makes spawnWakers call wait
// instead of sleeping between polls, so a test can drive the backoff
// loop deterministically, one poll at a time, with no real time passing.
func withWakerInterval(ctx context.Context, wait func(context.Context, time.Duration) bool) context.Context {
	return context.WithValue(ctx, wakerIntervalKey{}, wait)
}

// wakerIntervalFunc returns the wait function spawnWakers should use for
// ctx: the one withWakerInterval attached, or the real clock.
func wakerIntervalFunc(ctx context.Context) func(context.Context, time.Duration) bool {
	if wait, ok := ctx.Value(wakerIntervalKey{}).(func(context.Context, time.Duration) bool); ok {
		return wait
	}
	return waitWakerInterval
}

// waitWakerInterval waits out one poll's computed interval, or returns
// early once ctx ends.
//
// It reports whether the interval elapsed, as opposed to ctx ending
// first.
func waitWakerInterval(ctx context.Context, interval time.Duration) bool {
	select {
	case <-ctx.Done():
		return false
	case <-time.After(interval):
		return true
	}
}

// parseRecordMeta decodes the 32-byte pdump metadata block
// (modules/pdump/dataplane/record.h) at the front of a record's payload.
//
// It reports false when the payload is shorter than the block or the
// block's magic does not match, so a caller drops the record instead of
// trusting bytes that are not really metadata. The returned data is the
// payload bytes behind the block.
func parseRecordMeta(payload []byte) (meta *pdumppb.RecordMeta, data []byte, ok bool) {
	if uint64(len(payload)) < uint64(pdumpRecordHdrSize) {
		return nil, nil, false
	}
	if binary.LittleEndian.Uint32(payload[0:4]) != pdumpRecordMagic {
		return nil, nil, false
	}

	data = payload[pdumpRecordHdrSize:]
	meta = &pdumppb.RecordMeta{
		PacketLen:   binary.LittleEndian.Uint32(payload[4:8]),
		Timestamp:   binary.LittleEndian.Uint64(payload[8:16]),
		WorkerIdx:   binary.LittleEndian.Uint32(payload[16:20]),
		PipelineIdx: binary.LittleEndian.Uint32(payload[20:24]),
		RxDeviceId:  uint32(binary.LittleEndian.Uint16(payload[24:26])),
		TxDeviceId:  uint32(binary.LittleEndian.Uint16(payload[26:28])),
		Queue:       uint32(payload[28]),
		DataSize:    uint32(len(data)),
	}
	return meta, data, true
}

// runReaders opens one reader per source and reads them until ctx is
// done, parsing whole records and sending them on recordCh.
//
// Each reader starts at its source's current write position, so a
// session sees only records committed after it started, never the
// ring's history. Each source gets its own reader and its own cursor, so
// one reader never affects another's. A record with a short or
// mismatched header is dropped, not treated as fatal, since the writer's
// own alignment keeps later records on frame boundaries; a session logs
// only its first such drop, plus a total when it stops, instead of one
// line per record.
func runReaders(
	ctx context.Context,
	sources []cring.RecordSource,
	capacity uint32,
	log *zap.Logger,
	recordCh chan<- *pdumppb.Record,
) error {
	readers := make([]*cring.Reader, 0, len(sources))
	for idx, src := range sources {
		reader, err := cring.NewReaderFromTail(uint16(idx), capacity, src)
		if err != nil {
			return err
		}
		readers = append(readers, reader)
	}

	wakers, wakersDone := spawnWakers(ctx, readers)
	wg, _ := errgroup.WithContext(ctx)
	for idx, reader := range readers {
		waker := wakers[idx]
		wg.Go(func() error {
			var malformed int
			defer func() {
				if malformed > 0 {
					log.Warn("dropped malformed pdump records in this session",
						zap.Int("worker", idx), zap.Int("count", malformed))
				}
			}()

			for {
				for _, rec := range reader.Read(readChunkBytes) {
					meta, data, ok := parseRecordMeta(rec.Bytes)
					if !ok {
						if malformed == 0 {
							log.Warn("dropped the first malformed pdump record in this session",
								zap.Int("worker", idx), zap.Int("len", len(rec.Bytes)))
						}
						malformed++
						continue
					}
					select {
					case <-ctx.Done():
						return ctx.Err()
					case recordCh <- &pdumppb.Record{Meta: meta, Data: data}:
					}
				}
				if reader.HasMore() {
					continue
				}
				select {
				case <-ctx.Done():
					return ctx.Err()
				case <-waker:
				}
			}
		})
	}
	err := wg.Wait()
	// The waker reads the rings too, so it must stop before this returns.
	<-wakersDone
	return err
}

// spawnWakers starts a background goroutine that polls every reader's
// source for new data and notifies the matching channel.
//
// It polls at wakerStartInterval right after a poll actually wakes an
// idle reader, and doubles its interval otherwise, up to wakerMaxInterval.
// A reader that already has an unconsumed wake-up pending does not reset
// the interval: it is busy, not idle, and unread data sitting behind a
// slow client must not keep the waker spinning at its fastest rate
// forever. The returned done channel closes once that goroutine has
// returned; since the goroutine reads the rings too, a caller waits for
// it after its own readers have stopped.
func spawnWakers(ctx context.Context, readers []*cring.Reader) ([]chan struct{}, <-chan struct{}) {
	wakers := make([]chan struct{}, len(readers))
	for idx := range wakers {
		wakers[idx] = make(chan struct{}, 1)
	}

	wait := wakerIntervalFunc(ctx)

	done := make(chan struct{})
	go func() {
		defer close(done)

		interval := wakerStartInterval
		for {
			woke := false
			for idx, reader := range readers {
				if !reader.HasMore() {
					continue
				}
				select {
				case wakers[idx] <- struct{}{}:
					woke = true
				default: // the reader has not drained the last wake-up yet
				}
			}

			if woke {
				interval = wakerStartInterval
			} else {
				interval *= 2
				if interval > wakerMaxInterval {
					interval = wakerMaxInterval
				}
			}

			if !wait(ctx, interval) {
				return
			}
		}
	}()

	return wakers, done
}
