package fwstate

import (
	"go.uber.org/zap"
	"google.golang.org/grpc"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/fwstate/controlplane/fwstatepb"
)

// FwStateModule is a control-plane component for firewall state management
type FwStateModule struct {
	cfg            *Config
	shm            *ffi.SharedMemory
	agents         []*ffi.Agent
	fwstateService *FwStateService
	log            *zap.SugaredLogger
}

// NewFwStateModule creates a new FwState module instance
func NewFwStateModule(cfg *Config, log *zap.SugaredLogger) (*FwStateModule, error) {
	log = log.With(zap.String("module", "fwstatepb.FwStateService"))

	shm, err := ffi.AttachSharedMemory(cfg.MemoryPath)
	if err != nil {
		return nil, err
	}

	instanceIndices := shm.InstanceIndices()
	log.Debugw("mapping shared memory",
		zap.Uint32s("instances", instanceIndices),
		zap.Stringer("size", cfg.MemoryRequirements),
	)

	agents, err := shm.AgentsAttach("fwstate", instanceIndices, uint(cfg.MemoryRequirements))
	if err != nil {
		return nil, err
	}

	fwstateService := NewFwStateService(shm, agents, log)

	return &FwStateModule{
		cfg:            cfg,
		shm:            shm,
		agents:         agents,
		fwstateService: fwstateService,
		log:            log,
	}, nil
}

// Name returns the module name
func (m *FwStateModule) Name() string {
	return "fwstate"
}

// Endpoint returns the gRPC endpoint
func (m *FwStateModule) Endpoint() string {
	return m.cfg.Endpoint
}

// ServicesNames returns the list of gRPC services provided by this module
func (m *FwStateModule) ServicesNames() []string {
	return []string{"fwstatepb.FwStateService"}
}

// RegisterService registers the gRPC service
func (m *FwStateModule) RegisterService(server *grpc.Server) {
	fwstatepb.RegisterFwStateServiceServer(server, m.fwstateService)
}

// Close closes the module and releases resources
func (m *FwStateModule) Close() error {
	for inst, agent := range m.agents {
		if err := agent.Close(); err != nil {
			m.log.Warnw("failed to close shared memory agent", zap.Int("inst", inst), zap.Error(err))
		}
	}

	if err := m.shm.Detach(); err != nil {
		m.log.Warnw("failed to detach from shared memory mapping", zap.Error(err))
	}

	return nil
}
