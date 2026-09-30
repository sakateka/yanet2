package ringpb_test

import (
	"strings"
	"testing"

	"github.com/stretchr/testify/require"

	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
)

// Test_ValidateRingName verifies that names outside the C object-name
// contract are rejected with exact plain-error text, at the caller's field.
func Test_ValidateRingName(t *testing.T) {
	cases := []struct {
		name     string
		field    string
		ringName string
		message  string
	}{
		{name: "empty name", field: "name", message: "name is required"},
		{
			name:     "name contains NUL",
			field:    "name",
			ringName: "a\x00b",
			message:  "name must not contain NUL",
		},
		{
			name:     "name at byte limit",
			field:    "name",
			ringName: strings.Repeat("a", ringpb.MaxRingNameLen),
			message:  "name must be shorter than 80 bytes",
		},
		{
			name:     "name beyond byte limit",
			field:    "name",
			ringName: strings.Repeat("a", ringpb.MaxRingNameLen+120),
			message:  "name must be shorter than 80 bytes",
		},
		{
			name:     "longest accepted name",
			field:    "name",
			ringName: strings.Repeat("a", ringpb.MaxRingNameLen-1),
		},
		{name: "ordinary name", field: "name", ringName: "ring0"},
		{
			name:    "reports the caller's field",
			field:   "ring_name",
			message: "ring_name is required",
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := ringpb.ValidateRingName(tc.field, tc.ringName)
			if tc.message == "" {
				require.NoError(t, err)
				return
			}
			require.EqualError(t, err, tc.message)
		})
	}
}

// Test_CreateRingRequest_Validate verifies that the ring name and the
// per-worker capacity are validated, and that no upper bound is enforced on
// capacity since the C maximum has no stable Go-side literal.
func Test_CreateRingRequest_Validate(t *testing.T) {
	cases := []struct {
		name    string
		request *ringpb.CreateRingRequest
		message string
	}{
		{
			name:    "empty name",
			request: &ringpb.CreateRingRequest{Capacity: 8},
			message: "name is required",
		},
		{
			name: "name contains NUL",
			request: &ringpb.CreateRingRequest{
				Name:     "a\x00b",
				Capacity: 8,
			},
			message: "name must not contain NUL",
		},
		{
			name: "name at byte limit",
			request: &ringpb.CreateRingRequest{
				Name:     strings.Repeat("a", 80),
				Capacity: 8,
			},
			message: "name must be shorter than 80 bytes",
		},
		{
			name:    "zero capacity",
			request: &ringpb.CreateRingRequest{Name: "ring0"},
			message: "capacity 0 must be at least 8",
		},
		{
			name:    "capacity beyond uint32 range",
			request: &ringpb.CreateRingRequest{Name: "ring0", Capacity: 1 << 32},
			message: "capacity 4294967296 exceeds the maximum representable value 4294967295",
		},
		{
			name:    "capacity not a power of two",
			request: &ringpb.CreateRingRequest{Name: "ring0", Capacity: 12},
			message: "capacity 12 must be a power of two",
		},
		{
			name:    "capacity below one record frame",
			request: &ringpb.CreateRingRequest{Name: "ring0", Capacity: 4},
			message: "capacity 4 must be at least 8",
		},
		{
			name:    "capacity exactly one record frame",
			request: &ringpb.CreateRingRequest{Name: "ring0", Capacity: 8},
		},
		{
			name: "large power-of-two capacity accepted",
			request: &ringpb.CreateRingRequest{
				Name:     "ring0",
				Capacity: 1 << 30,
			},
		},
		{name: "nil request", request: nil, message: "name is required"},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := tc.request.Validate()
			if tc.message == "" {
				require.NoError(t, err)
				return
			}
			require.EqualError(t, err, tc.message)
		})
	}
}

// Test_ShowRingRequest_Validate verifies that the ring-name rule applies to
// show requests, including nil receivers.
func Test_ShowRingRequest_Validate(t *testing.T) {
	cases := []struct {
		name    string
		request *ringpb.ShowRingRequest
		message string
	}{
		{name: "empty name", request: &ringpb.ShowRingRequest{}, message: "name is required"},
		{
			name:    "name contains NUL",
			request: &ringpb.ShowRingRequest{Name: "a\x00b"},
			message: "name must not contain NUL",
		},
		{
			name:    "name at byte limit",
			request: &ringpb.ShowRingRequest{Name: strings.Repeat("a", 80)},
			message: "name must be shorter than 80 bytes",
		},
		{name: "valid name", request: &ringpb.ShowRingRequest{Name: "ring0"}},
		{name: "nil request", request: nil, message: "name is required"},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := tc.request.Validate()
			if tc.message == "" {
				require.NoError(t, err)
				return
			}
			require.EqualError(t, err, tc.message)
		})
	}
}

// Test_DeleteRingRequest_Validate verifies that the ring-name rule applies
// to delete requests, including nil receivers.
func Test_DeleteRingRequest_Validate(t *testing.T) {
	cases := []struct {
		name    string
		request *ringpb.DeleteRingRequest
		message string
	}{
		{name: "empty name", request: &ringpb.DeleteRingRequest{}, message: "name is required"},
		{
			name:    "name contains NUL",
			request: &ringpb.DeleteRingRequest{Name: "a\x00b"},
			message: "name must not contain NUL",
		},
		{
			name:    "name at byte limit",
			request: &ringpb.DeleteRingRequest{Name: strings.Repeat("a", 80)},
			message: "name must be shorter than 80 bytes",
		},
		{name: "valid name", request: &ringpb.DeleteRingRequest{Name: "ring0"}},
		{name: "nil request", request: nil, message: "name is required"},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := tc.request.Validate()
			if tc.message == "" {
				require.NoError(t, err)
				return
			}
			require.EqualError(t, err, tc.message)
		})
	}
}
