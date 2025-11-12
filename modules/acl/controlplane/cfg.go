package acl

import (
	"github.com/c2h5oh/datasize"
)

// Config holds the configuration for the ACL module
type Config struct {
	// Endpoint is the gRPC endpoint for the ACL service
	Endpoint string `yaml:"endpoint"`

	// MemoryPath is the path to the shared memory file
	MemoryPath string `yaml:"memory_path"`

	// MemoryRequirements specifies the memory requirements for the module
	MemoryRequirements datasize.ByteSize `yaml:"memory_requirements"`
}

// DefaultConfig returns a default configuration for the ACL module
func DefaultConfig() *Config {
	return &Config{
		Endpoint:           "[::1]:50051",
		MemoryPath:         "/dev/hugepages/yanet",
		MemoryRequirements: 64 * datasize.MB,
	}
}
