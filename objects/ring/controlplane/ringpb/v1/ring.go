package ringpb

import (
	"fmt"
	"math"
	"strings"
)

// MaxRingNameLen is the C object-name buffer size, including the
// terminating NUL. The longest accepted name is one byte shorter than this
// bound.
const MaxRingNameLen = 80

// MinRingCapacity is the smallest per-worker capacity a ring accepts, in
// bytes: room for exactly one record frame.
const MinRingCapacity = 8

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

// Validate checks the ring name and the per-worker capacity.
//
// It does not enforce a business upper bound: the C maximum shrinks under
// ASan builds, so no Go literal can stay in parity with it. The create
// path on the service maps the C rejection of an oversized capacity to
// InvalidArgument. It does reject a capacity that cannot survive the
// truncation to the uint32 the C API takes, since accepting one here would
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
	return nil
}

// Validate checks that the request names the ring to describe.
func (m *ShowRingRequest) Validate() error {
	return ValidateRingName("name", m.GetName())
}

// Validate checks that the request names the ring to delete.
func (m *DeleteRingRequest) Validate() error {
	return ValidateRingName("name", m.GetName())
}
