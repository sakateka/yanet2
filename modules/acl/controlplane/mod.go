package acl

import (
	"fmt"

	"go.uber.org/zap"
	"google.golang.org/grpc"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/acl/controlplane/aclpb"
)

const agentName = "acl"
const serviceName = "aclpb.AclService"

// ACLModule is a control-plane component for ACL (Access Control List) module
// with integrated firewall state management
type AclModule struct {
	cfg     *Config
	shm     *ffi.SharedMemory
	agents  []*ffi.Agent
	service *AclService
	log     *zap.SugaredLogger
}

// NewAclModule creates a new Acl module instance
func NewAclModule(cfg *Config, log *zap.SugaredLogger) (*AclModule, error) {
	log = log.With(zap.String("module", serviceName))

	shm, err := ffi.AttachSharedMemory(cfg.MemoryPath)
	if err != nil {
		return nil, fmt.Errorf("failed to attach shared memory: %w", err)
	}

	instanceIndices := shm.InstanceIndices()
	log.Debugw("mapping shared memory",
		zap.Uint32s("instances", instanceIndices),
		zap.Stringer("size", cfg.MemoryRequirements),
	)

	agents, err := shm.AgentsAttach(agentName, instanceIndices, uint(cfg.MemoryRequirements))
	if err != nil {
		return nil, fmt.Errorf("failed to attach agents: %w", err)
	}

	// Create ACL service - gateway endpoint will be set via SetGatewayEndpoint
	service := NewAclService(shm, agents, log)

	return &AclModule{
		cfg:     cfg,
		shm:     shm,
		agents:  agents,
		service: service,
		log:     log,
	}, nil
}

func (m *AclModule) Name() string {
	return "acl"
}

func (m *AclModule) Endpoint() string {
	return m.cfg.Endpoint
}

func (m *AclModule) ServicesNames() []string {
	return []string{serviceName}
}

func (m *AclModule) RegisterService(server *grpc.Server) {
	aclpb.RegisterAclServiceServer(server, m.service)
}

func (m *AclModule) Close() error {
	for i, agent := range m.agents {
		if err := agent.Close(); err != nil {
			m.log.Warnw("failed to close shared memory agent", "instance", i, "error", err)
		}
	}
	if err := m.shm.Detach(); err != nil {
		m.log.Warnw("failed to detach shared memory", "error", err)
	}

	return nil
}

// SetGatewayEndpoint implements the GatewayAwareModule interface
// This is called by the gateway runner to provide the gateway endpoint for inter-module communication
func (m *AclModule) SetGatewayEndpoint(endpoint string) {
	m.service.SetGatewayEndpoint(endpoint)
	m.log.Infow("gateway endpoint configured", zap.String("endpoint", endpoint))
}
