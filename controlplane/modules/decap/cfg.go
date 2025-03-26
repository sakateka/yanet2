package decap

import (
	"net/netip"

	"github.com/c2h5oh/datasize"
)

type Config struct {
	// MemoryPath is the path to the shared-memory file that is used to
	// communicate with dataplane.
	MemoryPath string `yaml:"memory_path"`
	// MemoryRequirements is the amount of memory that is required for a single
	// transaction.
	MemoryRequirements datasize.ByteSize `yaml:"memory_requirements"`

	Endpoint        string `yaml:"endpoint"`
	GatewayEndpoint string `yaml:"gateway_endpoint"`

	// Prefixes is a list of network prefixes against which
	// the module will match packets for processing.
	Prefixes []netip.Prefix `yaml:"prefixes"`
}

func DefaultConfig() *Config {
	return &Config{
		MemoryPath:         "/dev/hugepages/yanet",
		MemoryRequirements: 16 * datasize.MB,
		Endpoint:           "[::1]:0",
		GatewayEndpoint:    "[::1]:8080",
		Prefixes:           nil, // Do not include any prefixes by default
	}
}
