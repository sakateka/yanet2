package pdump

import (
	"context"
	"encoding/binary"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
	"go.uber.org/zap/zaptest/observer"

	"github.com/yanet-platform/yanet2/modules/pdump/controlplane/pdumppb/v1"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
)

// Test_MaxMode_MatchesC verifies that the pure-Go mode bound matches the
// dataplane bitmap bound.
func Test_MaxMode_MatchesC(t *testing.T) {
	require.Equal(t, uint32(pdumppb.MaxMode), maxMode)
}

// stubSourceCapacity comfortably fits every test's small, non-wrapping
// pushes into a stubSource.
const stubSourceCapacity = 4096

// stubSource is a minimal cring.RecordSource whose write position a test
// advances directly, into a fixed backing array, with no wraparound and
// no real ring frame behind it.
//
// The backing array is allocated once, on the first push, and never
// reassigned again: a concurrent reader's CopyRange then never races
// with a later push growing or replacing it, the same way the real ring
// stays race-free through its write position alone. Its readable
// position never moves: readable marks the oldest position a record was
// evicted up to, not the newest write, and this stub never evicts. The
// real framing and wraparound rules are covered in
// objects/ring/bindings/go/cring.
type stubSource struct {
	data  []byte
	write atomic.Uint64
}

func (m *stubSource) Indices() (write, readable uint64) {
	return m.write.Load(), 0
}

func (m *stubSource) CopyRange(dst []byte, start, size uint64) {
	copy(dst, m.data[start:start+size])
}

// push appends a frame-aligned chunk at the current write position and
// publishes it.
//
// Only the test goroutine ever calls push, so the lazy allocation below
// never races with itself.
func (m *stubSource) push(chunk []byte) {
	if m.data == nil {
		m.data = make([]byte, stubSourceCapacity)
	}

	start := m.write.Load()
	if start+uint64(len(chunk)) > uint64(len(m.data)) {
		panic("stubSource: push exceeded stubSourceCapacity; this stub does not wrap")
	}
	copy(m.data[start:], chunk)
	m.write.Add(uint64(len(chunk)))
}

// signalingSource wraps a stubSource and closes a channel the moment
// Indices has been called for the first time, after it already read the
// current value.
//
// A test waits on that channel to know exactly when a tail reader has
// captured its starting position, so a write issued right after is
// guaranteed to land after that position, never folded into it.
type signalingSource struct {
	*stubSource
	called chan struct{}
	once   sync.Once
}

func newSignalingSource() *signalingSource {
	return &signalingSource{stubSource: &stubSource{}, called: make(chan struct{})}
}

func (m *signalingSource) Indices() (write, readable uint64) {
	write, readable = m.stubSource.Indices()
	m.once.Do(func() { close(m.called) })
	return write, readable
}

// waitStarted waits for a tail reader to have read this source's starting
// position.
func (m *signalingSource) waitStarted(t *testing.T) {
	t.Helper()
	select {
	case <-m.called:
	case <-time.After(time.Second):
		t.Fatal("the reader never started reading its source")
	}
}

// newRecordFrame builds the ring's own 8-byte frame around the given
// metadata and payload, aligned to 4 bytes like the real writer.
func newRecordFrame(meta, data []byte, seqno uint32) []byte {
	totalLen := uint32(cring.RecordFrameSize) + uint32(len(meta)) + uint32(len(data))
	aligned := (totalLen + 3) &^ 3
	buf := make([]byte, aligned)
	binary.LittleEndian.PutUint32(buf[0:4], totalLen)
	binary.LittleEndian.PutUint32(buf[4:8], seqno)
	copy(buf[8:], meta)
	copy(buf[8+len(meta):], data)
	return buf
}

// newRecordMeta builds the 32-byte pdump metadata block
// (modules/pdump/dataplane/record.h) with a valid magic.
func newRecordMeta(packetLen, workerIdx uint32) []byte {
	buf := make([]byte, pdumpRecordHdrSize)
	binary.LittleEndian.PutUint32(buf[0:4], pdumpRecordMagic)
	binary.LittleEndian.PutUint32(buf[4:8], packetLen)
	binary.LittleEndian.PutUint32(buf[16:20], workerIdx)
	return buf
}

// Test_Ring_RecordOverhead_MatchesFrameAndHeader verifies that the fit
// check's overhead is exactly the ring frame plus the pdump metadata
// block, 8 plus 32 bytes, not a value that silently drifted from either.
func Test_Ring_RecordOverhead_MatchesFrameAndHeader(t *testing.T) {
	require.Equal(t, uint64(40), recordOverhead)
	require.Equal(t, uint64(cring.RecordFrameSize)+uint64(pdumpRecordHdrSize), recordOverhead)
}

// Test_Ring_ParseRecordMeta_ValidRecord verifies that a well-formed record
// decodes every fixed field, including the queue bitmap's drop bit, and
// keeps the data behind the metadata block.
func Test_Ring_ParseRecordMeta_ValidRecord(t *testing.T) {
	meta := newRecordMeta(9000, 3)
	binary.LittleEndian.PutUint64(meta[8:16], 123456789)
	binary.LittleEndian.PutUint32(meta[20:24], 7)
	binary.LittleEndian.PutUint16(meta[24:26], 11)
	binary.LittleEndian.PutUint16(meta[26:28], 22)
	meta[28] = pdumppb.MaxMode // the drop bit plus every other mode bit

	payload := append(meta, []byte("hello")...)

	got, data, ok := parseRecordMeta(payload)
	require.True(t, ok)
	want := &pdumppb.RecordMeta{
		Timestamp:   123456789,
		DataSize:    uint32(len("hello")),
		PacketLen:   9000,
		WorkerIdx:   3,
		PipelineIdx: 7,
		RxDeviceId:  11,
		TxDeviceId:  22,
		Queue:       uint32(pdumppb.MaxMode),
	}
	require.Equal(t, want, got)
	require.Equal(t, []byte("hello"), data)
}

// Test_Ring_ParseRecordMeta_OffsetsMatchC verifies that the Go decoder
// reads every field at the same byte offset as struct pdump_record_hdr.
func Test_Ring_ParseRecordMeta_OffsetsMatchC(t *testing.T) {
	require.Equal(t, uintptr(0), pdumpRecordHdrMagicOffset)
	require.Equal(t, uintptr(4), pdumpRecordHdrPacketLenOffset)
	require.Equal(t, uintptr(8), pdumpRecordHdrTimestampOffset)
	require.Equal(t, uintptr(16), pdumpRecordHdrWorkerIdxOffset)
	require.Equal(t, uintptr(20), pdumpRecordHdrPipelineIdxOffset)
	require.Equal(t, uintptr(24), pdumpRecordHdrRxDeviceIDOffset)
	require.Equal(t, uintptr(26), pdumpRecordHdrTxDeviceIDOffset)
	require.Equal(t, uintptr(28), pdumpRecordHdrQueueOffset)
	require.Equal(t, uintptr(32), uintptr(pdumpRecordHdrSize))
}

// Test_Ring_ParseRecordMeta_ShortRecord verifies that a payload shorter
// than the metadata block is dropped rather than read out of bounds.
func Test_Ring_ParseRecordMeta_ShortRecord(t *testing.T) {
	_, _, ok := parseRecordMeta(make([]byte, pdumpRecordHdrSize-1))
	require.False(t, ok)
}

// Test_Ring_ParseRecordMeta_BadMagic verifies that a metadata block whose
// magic does not match is dropped even though its length is otherwise
// valid.
func Test_Ring_ParseRecordMeta_BadMagic(t *testing.T) {
	payload := newRecordMeta(1, 0)
	payload[0] ^= 0xFF

	_, _, ok := parseRecordMeta(payload)
	require.False(t, ok)
}

// Test_Ring_RunReaders_TagsEachWorker verifies that runReaders gives each
// source its own reader: a worker's record carries its own metadata,
// independently of the other worker's.
func Test_Ring_RunReaders_TagsEachWorker(t *testing.T) {
	sigs := []*signalingSource{newSignalingSource(), newSignalingSource()}
	sources := make([]cring.RecordSource, len(sigs))
	for idx, sig := range sigs {
		sources[idx] = sig
	}

	ctx, cancel := context.WithCancel(t.Context())
	recordCh := make(chan *pdumppb.Record, len(sources))
	done := make(chan error, 1)
	go func() { done <- runReaders(ctx, sources, 64, zap.NewNop(), recordCh) }()

	for _, sig := range sigs {
		sig.waitStarted(t)
	}
	for idx, sig := range sigs {
		sig.push(newRecordFrame(newRecordMeta(1, uint32(idx)), []byte{byte(idx)}, 0))
	}

	got := map[uint32][]byte{}
	for range sources {
		rec := <-recordCh
		got[rec.GetMeta().GetWorkerIdx()] = rec.GetData()
	}
	require.Equal(t, map[uint32][]byte{0: {0}, 1: {1}}, got)

	cancel()
	require.ErrorIs(t, <-done, context.Canceled)
}

// Test_Ring_RunReaders_StartsAtTailSkipsHistory verifies that a session
// starts at the source's current write position: a record committed
// before the session starts is never delivered, while one committed
// after it is.
func Test_Ring_RunReaders_StartsAtTailSkipsHistory(t *testing.T) {
	sig := newSignalingSource()
	sig.push(newRecordFrame(newRecordMeta(1, 0), []byte("before"), 0))

	ctx, cancel := context.WithCancel(t.Context())
	recordCh := make(chan *pdumppb.Record, 4)
	done := make(chan error, 1)
	go func() { done <- runReaders(ctx, []cring.RecordSource{sig}, 64, zap.NewNop(), recordCh) }()

	sig.waitStarted(t)
	sig.push(newRecordFrame(newRecordMeta(1, 0), []byte("after"), 0))

	rec := <-recordCh
	require.Equal(t, []byte("after"), rec.GetData(), "a record written before the session started must never arrive")

	cancel()
	require.ErrorIs(t, <-done, context.Canceled)
}

// Test_Ring_RunReaders_DropsMalformedRecordAndContinues verifies that
// malformed records are dropped without ending the session, and the
// valid record after them still arrives.
//
// Three drops must log only one first-drop warning plus one final
// count, not one line per record.
func Test_Ring_RunReaders_DropsMalformedRecordAndContinues(t *testing.T) {
	sig := newSignalingSource()

	ctx, cancel := context.WithCancel(t.Context())
	recordCh := make(chan *pdumppb.Record, 4)
	done := make(chan error, 1)
	core, logs := observer.New(zapcore.WarnLevel)
	go func() { done <- runReaders(ctx, []cring.RecordSource{sig}, 64, zap.New(core), recordCh) }()

	sig.waitStarted(t)

	badMeta := newRecordMeta(1, 0)
	badMeta[0] ^= 0xFF // corrupt the magic
	for range 3 {
		sig.push(newRecordFrame(badMeta, nil, 0))
	}
	sig.push(newRecordFrame(newRecordMeta(1, 0), []byte("ok"), 0))

	rec := <-recordCh
	require.Equal(t, []byte("ok"), rec.GetData(), "the valid record after the malformed ones must still arrive")

	cancel()
	require.ErrorIs(t, <-done, context.Canceled)

	require.Equal(t, 1, logs.FilterMessage("dropped the first malformed pdump record in this session").Len(),
		"three malformed records must log the first-drop warning only once")
	require.Equal(t, 1, logs.FilterMessage("dropped malformed pdump records in this session").Len(),
		"the session must log its final drop count exactly once, when it stops")
}

// raceSourcePlainWrite is a RecordSource whose write position is a plain,
// unsynchronized field: a stand-in for memory a caller may reuse or free
// the moment a read session returns.
//
// Its CopyRange never has anything to copy, since no test using it ever
// reads a whole record; it exists only to let the waker poll Indices.
type raceSourcePlainWrite struct {
	write uint64
}

func (m *raceSourcePlainWrite) Indices() (write, readable uint64) {
	return m.write, 0
}

func (m *raceSourcePlainWrite) CopyRange(dst []byte, start, size uint64) {}

// Test_Ring_RunReaders_StopsWakerBeforeReturning pins that runReaders does
// not return until its waker has stopped touching the sources.
//
// The waker polls a source's indices on its own schedule, independent of
// the readers; a caller that reuses or frees that memory right after
// runReaders returns must never still race with it. This source's write
// position carries no synchronization of its own, so a waker still
// running when runReaders returned would race, under the race detector,
// with the plain write below. Repeated runs (see the gate's -count) make
// the window this pins reliably observable.
func Test_Ring_RunReaders_StopsWakerBeforeReturning(t *testing.T) {
	src := &raceSourcePlainWrite{}

	ctx, cancel := context.WithCancel(t.Context())
	done := make(chan error, 1)
	go func() {
		done <- runReaders(ctx, []cring.RecordSource{src}, 64, zap.NewNop(), make(chan *pdumppb.Record, 1))
	}()

	cancel()
	require.ErrorIs(t, <-done, context.Canceled)

	src.write = 1
}

// controlledInterval is a handshake a test attaches to a context with
// withWakerInterval.
//
// The waker reports the interval it just computed and then blocks until
// the test releases it, instead of sleeping, so a test drives the
// backoff loop one poll at a time with no real time passing, and may
// act on the ring while a poll is held, before it runs. Being carried on
// a context rather than a package variable keeps it scoped to the one
// spawnWakers call the test starts: a goroutine a previous,
// already-returned test leaked keeps using that test's own, unmodified
// context, so two tests' wakers never interfere.
type controlledInterval struct {
	reported chan time.Duration
	proceed  chan struct{}
}

// hookWakerInterval returns a context wired to a fresh controlledInterval.
func hookWakerInterval(t *testing.T, ctx context.Context) (context.Context, *controlledInterval) {
	t.Helper()

	c := &controlledInterval{reported: make(chan time.Duration), proceed: make(chan struct{})}
	wait := func(ctx context.Context, interval time.Duration) bool {
		select {
		case c.reported <- interval:
		case <-ctx.Done():
			return false
		}
		select {
		case <-c.proceed:
			return true
		case <-ctx.Done():
			return false
		}
	}
	return withWakerInterval(ctx, wait), c
}

// hold waits for the next poll's interval without releasing it yet, so
// the caller may act while the waker is blocked right before that poll.
func (m *controlledInterval) hold(t *testing.T) time.Duration {
	t.Helper()
	select {
	case d := <-m.reported:
		return d
	case <-time.After(time.Second):
		t.Fatal("the waker did not reach its next poll in time")
		return 0
	}
}

// release lets a held poll proceed.
func (m *controlledInterval) release() {
	m.proceed <- struct{}{}
}

// next waits for the next poll's interval and releases it, returning the
// interval.
func (m *controlledInterval) next(t *testing.T) time.Duration {
	t.Helper()
	d := m.hold(t)
	m.release()
	return d
}

// Test_Ring_SpawnWakers_BusyReaderBacksOff verifies that a reader with
// unread data it never drains does not keep the waker polling at its
// fastest rate.
//
// A wake-up still sitting unconsumed is not a fresh one: without this, a
// slow client that never catches up would force the waker to poll every
// source at wakerStartInterval forever.
func Test_Ring_SpawnWakers_BusyReaderBacksOff(t *testing.T) {
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	ctx, ctrl := hookWakerInterval(t, ctx)

	stub := &stubSource{}
	stub.push(newRecordFrame(newRecordMeta(1, 0), nil, 0))
	reader, err := cring.NewReader(0, 64, stub)
	require.NoError(t, err)

	_, done := spawnWakers(ctx, []*cring.Reader{reader})

	require.Equal(t, wakerStartInterval, ctrl.next(t), "the first poll delivers the pending record")

	// Never read the wake-up channel or the reader itself: the one
	// notification already sent is never drained, so every later poll
	// finds the reader still busy, not newly idle, and must keep backing
	// off instead of resetting.
	var last time.Duration
	for range 20 {
		last = ctrl.next(t)
		require.GreaterOrEqual(t, last, wakerStartInterval)
	}
	require.Equal(t, wakerMaxInterval, last, "a perpetually busy reader must reach and stay at the cap")

	cancel()
	<-done
}

// Test_Ring_SpawnWakers_ResetsToStartIntervalAfterWake verifies that the
// interval returns to wakerStartInterval as soon as a poll wakes an idle
// reader.
//
// This follows a stretch of backing off while there was nothing to
// report.
func Test_Ring_SpawnWakers_ResetsToStartIntervalAfterWake(t *testing.T) {
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	ctx, ctrl := hookWakerInterval(t, ctx)

	stub := &stubSource{}
	reader, err := cring.NewReader(0, 64, stub)
	require.NoError(t, err)

	wakers, done := spawnWakers(ctx, []*cring.Reader{reader})

	// Back off for a few idle polls.
	var backedOff time.Duration
	for range 3 {
		backedOff = ctrl.next(t)
	}
	require.Greater(t, backedOff, wakerStartInterval, "the interval must have grown while idle")

	// The waker is now held right before its next poll: publish here, so
	// that poll is the first one to see the new data.
	held := ctrl.hold(t)
	require.Greater(t, held, wakerStartInterval)
	stub.push(newRecordFrame(newRecordMeta(1, 0), nil, 0))
	ctrl.release()

	require.Equal(t, wakerStartInterval, ctrl.next(t), "a poll that wakes an idle reader resets the interval")

	cancel()
	<-done
	<-wakers[0] // the record published above was delivered to the waker
}

// Test_Ring_SpawnWakers_NotifiesOnlyAfterDataArrives verifies that the
// waker stays quiet on an empty ring, wakes the right reader once data is
// published, and stops once its context ends.
func Test_Ring_SpawnWakers_NotifiesOnlyAfterDataArrives(t *testing.T) {
	src := &stubSource{}
	reader, err := cring.NewReader(0, 64, src)
	require.NoError(t, err)

	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	wakers, done := spawnWakers(ctx, []*cring.Reader{reader})

	select {
	case <-wakers[0]:
		t.Fatal("waker must not notify before any data exists")
	case <-time.After(20 * time.Millisecond):
	}

	src.push(newRecordFrame(newRecordMeta(1, 0), nil, 0))

	select {
	case <-wakers[0]:
	case <-time.After(time.Second):
		t.Fatal("waker did not notify after data arrived")
	}

	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("waker goroutine did not stop")
	}
}
