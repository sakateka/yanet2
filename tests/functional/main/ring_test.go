package functional

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/gopacket/gopacket/pcapgo"
	"github.com/stretchr/testify/require"
	"gopkg.in/yaml.v3"

	"github.com/yanet-platform/yanet2/tests/functional/framework"
)

// ringInfo is one ring as the ring CLI renders it in JSON.
type ringInfo struct {
	Name        string `json:"name"`
	Capacity    uint64 `json:"capacity"`
	WorkerCount uint64 `json:"worker_count"`
}

// ringCLI runs the ring CLI with the given arguments in the guest.
func ringCLI(fw *framework.TestFramework, args string) (string, error) {
	return fw.ExecuteCommand(framework.CLIRing + " " + args)
}

// listRings returns every registered ring, read from the JSON listing.
func listRings(t *testing.T, fw *framework.TestFramework) []ringInfo {
	t.Helper()
	output, err := ringCLI(fw, "list --format json")
	require.NoError(t, err, "ring list failed")
	var rings []ringInfo
	require.NoError(t, json.Unmarshal([]byte(strings.TrimSpace(output)), &rings),
		"ring list must be a JSON array: %q", output)
	return rings
}

// showRing returns one ring read from the JSON show output.
func showRing(t *testing.T, fw *framework.TestFramework, name string) ringInfo {
	t.Helper()
	output, err := ringCLI(fw, "show --format json --name "+name)
	require.NoError(t, err, "ring show failed")
	var response struct {
		Ring *ringInfo `json:"ring"`
	}
	require.NoError(t, json.Unmarshal([]byte(strings.TrimSpace(output)), &response),
		"ring show must be a JSON object: %q", output)
	require.NotNil(t, response.Ring, "ring show must carry the ring")
	return *response.Ring
}

// findRing returns the listed ring with the given name and whether it is
// listed at all.
func findRing(rings []ringInfo, name string) (ringInfo, bool) {
	for _, ring := range rings {
		if ring.Name == name {
			return ring, true
		}
	}
	return ringInfo{}, false
}

// dataplaneWorkerCount returns the number of dataplane workers the package
// harness declares, which is the per-worker ring count every ring has.
func dataplaneWorkerCount(t *testing.T) uint64 {
	t.Helper()
	var config struct {
		Dataplane struct {
			Devices []struct {
				Workers []struct{} `yaml:"workers"`
			} `yaml:"devices"`
		} `yaml:"dataplane"`
	}
	raw := framework.DataplaneConfig(framework.DataplaneOptions{
		PacketRecircLimit: testPacketRecircLimit,
	})
	require.NoError(t, yaml.Unmarshal([]byte(raw), &config))
	count := 0
	for _, device := range config.Dataplane.Devices {
		count += len(device.Workers)
	}
	require.Positive(t, count, "harness must declare dataplane workers")
	return uint64(count)
}

// pdumpCaptureDport is the UDP destination port of the packets the pdump
// capture check sends; the capture filter selects only them.
const pdumpCaptureDport = 5555

// requirePdumpCapturesInput installs pdump in front of the forwarding chain,
// sends UDP packets and asserts they are forwarded and captured exactly.
//
// The pdump reader starts from the oldest retained record, so reading
// after sending observes every captured packet without racing the sender.
// The chain and the pdump config are restored and removed afterwards.
func requirePdumpCapturesInput(t *testing.T, fw *framework.TestFramework, config string) {
	t.Helper()
	const packetCount = 3

	_, err := fw.ExecuteCommands(
		framework.CLIPdump+" set --name "+config+
			fmt.Sprintf(" --input --filter 'udp and dst port %d'", pdumpCaptureDport),
		framework.CLIFunction+" update --name=test --chains chain2:1=pdump:"+config+
			",forward:forward0,route:route0",
		framework.CLIPipeline+" update --name=test --functions test",
	)
	require.NoError(t, err, "pdump setup failed")
	t.Cleanup(func() {
		_, err := fw.ExecuteCommands(
			framework.CLIFunction+" update --name=test --chains chain2:1=forward:forward0,route:route0",
			framework.CLIPipeline+" update --name=test --functions test",
			framework.CLIPdump+" delete --name "+config,
		)
		require.NoError(t, err, "pdump teardown failed")
	})

	sent := make([][]byte, 0, packetCount)
	for idx := range packetCount {
		packet, err := framework.NewPacket(nil,
			framework.Ether(framework.EtherSrc(framework.SrcMAC), framework.EtherDst(framework.DstMAC)),
			framework.IPv4(framework.IPSrc("192.0.2.100"), framework.IPDst("172.16.0.10")),
			framework.UDP(framework.UDPSport(uint16(10000+idx)), framework.UDPDport(pdumpCaptureDport)),
			// The payload brings the frame to the Ethernet minimum, so the
			// wire adds no padding and the captured frame equals the sent one.
			framework.Raw([]byte("pdump ring capture")),
		)
		require.NoError(t, err)
		sent = append(sent, packet.Data())

		_, err = fw.SendPacketAndCapture(0, 0, packet.Data(), 500*time.Millisecond)
		require.NoError(t, err, "packet %d must still be forwarded with pdump in the chain", idx)
	}

	const dumpPath = "/tmp/pdump-ring-test.pcap"
	readOutput, err := fw.ExecuteCommandWithTimeout(
		fmt.Sprintf("rm -f %[1]s && timeout 20 %[2]s read --name %[3]s --dump-format pcap --num %[4]d --output %[1]s",
			dumpPath, framework.CLIPdump, config, packetCount),
		30*time.Second,
	)
	require.NoError(t, err, "pdump read must return the captured packets: %s", readOutput)

	// The dump crosses the serial console as wrapped base64 lines, since a
	// binary stream or one unterminated line does not survive it.
	encoded, err := fw.ExecuteCommand("base64 " + dumpPath)
	require.NoError(t, err)
	dump, err := base64.StdEncoding.DecodeString(strings.Join(strings.Fields(encoded), ""))
	require.NoError(t, err)

	reader, err := pcapgo.NewReader(bytes.NewReader(dump))
	require.NoError(t, err, "pdump output must be a pcap stream: read %q, dump %q", readOutput, encoded)
	captured := make([][]byte, 0, packetCount)
	for {
		data, _, err := reader.ReadPacketData()
		if err != nil {
			break
		}
		captured = append(captured, data)
	}
	require.ElementsMatch(t, sent, captured, "pdump must capture exactly the sent frames")
}

// Test_RingCLI_LifecycleAndPdumpCapture verifies that the ring CLI drives the
// ring service hosted by a running pdump module.
//
// A created ring is listed and shown with its capacity and the dataplane
// worker count, bad creates leave the registry unchanged, a deleted name can
// be reused, and pdump capture works with and without a ring present.
func Test_RingCLI_LifecycleAndPdumpCapture(t *testing.T) {
	t.Parallel()
	withBootedVM(t, func(fw *framework.TestFramework) {
		testRingCLILifecycleAndPdumpCapture(t, fw)
	})
}

func testRingCLILifecycleAndPdumpCapture(t *testing.T, fw *framework.TestFramework) {
	const (
		ringName     = "ring-tfn0"
		badRingName  = "ring-tfn-bad"
		capacity     = uint64(64 << 10)
		recreatedCap = uint64(128 << 10)
	)
	workers := dataplaneWorkerCount(t)

	// A failed step may leave rings behind in the shared VM; remove them
	// so later tests start from an empty registry.
	//
	// Deleting an absent ring just fails, which is fine here.
	t.Cleanup(func() {
		if !t.Failed() {
			return
		}
		for _, name := range []string{ringName, badRingName} {
			_, _ = ringCLI(fw, "delete --name "+name)
		}
	})

	fw.Run("Create_lists_and_shows_ring", func(fw *framework.TestFramework, t *testing.T) {
		_, listed := findRing(listRings(t, fw), ringName)
		require.False(t, listed, "ring must not exist before create")

		_, err := ringCLI(fw, fmt.Sprintf("create --name %s --capacity %d", ringName, capacity))
		require.NoError(t, err, "ring create failed")

		want := ringInfo{Name: ringName, Capacity: capacity, WorkerCount: workers}
		ring, listed := findRing(listRings(t, fw), ringName)
		require.True(t, listed, "created ring must be listed")
		require.Equal(t, want, ring)
		require.Equal(t, want, showRing(t, fw, ringName))
	})

	fw.Run("Bad_create_changes_nothing", func(fw *framework.TestFramework, t *testing.T) {
		before := listRings(t, fw)

		cases := []struct {
			name string
			args string
			code string
		}{
			{
				name: "duplicate name",
				args: fmt.Sprintf("create --name %s --capacity %d", ringName, recreatedCap),
				code: "AlreadyExists",
			},
			{
				// A power of two, so the CLI passes it on and the
				// service's own range check rejects it.
				name: "capacity below the record frame size",
				args: "create --name " + badRingName + " --capacity 4",
				code: "InvalidArgument",
			},
		}
		for _, tc := range cases {
			fw.Run(strings.ReplaceAll(tc.name, " ", "_"), func(fw *framework.TestFramework, t *testing.T) {
				output, err := ringCLI(fw, "--format json "+tc.args)
				require.Error(t, err, "bad create must exit non-zero")
				var failure struct {
					OK    bool `json:"ok"`
					Error struct {
						Code string `json:"code"`
					} `json:"error"`
				}
				require.NoError(t, json.Unmarshal([]byte(strings.TrimSpace(output)), &failure),
					"bad create must report a JSON error: %q", output)
				require.False(t, failure.OK)
				require.Equal(t, tc.code, failure.Error.Code)
				require.ElementsMatch(t, before, listRings(t, fw), "bad create must not change the registry")
			})
		}
	})

	fw.Run("Pdump_captures_with_ring_present", func(fw *framework.TestFramework, t *testing.T) {
		requirePdumpCapturesInput(t, fw, "pdump-tfn-ring")
	})

	fw.Run("Delete_removes_ring", func(fw *framework.TestFramework, t *testing.T) {
		_, err := ringCLI(fw, "delete --name "+ringName)
		require.NoError(t, err, "ring delete failed")

		_, listed := findRing(listRings(t, fw), ringName)
		require.False(t, listed, "deleted ring must not be listed")
		_, err = ringCLI(fw, "show --name "+ringName)
		require.Error(t, err, "show of a deleted ring must exit non-zero")
	})

	fw.Run("Recreate_reuses_name", func(fw *framework.TestFramework, t *testing.T) {
		_, err := ringCLI(fw, fmt.Sprintf("create --name %s --capacity %d", ringName, recreatedCap))
		require.NoError(t, err, "recreate after delete failed")
		require.Equal(t,
			ringInfo{Name: ringName, Capacity: recreatedCap, WorkerCount: workers},
			showRing(t, fw, ringName),
		)

		_, err = ringCLI(fw, "delete --name "+ringName)
		require.NoError(t, err, "delete of the recreated ring failed")
		_, listed := findRing(listRings(t, fw), ringName)
		require.False(t, listed, "deleted ring must not be listed")
	})

	fw.Run("Pdump_captures_with_ring_absent", func(fw *framework.TestFramework, t *testing.T) {
		requirePdumpCapturesInput(t, fw, "pdump-tfn-noring")
	})
}
