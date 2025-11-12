package acl

import (
	"context"
	"fmt"
	"sync"

	"go.uber.org/zap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	commonpb "github.com/yanet-platform/yanet2/common/proto"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/acl/controlplane/aclpb"
	"github.com/yanet-platform/yanet2/modules/fwstate/controlplane/fwstatepb"
)

// ACLService implements the ACL gRPC service
type ACLService struct {
	aclpb.UnimplementedAclServiceServer

	mu              sync.Mutex
	shm             *ffi.SharedMemory
	agents          []*ffi.Agent
	configs         map[instanceKey]*instanceConfig
	gatewayEndpoint string
	log             *zap.SugaredLogger
}

type instanceKey struct {
	name              string
	dataplaneInstance uint32
}

type instanceConfig struct {
	rules         []Rule
	moduleConfig  *ModuleConfig                       // Keep reference to the deployed ACL module config
	fwstateConfig *fwstatepb.GetFwStateConfigResponse // Cached fwstate config from gateway
}

// NewACLService creates a new ACL service
func NewACLService(shm *ffi.SharedMemory, agents []*ffi.Agent, log *zap.SugaredLogger) *ACLService {
	return &ACLService{
		shm:     shm,
		agents:  agents,
		configs: map[instanceKey]*instanceConfig{},
		log:     log,
	}
}

// SetGatewayEndpoint sets the gateway endpoint for inter-module communication
func (s *ACLService) SetGatewayEndpoint(endpoint string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.gatewayEndpoint = endpoint
}

// ListConfigs lists all ACL configurations
func (s *ACLService) ListConfigs(
	ctx context.Context, request *aclpb.ListConfigsRequest,
) (*aclpb.ListConfigsResponse, error) {
	response := &aclpb.ListConfigsResponse{
		InstanceConfigs: make([]*aclpb.InstanceConfigs, len(s.agents)),
	}
	for inst := range s.agents {
		response.InstanceConfigs[inst] = &aclpb.InstanceConfigs{
			Instance: uint32(inst),
		}
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	for key := range s.configs {
		instConfig := response.InstanceConfigs[key.dataplaneInstance]
		instConfig.Configs = append(instConfig.Configs, key.name)
	}

	return response, nil
}

// ShowConfig shows a specific ACL configuration
func (s *ACLService) ShowConfig(
	ctx context.Context, request *aclpb.ShowConfigRequest,
) (*aclpb.ShowConfigResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, err
	}

	key := instanceKey{name: name, dataplaneInstance: inst}
	response := &aclpb.ShowConfigResponse{Instance: inst}

	s.mu.Lock()
	defer s.mu.Unlock()

	if config, ok := s.configs[key]; ok {
		pbConfig := &aclpb.Config{
			Rules: make([]*aclpb.Rule, len(config.rules)),
		}

		// Convert rules (stub)
		for i, rule := range config.rules {
			pbConfig.Rules[i] = &aclpb.Rule{
				Id:     rule.ID,
				Action: rule.Action,
			}
		}

		// Add fwstate map offsets if available
		if config.fwstateConfig != nil {
			pbConfig.Fw4StateOffset = config.fwstateConfig.Fw4StateOffset
			pbConfig.Fw6StateOffset = config.fwstateConfig.Fw6StateOffset
		}

		response.Config = pbConfig
	}

	return response, nil
}

// CompileRules compiles ACL rules (stub implementation)
func (s *ACLService) CompileRules(
	ctx context.Context, request *aclpb.CompileRulesRequest,
) (*aclpb.CompileRulesResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, err
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	key := instanceKey{name: name, dataplaneInstance: inst}

	// Stub: Just store the rules without actual compilation
	rules := make([]Rule, len(request.GetRules()))
	for i, pbRule := range request.GetRules() {
		rules[i] = Rule{
			ID:     pbRule.GetId(),
			Action: pbRule.GetAction(),
		}
	}

	if _, ok := s.configs[key]; !ok {
		s.configs[key] = &instanceConfig{
			rules: rules,
		}
	} else {
		s.configs[key].rules = rules
	}

	s.log.Infow("rules compilation requested (stub)",
		zap.String("name", name),
		zap.Uint32("instance", inst),
		zap.Int("rule_count", len(rules)),
	)

	return &aclpb.CompileRulesResponse{
		Status: "stub: filter compilation not yet implemented",
	}, nil
}

// SyncFWStateConfig is the gRPC handler for synchronizing fwstate config
func (s *ACLService) SyncFWStateConfig(
	ctx context.Context, request *aclpb.SyncFWStateConfigRequest,
) (*aclpb.SyncFWStateConfigResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, err
	}

	fwstateConfigName := request.GetFwstateConfigName()
	if fwstateConfigName == "" {
		return nil, fmt.Errorf("fwstate_config_name is required")
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	s.log.Debugw("sync fwstate config",
		zap.String("module", name),
		zap.Uint32("instance", inst),
		zap.String("fwstate_config", fwstateConfigName),
	)

	// Get fwstate config from gateway
	fwstateResp, err := s.getFwStateConfigFromGateway(fwstateConfigName, inst)
	if err != nil {
		s.log.Errorw("failed to get fwstate config",
			zap.String("module", name),
			zap.Uint32("instance", inst),
			zap.String("fwstate_config", fwstateConfigName),
			zap.Error(err),
		)
		return &aclpb.SyncFWStateConfigResponse{
			Status: fmt.Sprintf("failed to sync fwstate config: %v", err),
		}, err
	}

	s.log.Infow("retrieved fwstate config from gateway",
		zap.String("module", name),
		zap.Uint32("instance", inst),
		zap.String("fwstate_config", fwstateConfigName),
	)

	// Store the fwstate config in the instance config map
	key := instanceKey{name: name, dataplaneInstance: inst}
	if _, ok := s.configs[key]; !ok {
		s.configs[key] = &instanceConfig{
			fwstateConfig: fwstateResp,
		}
	} else {
		s.configs[key].fwstateConfig = fwstateResp
	}

	// Call updateModuleConfig to apply the configuration
	if err := s.updateModuleConfig(name, inst); err != nil {
		return &aclpb.SyncFWStateConfigResponse{
			Status: fmt.Sprintf("failed to sync fwstate config: %v", err),
		}, err
	}

	return &aclpb.SyncFWStateConfigResponse{
		Status: "fwstate config synchronized successfully",
	}, nil
}

// FIXME: add SetRules method that will add simple rules sufficient to test the fwstate implementation, no more.
// This method should be a stub implementation for proving the MVP works

// getFwStateConfigFromGateway gets the fwstate config from the fwstate service via gateway
func (s *ACLService) getFwStateConfigFromGateway(name string, instance uint32) (*fwstatepb.GetFwStateConfigResponse, error) {
	// Connect to gateway
	conn, err := grpc.NewClient(s.gatewayEndpoint, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("failed to connect to gateway: %w", err)
	}
	defer conn.Close()

	// Create fwstate client
	client := fwstatepb.NewFwStateServiceClient(conn)

	// Call GetFwStateConfig
	resp, err := client.GetFwStateConfig(context.Background(), &fwstatepb.GetFwStateConfigRequest{
		Target: &commonpb.TargetModule{
			ConfigName:        name,
			DataplaneInstance: instance,
		},
	})
	if err != nil {
		return nil, fmt.Errorf("failed to get fwstate config: %w", err)
	}

	return resp, nil
}

// updateModuleConfig updates the module configuration in the dataplane
func (s *ACLService) updateModuleConfig(name string, instance uint32) error {
	s.log.Debugw("update config", zap.String("module", name), zap.Uint32("instance", instance))

	agent := s.agents[instance]
	key := instanceKey{name: name, dataplaneInstance: instance}
	instanceCfg := s.configs[key]

	// Create new module config
	config, err := NewModuleConfig(agent, name)
	if err != nil {
		return fmt.Errorf("failed to create %q module config: %w", name, err)
	}

	// Get fwstate config from the instance config map
	if instanceCfg != nil && instanceCfg.fwstateConfig != nil {
		// Set the fwstate config (C function will convert pointers to offsets)
		if err := config.SetFwStateConfig(s.shm.AsRawPtr(), instanceCfg.fwstateConfig); err != nil {
			return fmt.Errorf("failed to set fwstate config for %s: %w", name, err)
		}

		s.log.Debugw("set fwstate config",
			zap.String("module", name),
			zap.Uint32("instance", instance),
		)
	} else {
		s.log.Warnw("no fwstate config available, skipping fwstate setup",
			zap.String("module", name),
			zap.Uint32("instance", instance),
		)
	}

	// TODO: Apply rules

	// Update module in dataplane
	if err := agent.UpdateModules([]ffi.ModuleConfig{config.AsFFIModule()}); err != nil {
		return fmt.Errorf("failed to update module: %w", err)
	}

	// Store the new module config reference for future updates
	if instanceCfg == nil {
		s.configs[key] = &instanceConfig{
			moduleConfig: config,
		}
	} else {
		instanceCfg.moduleConfig = config
	}

	s.log.Infow("successfully updated ACL module",
		zap.String("name", name),
		zap.Uint32("instance", instance),
	)
	return nil
}
