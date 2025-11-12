package fwstate

import (
	"github.com/c2h5oh/datasize"
)

// Config holds the configuration for the FwState module
type Config struct {
	// Endpoint is the gRPC endpoint for the FwState service
	Endpoint string `yaml:"endpoint"`

	// MemoryPath is the path to the shared memory file
	MemoryPath string `yaml:"memory_path"`

	// MemoryRequirements specifies the memory requirements for the module
	MemoryRequirements datasize.ByteSize `yaml:"memory_requirements"`
}

// DefaultConfig returns a default configuration for the FwState module
func DefaultConfig() *Config {
	return &Config{
		Endpoint:           "[::1]:50052",
		MemoryPath:         "/dev/hugepages/yanet",
		MemoryRequirements: 64 * datasize.MB,
	}
}

// Timeouts represents connection timeout configuration
type Timeouts struct {
	TCPSynAck uint64 // TCP SYN-ACK timeout in nanoseconds
	TCPSyn    uint64 // TCP SYN timeout in nanoseconds
	TCPFin    uint64 // TCP FIN timeout in nanoseconds
	TCP       uint64 // TCP established timeout in nanoseconds
	UDP       uint64 // UDP timeout in nanoseconds
	Default   uint64 // Default timeout for other protocols in nanoseconds
}

// MapConfig represents firewall state map configuration
type MapConfig struct {
	IndexSize        uint32 // Size of the hash table index
	ExtraBucketCount uint32 // Number of extra buckets for collision handling
}

// DefaultMapConfig returns default map configuration values
func DefaultMapConfig() MapConfig {
	return MapConfig{
		IndexSize:        1024 * 1024, // 1M entries
		ExtraBucketCount: 1024,        // 1024 extra buckets
	}
}

// SyncConfig represents firewall state synchronization configuration
type SyncConfig struct {
	SrcAddr          [16]byte // Source IPv6 address for sync packets
	DstEther         [6]byte  // Destination MAC address
	DstAddrMulticast [16]byte // Multicast IPv6 address
	PortMulticast    uint16   // Multicast port
	DstAddrUnicast   [16]byte // Unicast IPv6 address
	PortUnicast      uint16   // Unicast port
	Timeouts         Timeouts // Connection timeouts
}

// DefaultTimeouts returns default timeout values
func DefaultTimeouts() Timeouts {
	return Timeouts{
		TCPSynAck: 120e9, // 120 seconds
		TCPSyn:    120e9, // 120 seconds
		TCPFin:    120e9, // 120 seconds
		TCP:       120e9, // 120 seconds
		UDP:       30e9,  // 30 seconds
		Default:   16e9,  // 16 seconds
	}
}

// DefaultSyncConfig returns default sync configuration values
func DefaultSyncConfig() SyncConfig {
	return SyncConfig{
		Timeouts: DefaultTimeouts(),
	}
}
