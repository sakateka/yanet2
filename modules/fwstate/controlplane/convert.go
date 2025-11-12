package fwstate

import (
	"github.com/yanet-platform/yanet2/modules/fwstate/controlplane/fwstatepb"
)

// ConvertPbToMapConfig converts protobuf MapConfig to internal MapConfig
func ConvertPbToMapConfig(pb *fwstatepb.MapConfig) *MapConfig {
	return &MapConfig{
		IndexSize:        pb.GetIndexSize(),
		ExtraBucketCount: pb.GetExtraBucketCount(),
	}
}

// ConvertMapConfigToPb converts internal MapConfig to protobuf MapConfig
func ConvertMapConfigToPb(cfg *MapConfig) *fwstatepb.MapConfig {
	return &fwstatepb.MapConfig{
		IndexSize:        cfg.IndexSize,
		ExtraBucketCount: cfg.ExtraBucketCount,
	}
}

// ConvertPbToSyncConfig converts protobuf SyncConfig to internal SyncConfig
func ConvertPbToSyncConfig(pb *fwstatepb.SyncConfig) *SyncConfig {
	cfg := &SyncConfig{
		Timeouts: Timeouts{
			TCPSynAck: pb.GetTcpSynAck(),
			TCPSyn:    pb.GetTcpSyn(),
			TCPFin:    pb.GetTcpFin(),
			TCP:       pb.GetTcp(),
			UDP:       pb.GetUdp(),
			Default:   pb.GetDefault(),
		},
	}

	copy(cfg.SrcAddr[:], pb.GetSrcAddr())
	copy(cfg.DstEther[:], pb.GetDstEther())
	copy(cfg.DstAddrMulticast[:], pb.GetDstAddrMulticast())
	cfg.PortMulticast = uint16(pb.GetPortMulticast())
	copy(cfg.DstAddrUnicast[:], pb.GetDstAddrUnicast())
	cfg.PortUnicast = uint16(pb.GetPortUnicast())

	return cfg
}

// ConvertSyncConfigToPb converts internal SyncConfig to protobuf SyncConfig
func ConvertSyncConfigToPb(cfg *SyncConfig) *fwstatepb.SyncConfig {
	return &fwstatepb.SyncConfig{
		SrcAddr:          cfg.SrcAddr[:],
		DstEther:         cfg.DstEther[:],
		DstAddrMulticast: cfg.DstAddrMulticast[:],
		PortMulticast:    uint32(cfg.PortMulticast),
		DstAddrUnicast:   cfg.DstAddrUnicast[:],
		PortUnicast:      uint32(cfg.PortUnicast),
		TcpSynAck:        cfg.Timeouts.TCPSynAck,
		TcpSyn:           cfg.Timeouts.TCPSyn,
		TcpFin:           cfg.Timeouts.TCPFin,
		Tcp:              cfg.Timeouts.TCP,
		Udp:              cfg.Timeouts.UDP,
		Default:          cfg.Timeouts.Default,
	}
}
