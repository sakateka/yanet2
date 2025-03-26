package decap

import (
	"context"
	"fmt"
	"net/netip"
	"slices"
	"sync"

	"go.uber.org/zap"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/yanet-platform/yanet2/controlplane/internal/ffi"
	"github.com/yanet-platform/yanet2/controlplane/modules/decap/decappb"
)

type DecapService struct {
	decappb.UnimplementedDecapServiceServer

	mu          sync.Mutex
	cfg         *Config
	cfgReloader decapConfigReloader
	agents      []*ffi.Agent
	log         *zap.SugaredLogger
}

func NewDecapService(cfg *Config, configReloader decapConfigReloader, agents []*ffi.Agent, log *zap.SugaredLogger) *DecapService {
	return &DecapService{
		cfg:         cfg,
		cfgReloader: configReloader,
		agents:      agents,
		log:         log,
	}
}

func (m *DecapService) ShowConfig(
	ctx context.Context,
	request *decappb.ShowConfigRequest,
) (*decappb.ShowConfigResponse, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	prefixes := make([]string, 0, len(m.cfg.Prefixes))
	for _, prefix := range m.cfg.Prefixes {
		prefixes = append(prefixes, prefix.String())
	}
	response := &decappb.ShowConfigResponse{
		Config: &decappb.Config{
			MemoryPath:         m.cfg.MemoryPath,
			MemoryRequirements: m.cfg.MemoryRequirements.String(),
			Endpoint:           m.cfg.Endpoint,
			GatewayEndpoint:    m.cfg.GatewayEndpoint,
			Prefixes:           prefixes,
		},
	}
	return response, nil
}

func (m *DecapService) targetNuma(target *decappb.TargetModule) ([]uint32, error) {
	numaIndices := target.GetNuma()
	slices.Sort(numaIndices)
	numaIndices = slices.Compact(numaIndices)
	if !slices.Equal(numaIndices, target.GetNuma()) {
		return nil, status.Error(codes.InvalidArgument, "repeated NUMA indices are duplicated")
	}
	if len(numaIndices) > 0 && int(numaIndices[len(numaIndices)-1]) >= len(m.agents) {
		return nil, status.Error(codes.InvalidArgument, "NUMA indices are out of range")
	}
	if len(numaIndices) == 0 {
		for idx := range m.agents {
			numaIndices = append(numaIndices, uint32(idx))
		}
	}
	return numaIndices, nil
}

func (m *DecapService) ReloadConfig(
	ctx context.Context,
	request *decappb.ReloadConfigRequest,
) (*decappb.ReloadConfigResponse, error) {
	cfg := &Config{}
	if err := m.cfgReloader(cfg); err != nil {
		return nil, status.Error(codes.Internal, fmt.Sprintf("reloading failed: %v", err))
	}
	m.cfg = cfg

	name := request.GetTarget().GetModuleName()
	numa, err := m.targetNuma(request.GetTarget())
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	return &decappb.ReloadConfigResponse{}, m.updateModuleConfigs(name, numa, cfg.Prefixes)
}

func (m *DecapService) updateModuleConfigs(
	name string,
	numaIndices []uint32,
	prefixes []netip.Prefix,
) error {
	configs := make([]*ModuleConfig, 0, len(numaIndices))

	for _, numaIdx := range numaIndices {
		agent := m.agents[numaIdx]

		config, err := NewModuleConfig(agent, name)
		if err != nil {
			return fmt.Errorf("failed to create %q module config: %w", name, err)
		}
		for _, prefix := range prefixes {
			if err := config.PrefixAdd(prefix); err != nil {
				return fmt.Errorf("failed to add prefix for %s: %w", name, err)
			}
		}

		configs = append(configs, config)
	}

	for _, numaIdx := range numaIndices {
		agent := m.agents[numaIdx]
		config := configs[numaIdx]

		if err := agent.UpdateModules([]ffi.ModuleConfig{config.AsFFIModule()}); err != nil {
			return fmt.Errorf("failed to update module: %w", err)
		}

		m.log.Infow("successfully updated module",
			zap.String("name", name),
			zap.Uint32("numa", numaIdx),
		)
	}
	return nil
}
