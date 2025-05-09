package forward

import (
	"context"
	"fmt"
	"math"
	"net"

	"go.uber.org/zap"
	"golang.org/x/sync/errgroup"
	"google.golang.org/grpc"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/controlplane/yncp/gateway"
	"github.com/yanet-platform/yanet2/modules/forward/controlplane/forwardpb"
)

// ForwardModule is a control-plane component of a module that is responsible for
// forwarding traffic between devices.
type ForwardModule struct {
	cfg            *Config
	server         *grpc.Server
	shm            *ffi.SharedMemory
	agents         []*ffi.Agent
	forwardService *ForwardService
	log            *zap.SugaredLogger
}

func NewForwardModule(cfg *Config, log *zap.SugaredLogger) (*ForwardModule, error) {
	log = log.With(zap.String("module", "forwardpb.ForwardService"))

	shm, err := ffi.AttachSharedMemory(cfg.MemoryPath)
	if err != nil {
		return nil, err
	}

	numaIndices := shm.NumaIndices()
	log.Debugw("mapping shared memory",
		zap.Uint32s("numa", numaIndices),
		zap.Stringer("size", cfg.MemoryRequirements),
	)

	agents, err := shm.AgentsAttach("forward", numaIndices, uint(cfg.MemoryRequirements))
	if err != nil {
		return nil, err
	}

	server := grpc.NewServer()

	// STATEMENT: All agents have the same topology.
	deviceCount := topologyDeviceCount(agents[0])
	if deviceCount >= math.MaxUint16 {
		return nil, fmt.Errorf("too many devices: %d (max %d)", deviceCount, math.MaxUint16)
	}

	forwardService := NewForwardService(agents, log, uint16(deviceCount))
	forwardpb.RegisterForwardServiceServer(server, forwardService)

	return &ForwardModule{
		cfg:            cfg,
		server:         server,
		shm:            shm,
		agents:         agents,
		forwardService: forwardService,
		log:            log,
	}, nil
}

// Close closes the module.
func (m *ForwardModule) Close() error {
	for numaIdx, agent := range m.agents {
		if err := agent.Close(); err != nil {
			m.log.Warnw("failed to close shared memory agent", zap.Int("numa", numaIdx), zap.Error(err))
		}
	}

	if err := m.shm.Detach(); err != nil {
		m.log.Warnw("failed to detach from shared memory mapping", zap.Error(err))
	}

	return nil
}

// Run runs the module until the specified context is canceled.
func (m *ForwardModule) Run(ctx context.Context) error {
	listener, err := net.Listen("tcp", m.cfg.Endpoint)
	if err != nil {
		return fmt.Errorf("failed to initialize gRPC listener: %w", err)
	}

	wg, ctx := errgroup.WithContext(ctx)
	wg.Go(func() error {
		m.log.Infow("exposing gRPC API", zap.Stringer("addr", listener.Addr()))
		return m.server.Serve(listener)
	})

	serviceNames := []string{"forwardpb.ForwardService"}

	if err = gateway.RegisterModule(
		ctx,
		m.cfg.GatewayEndpoint,
		listener,
		serviceNames,
		m.log,
	); err != nil {
		return fmt.Errorf("failed to register services: %w", err)
	}

	<-ctx.Done()

	m.log.Infow("stopping gRPC API", zap.Stringer("addr", listener.Addr()))
	defer m.log.Infow("stopped gRPC API", zap.Stringer("addr", listener.Addr()))

	m.server.GracefulStop()

	return wg.Wait()
}
