package acl

import (
	"go.uber.org/zap"
	"google.golang.org/grpc"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/acl/controlplane/aclpb"
)

// ACLModule is a control-plane component for ACL (Access Control List) module
// with integrated firewall state management
type ACLModule struct {
	cfg        *Config
	shm        *ffi.SharedMemory
	agents     []*ffi.Agent
	aclService *ACLService
	log        *zap.SugaredLogger
}

// NewACLModule creates a new ACL module instance
func NewACLModule(cfg *Config, log *zap.SugaredLogger) (*ACLModule, error) {
	log = log.With(zap.String("module", "aclpb.AclService"))

	shm, err := ffi.AttachSharedMemory(cfg.MemoryPath)
	if err != nil {
		return nil, err
	}

	instanceIndices := shm.InstanceIndices()
	log.Debugw("mapping shared memory",
		zap.Uint32s("instances", instanceIndices),
		zap.Stringer("size", cfg.MemoryRequirements),
	)

	agents, err := shm.AgentsAttach("acl", instanceIndices, uint(cfg.MemoryRequirements))
	if err != nil {
		return nil, err
	}

	// Create ACL service - gateway endpoint will be set via SetGatewayEndpoint
	aclService := NewACLService(shm, agents, log)

	return &ACLModule{
		cfg:        cfg,
		shm:        shm,
		agents:     agents,
		aclService: aclService,
		log:        log,
	}, nil
}

// SetGatewayEndpoint implements the GatewayAwareModule interface
// This is called by the gateway runner to provide the gateway endpoint for inter-module communication
func (m *ACLModule) SetGatewayEndpoint(endpoint string) {
	m.aclService.SetGatewayEndpoint(endpoint)
	m.log.Infow("gateway endpoint configured", zap.String("endpoint", endpoint))
}

// Name returns the module name
func (m *ACLModule) Name() string {
	return "acl"
}

// Endpoint returns the gRPC endpoint
func (m *ACLModule) Endpoint() string {
	return m.cfg.Endpoint
}

// ServicesNames returns the list of gRPC services provided by this module
func (m *ACLModule) ServicesNames() []string {
	return []string{"aclpb.AclService"}
}

// RegisterService registers the gRPC service
func (m *ACLModule) RegisterService(server *grpc.Server) {
	aclpb.RegisterAclServiceServer(server, m.aclService)
}

// Close closes the module and releases resources
func (m *ACLModule) Close() error {
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
