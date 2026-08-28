package fwstatepb

import (
	commonpb "github.com/yanet-platform/yanet2/common/commonpb/v1"
	"github.com/yanet-platform/yanet2/modules/fwstate/bindings/go/cfwstate"
)

func (m *SyncConfig) ToC() cfwstate.SyncConfig {
	if m == nil {
		return cfwstate.SyncConfig{}
	}

	var cfg cfwstate.SyncConfig
	src := m.GetSrcAddr().GetAddr()
	copy(cfg.SrcAddr[:], src)
	dstEther := m.GetDstEther().EUI48()
	copy(cfg.DstEther[:], dstEther[:])
	copy(cfg.DstAddrMulticast[:], m.GetDstAddrMulticast().GetAddr())
	cfg.PortMulticast = uint16(m.GetPortMulticast())
	copy(cfg.DstAddrUnicast[:], m.GetDstAddrUnicast().GetAddr())
	cfg.PortUnicast = uint16(m.GetPortUnicast())
	cfg.TcpSynAck = m.GetTcpSynAck()
	cfg.TcpSyn = m.GetTcpSyn()
	cfg.TcpFin = m.GetTcpFin()
	cfg.Tcp = m.GetTcp()
	cfg.Udp = m.GetUdp()
	cfg.Default = m.GetDefault()
	cfg.SyncSuppressTimeout = m.GetSyncSuppressTimeout()
	return cfg
}

func (m *SyncConfig) ToCWithDefaults(current cfwstate.SyncConfig) cfwstate.SyncConfig {
	cfg := m.ToC()
	if m == nil {
		return cfg
	}
	pbPortMulticast := m.GetPortMulticast()

	if len(m.GetSrcAddr().GetAddr()) == 0 {
		cfg.SrcAddr = current.SrcAddr
	}
	if m.GetDstEther() == nil {
		cfg.DstEther = current.DstEther
	}
	destinationsProvided := len(m.GetDstAddrMulticast().GetAddr()) != 0 ||
		pbPortMulticast != 0 || len(m.GetDstAddrUnicast().GetAddr()) != 0 ||
		m.GetPortUnicast() != 0
	if !destinationsProvided {
		cfg.DstAddrMulticast = current.DstAddrMulticast
		cfg.PortMulticast = current.PortMulticast
		cfg.DstAddrUnicast = current.DstAddrUnicast
		cfg.PortUnicast = current.PortUnicast
	}
	if cfg.TcpSynAck == 0 {
		cfg.TcpSynAck = current.TcpSynAck
	}
	if cfg.TcpSyn == 0 {
		cfg.TcpSyn = current.TcpSyn
	}
	if cfg.TcpFin == 0 {
		cfg.TcpFin = current.TcpFin
	}
	if cfg.Tcp == 0 {
		cfg.Tcp = current.Tcp
	}
	if cfg.Udp == 0 {
		cfg.Udp = current.Udp
	}
	if cfg.Default == 0 {
		cfg.Default = current.Default
	}
	if cfg.SyncSuppressTimeout == 0 {
		cfg.SyncSuppressTimeout = current.SyncSuppressTimeout
	}

	return cfg
}

func FromCSyncConfig(cfg cfwstate.SyncConfig) *SyncConfig {
	pb := &SyncConfig{
		SrcAddr:             &commonpb.IPAddress{Addr: append([]byte(nil), cfg.SrcAddr[:]...)},
		DstEther:            commonpb.NewMACAddressEUI48(cfg.DstEther),
		PortMulticast:       uint32(cfg.PortMulticast),
		PortUnicast:         uint32(cfg.PortUnicast),
		TcpSynAck:           cfg.TcpSynAck,
		TcpSyn:              cfg.TcpSyn,
		TcpFin:              cfg.TcpFin,
		Tcp:                 cfg.Tcp,
		Udp:                 cfg.Udp,
		Default:             cfg.Default,
		SyncSuppressTimeout: cfg.SyncSuppressTimeout,
	}
	if cfg.PortMulticast != 0 {
		pb.DstAddrMulticast = &commonpb.IPAddress{Addr: append([]byte(nil), cfg.DstAddrMulticast[:]...)}
	}
	if cfg.PortUnicast != 0 {
		pb.DstAddrUnicast = &commonpb.IPAddress{Addr: append([]byte(nil), cfg.DstAddrUnicast[:]...)}
	}
	return pb
}
