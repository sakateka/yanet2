package forward

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
}

type ForwardConfig struct {
	DeviceForwards []ForwardDeviceConfig `yaml:"devices"`
}

type ForwardDeviceID uint16

type ForwardDeviceConfig struct {
	L2ForwardDeviceID ForwardDeviceID                  `yaml:"l2_forward_device_id"`
	Forwards          map[netip.Prefix]ForwardDeviceID `yaml:"forwards"`
}

type instanceKey struct {
	name    string
	numaIdx uint32
}

func DefaultConfig() *Config {
	return &Config{
		MemoryPath:         "/dev/hugepages/yanet",
		MemoryRequirements: 16 * datasize.MB,
		Endpoint:           "[::1]:0",
		GatewayEndpoint:    "[::1]:8080",
	}
}
