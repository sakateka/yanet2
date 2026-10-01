package ringpb

import (
	"fmt"
	"math"
	"strings"
)

// MaxRingNameLen is the C object-name buffer size, including the terminating
// NUL; the longest accepted name is one byte shorter.
const MaxRingNameLen = 80

// MinRingCapacity is the smallest per-worker capacity a ring accepts, in
// bytes: room for exactly one record frame.
const MinRingCapacity = 8

// DefaultPublishBatch is the publish batch a ring gets when the request
// leaves it unset: the records a writer commits before publishing them on
// its own.
const DefaultPublishBatch = 8

// MaxPublishBatch is the largest publish batch a ring accepts.
const MaxPublishBatch = 1024

// ValidateRingName validates a ring name against the C object-name
// contract, reporting the given proto field on failure.
func ValidateRingName(field, name string) error {
	if name == "" {
		return fmt.Errorf("%s is required", field)
	}
	if strings.IndexByte(name, 0) != -1 {
		return fmt.Errorf("%s must not contain NUL", field)
	}
	if len(name) >= MaxRingNameLen {
		return fmt.Errorf("%s must be shorter than %d bytes", field, MaxRingNameLen)
	}
	return nil
}

// Validate checks the ring name, the per-worker capacity and the publish
// batch, which may be left unset for the default.
//
// The upper bound is the C layer's, which varies by build; the service maps
// its rejection of an oversized capacity to InvalidArgument. A capacity
// beyond 32 bits is rejected here, since truncating it for the C API would
// silently create a ring far smaller than requested.
func (m *CreateRingRequest) Validate() error {
	if err := ValidateRingName("name", m.GetName()); err != nil {
		return err
	}

	capacity := m.GetCapacity()
	if capacity > math.MaxUint32 {
		return fmt.Errorf(
			"capacity %d exceeds the maximum representable value %d",
			capacity, uint32(math.MaxUint32),
		)
	}
	if capacity&(capacity-1) != 0 {
		return fmt.Errorf("capacity %d must be a power of two", capacity)
	}
	if capacity < MinRingCapacity {
		return fmt.Errorf(
			"capacity %d must be at least %d",
			capacity, MinRingCapacity,
		)
	}
	if batch := m.GetPublishBatch(); batch > MaxPublishBatch {
		return fmt.Errorf(
			"publish_batch %d must be at most %d records",
			batch, MaxPublishBatch,
		)
	}
	return nil
}

// PublishBatchOrDefault returns the requested publish batch, or
// DefaultPublishBatch when the request leaves it unset.
func (m *CreateRingRequest) PublishBatchOrDefault() uint32 {
	if batch := m.GetPublishBatch(); batch != 0 {
		return batch
	}
	return DefaultPublishBatch
}

// Validate checks that the request names the ring to describe.
func (m *ShowRingRequest) Validate() error {
	return ValidateRingName("name", m.GetName())
}

// Validate checks that the request names the ring to delete.
func (m *DeleteRingRequest) Validate() error {
	return ValidateRingName("name", m.GetName())
}
