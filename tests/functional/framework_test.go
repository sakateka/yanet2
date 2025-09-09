package functional

import (
	"os"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"github.com/yanet-platform/yanet2/tests/functional/framework"
	"go.uber.org/zap"
)

// Global framework instance shared across all tests
var globalFramework *framework.TestFramework

// TestMain sets up and tears down the test framework
func TestMain(m *testing.M) {
	var code int

	// Create logger for detailed logging
	lg := zap.NewDevelopmentConfig()
	if _, ok := os.LookupEnv("YANET_TEST_DEBUG"); !ok {
		// no env - set error level
		lg.Level = zap.NewAtomicLevelAt(zap.ErrorLevel)
	} else {
		// save debug log to test.log
		lg.OutputPaths = []string{"test.log"}
		lg.ErrorOutputPaths = []string{"test.log"}
	}
	logger, err := lg.Build()
	if err != nil {
		panic(err)
	}
	defer logger.Sync()
	sugar := logger.Sugar()

	// Initialize framework once for all tests
	fw, err := framework.New(&framework.Config{
		QEMUImage: "yanet-test.qcow2",
	}, framework.WithLog(sugar))
	if err != nil {
		panic(err)
	}

	globalFramework = fw

	// Start test environment
	if err := fw.Start(); err != nil {
		panic(err)
	}

	// Ensure cleanup happens before os.Exit
	defer func() {
		if fw != nil {
			if err := fw.Stop(); err != nil {
				sugar.Errorf("Failed to stop framework: %v", err)
			}
		}
		os.Exit(code)
	}()

	// Wait for VM to be ready
	if err := fw.QEMU.WaitForReady(60 * time.Second); err != nil {
		panic(err)
	}

	// Start YANET with decap module configuration
	dataplaneConfig := `
dataplane:
  storage: /dev/hugepages/yanet
  dpdk_memory: 1024
  loglevel: trace
  instances:
    - dp_memory: 1073741824
      cp_memory: 1073741824
      numa_id: 0
  devices:
    - port_name: 01:00.0
      mac_addr: 52:54:00:6b:ff:a5
      mtu: 7000
      max_lro_packet_size: 7200
      rss_hash: 0
      workers:
        - core_id: 0
          instance_id: 0
          rx_queue_len: 1024
          tx_queue_len: 1024
    - port_name: virtio_user_kni0
      mac_addr: 52:54:00:6b:ff:a5
      mtu: 7000
      max_lro_packet_size: 7200
      rss_hash: 0
      workers:
        - core_id: 0
          instance_id: 0
          rx_queue_len: 1024
          tx_queue_len: 1024
  connections:
    - src_device_id: 0
      dst_device_id: 1
    - src_device_id: 1
      dst_device_id: 0
`

	controlplaneConfig := `
logging:
  level: debug
`

	if err := fw.StartYANET(dataplaneConfig, controlplaneConfig); err != nil {
		panic(err)
	}

	if _, err := fw.CLI.ExecuteCommands(framework.CommonConfigCommands...); err != nil {
		panic(err)
	}

	// Run tests
	code = m.Run()
}

// TestFramework - comprehensive test for checking all yanet functionality
func TestFramework(t *testing.T) {
	// Use global framework instance
	fw := globalFramework
	require.NotNil(t, fw, "Global framework should be initialized")

	// Test 1: Check basic command execution
	t.Run("Basic_Commands", func(t *testing.T) {
		// Check basic system commands
		basicCommands := []struct {
			name    string
			command string
			check   func(string) bool
		}{
			{
				name:    "whoami",
				command: "whoami",
				check:   func(output string) bool { return strings.Contains(output, "root") },
			},
			{
				name:    "pwd",
				command: "pwd",
				check:   func(output string) bool { return strings.Contains(output, "/root") },
			},
			{
				name:    "date",
				command: "date",
				check:   func(output string) bool { return len(strings.TrimSpace(output)) > 10 },
			},
			{
				name:    "uname",
				command: "uname -a",
				check:   func(output string) bool { return strings.Contains(strings.ToLower(output), "linux") },
			},
			{
				name:    "memory_info",
				command: "cat /proc/meminfo | head -5",
				check:   func(output string) bool { return strings.Contains(output, "MemTotal") },
			},
		}

		for _, cmd := range basicCommands {
			t.Run(cmd.name, func(t *testing.T) {
				output, err := fw.CLI.ExecuteCommand(cmd.command)
				require.NoError(t, err, "Command %s failed", cmd.command)
				require.True(t, cmd.check(output), "Command %s output validation failed: %s", cmd.command, output)
			})
		}
	})

	// Test 3: Check filesystem and mounting
	t.Run("Filesystem_Check", func(t *testing.T) {
		// Check main directories
		directories := []string{
			"/mnt/binaries",
			"/mnt/config",
			"/mnt/build",
			"/mnt/target",
		}

		for _, dir := range directories {
			t.Run("check_"+strings.ReplaceAll(dir, "/", "_"), func(t *testing.T) {
				output, err := fw.CLI.ExecuteCommand("ls -la " + dir)
				require.NoError(t, err, "Failed to list directory %s", dir)
				require.NotEmpty(t, output, "Directory %s appears to be empty", dir)
				require.NotContains(t, output, "such")
				output, err = fw.CLI.ExecuteCommand("mount | grep " + dir)
				require.NoError(t, err, "Failed to check mount point %s", dir)
				require.NotEmpty(t, output, "Mount point %s not found", dir)
			})
		}
	})

	// Test 4: Check YANET binaries availability
	t.Run("YANET_Binaries", func(t *testing.T) {
		// Check CLI binaries
		cliBinaries := []struct {
			name string
			path string
		}{
			{"main_cli", "/mnt/target/release/yanet-cli"},
			{"common_cli", "/mnt/target/release/yanet-cli-common"},
			{"decap_cli", "/mnt/target/release/yanet-cli-decap"},
			{"dscp_cli", "/mnt/target/release/yanet-cli-dscp"},
			{"forward_cli", "/mnt/target/release/yanet-cli-forward"},
			{"nat64_cli", "/mnt/target/release/yanet-cli-nat64"},
			{"route_cli", "/mnt/target/release/yanet-cli-route"},
			{"pipeline_cli", "/mnt/target/release/yanet-cli-pipeline"},
		}

		for _, binary := range cliBinaries {
			t.Run(binary.name, func(t *testing.T) {
				// Check file existence
				output, err := fw.CLI.ExecuteCommand("ls -la " + binary.path)
				require.NoError(t, err, "⚠️  Binary %s check failed: %v", binary.name, err)
				require.NotContainsf(t, output, "such", "⚠️  Binary %s not found: %v", binary.name)
				require.Contains(t, output, binary.path, "Binary file not found in listing")

				// Check binary help
				helpOutput, helpErr := fw.CLI.ExecuteCommand(binary.path + " --help")
				require.NoError(t, helpErr, "Binary %s help check failed: %v", binary.name, helpErr)
				require.NotEmpty(t, helpOutput, "Binary %s help check failed: %v", binary.name, helpErr)
			})
		}

		// Check main YANET components
		t.Run("yanet_components", func(t *testing.T) {
			components := []string{
				"/mnt/build/dataplane/yanet-dataplane",
				"/mnt/build/controlplane/yanet-controlplane",
			}

			for _, component := range components {
				output, err := fw.CLI.ExecuteCommand("ls -la " + component)
				require.NoError(t, err, "Component %s not found", component)
				require.NotContains(t, output, "such")
			}
		})
	})

	// Test 5: Check network interfaces and socket devices
	t.Run("Network_Interfaces", func(t *testing.T) {
		// Check network interfaces
		output, err := fw.CLI.ExecuteCommand("ip link show")
		require.NoError(t, err)
		require.Contains(t, output, "lo", "Loopback interface should be present")

		// Check framework socket clients
		t.Run("socket_clients", func(t *testing.T) {
			for i := range 2 {
				// Check if socket path exists
				socketPath := fw.QEMU.SocketPaths[i]

				// Check if socket file exists
				_, err := os.Stat(socketPath)
				os.IsNotExist(err)
				require.NoError(t, err, "Failed to check socket file %s", socketPath)

				client, err := fw.GetSocketClient(i)
				require.NoError(t, err, "Failed to get socket client %d", i)
				require.NotNil(t, client, "Socket client %d should not be nil", i)
			}
		})
	})

	// Test 6: Check PacketParser
	t.Run("PacketParser", func(t *testing.T) {
		require.NotNil(t, fw.PacketParser, "PacketParser should be initialized")

		// Create simple test packet
		testPacket := []byte{
			// Ethernet header (14 bytes)
			0x52, 0x54, 0x00, 0x11, 0x00, 0x01, // dst MAC
			0x52, 0x54, 0x00, 0x11, 0x00, 0x02, // src MAC
			0x08, 0x00, // EtherType IPv4
			// IPv4 header (20 bytes minimum)
			0x45, 0x00, 0x00, 0x1c, // version, IHL, TOS, length
			0x00, 0x01, 0x40, 0x00, // ID, flags, fragment offset
			0x40, 0x01, 0x00, 0x00, // TTL, protocol (ICMP), checksum
			0xc0, 0xa8, 0x01, 0x01, // source IP (192.168.1.1)
			0xc0, 0xa8, 0x01, 0x02, // dest IP (192.168.1.2)
		}

		// Pad to minimum Ethernet frame size
		if len(testPacket) < 60 {
			padding := make([]byte, 60-len(testPacket))
			testPacket = append(testPacket, padding...)
		}

		packetInfo, err := fw.PacketParser.ParsePacket(testPacket)
		require.NoError(t, err, "Failed to parse test packet")
		require.NotNil(t, packetInfo, "PacketInfo should not be nil")
		require.True(t, packetInfo.IsIPv4, "Packet should be IPv4")
		require.Equal(t, "192.168.1.1", packetInfo.SrcIP.String())
		require.Equal(t, "192.168.1.2", packetInfo.DstIP.String())
	})

	// Test 7: Check system resources
	t.Run("System_Resources", func(t *testing.T) {
		// Check memory
		t.Run("memory", func(t *testing.T) {
			output, err := fw.CLI.ExecuteCommand("free -h")
			require.NoError(t, err)
			require.Contains(t, output, "Mem:", "Memory information should be available")
		})

		// Check CPU
		t.Run("cpu", func(t *testing.T) {
			output, err := fw.CLI.ExecuteCommand("nproc")
			require.NoError(t, err)
			require.NotEmpty(t, strings.TrimSpace(output), "CPU count should be available")
		})

		// Check hugepages (important for DPDK)
		t.Run("hugepages", func(t *testing.T) {
			output, err := fw.CLI.ExecuteCommand("cat /proc/meminfo | grep -i huge")
			require.NoErrorf(t, err, "Failed to get hugepages info: %s", output)
		})
	})
}
