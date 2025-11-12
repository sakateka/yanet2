package acl

import (
	"context"
	"fmt"
	"sync"

	"net/netip"

	"go.uber.org/zap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	commonpb "github.com/yanet-platform/yanet2/common/proto"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/acl/controlplane/aclpb"
	"github.com/yanet-platform/yanet2/modules/fwstate/controlplane/fwstatepb"
)

////////////////////////////////////////////////////////////////////////////////

// AclService implements the ACL gRPC service
type AclService struct {
	aclpb.UnimplementedAclServiceServer

	mu              sync.Mutex
	shm             *ffi.SharedMemory
	agents          []*ffi.Agent
	configs         map[instanceKey]*instanceConfig
	gatewayEndpoint string

	log *zap.SugaredLogger
}

// NewAclService creates a new ACL service
func NewAclService(shm *ffi.SharedMemory, agents []*ffi.Agent, log *zap.SugaredLogger) *AclService {
	return &AclService{
		shm:     shm,
		agents:  agents,
		log:     log,
		configs: make(map[instanceKey]*instanceConfig),
	}
}

// SetGatewayEndpoint sets the gateway endpoint for inter-module communication
func (s *AclService) SetGatewayEndpoint(endpoint string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.gatewayEndpoint = endpoint
}

////////////////////////////////////////////////////////////////////////////////

type instanceKey struct {
	name     string
	instance uint32
}

type instanceConfig struct {
	moduleConfig  *ModuleConfig                       // Keep reference to the deployed ACL module config
	fwstateConfig *fwstatepb.GetFwStateConfigResponse // Cached fwstate config from gateway
}

////////////////////////////////////////////////////////////////////////////////

func (m *AclService) UpdateConfig(
	ctx context.Context,
	req *aclpb.UpdateConfigRequest,
) (*aclpb.UpdateConfigResponse, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	name, inst, err := req.GetTarget().Validate(uint32(len(m.agents)))
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	reqRules := req.Rules

	rules := make([]aclRule, 0, len(reqRules))
	for _, reqRule := range reqRules {
		rule := aclRule{
			counter:       reqRule.Counter,
			devices:       reqRule.Devices,
			vlanRanges:    make([]vlanRange, 0, len(reqRule.VlanRanges)),
			srcs:          make([]netip.Prefix, 0, len(reqRule.Srcs)),
			dsts:          make([]netip.Prefix, 0, len(reqRule.Dsts)),
			protoRanges:   make([]protoRange, 0, len(reqRule.ProtoRanges)),
			srcPortRanges: make([]portRange, 0, len(reqRule.SrcPortRanges)),
			dstPortRanges: make([]portRange, 0, len(reqRule.DstPortRanges)),
		}

		if reqRule.Action == aclpb.ActionKind_ACTION_KIND_PASS {
			rule.action = 0
		} else {
			rule.action = 1
		}

		for _, reqVlanRange := range reqRule.VlanRanges {
			rule.vlanRanges = append(rule.vlanRanges, vlanRange{
				from: uint16(reqVlanRange.From),
				to:   uint16(reqVlanRange.To),
			})
		}

		for _, reqSrc := range reqRule.Srcs {
			if len(reqSrc.Ip) != 4 && len(reqSrc.Ip) != 16 {
				return nil, fmt.Errorf("invalid network address length")
			}

			addr, _ := netip.AddrFromSlice(reqSrc.Ip)
			rule.srcs = append(rule.srcs, netip.PrefixFrom(addr, int(reqSrc.PrefixLen)))
		}

		for _, reqDst := range reqRule.Dsts {
			if len(reqDst.Ip) != 4 && len(reqDst.Ip) != 16 {
				return nil, fmt.Errorf("invalid network address length")
			}

			addr, _ := netip.AddrFromSlice(reqDst.Ip)
			rule.dsts = append(rule.dsts, netip.PrefixFrom(addr, int(reqDst.PrefixLen)))
		}

		for _, reqProtoRange := range reqRule.ProtoRanges {
			rule.protoRanges = append(rule.protoRanges, protoRange{
				from: uint16(reqProtoRange.From),
				to:   uint16(reqProtoRange.To),
			})
		}

		for _, reqSrcPortRange := range reqRule.SrcPortRanges {
			rule.srcPortRanges = append(rule.srcPortRanges, portRange{
				from: uint16(reqSrcPortRange.From),
				to:   uint16(reqSrcPortRange.To),
			})
		}

		for _, reqDstPortRange := range reqRule.DstPortRanges {
			rule.dstPortRanges = append(rule.dstPortRanges, portRange{
				from: uint16(reqDstPortRange.From),
				to:   uint16(reqDstPortRange.To),
			})
		}

		rules = append(rules, rule)
	}

	if inst >= uint32(len(m.agents)) {
		return nil, fmt.Errorf("invalid instance id")
	}
	agent := m.agents[inst]

	module, err := NewModuleConfig(agent, name)
	if err != nil {
		return nil, fmt.Errorf("failed to create module config: %w", err)

	}

	if err := module.Update(rules); err != nil {
		return nil, fmt.Errorf("failed to update module config: %w", err)
	}

	// FIXME! sync with updateModuleConfig
	if err := agent.UpdateModules([]ffi.ModuleConfig{module.AsFFIModule()}); err != nil {
		return nil, fmt.Errorf("failed to update module on instance %d: %w", inst, err)
	}

	m.log.Infow("successfully update acl config",
		zap.String("name", name),
		zap.Uint32("instance", inst),
	)

	return &aclpb.UpdateConfigResponse{}, nil
}

// SyncFWStateConfig is the gRPC handler for synchronizing fwstate config
func (s *AclService) SyncFWStateConfig(
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
	key := instanceKey{name: name, instance: inst}
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

// getFwStateConfigFromGateway gets the fwstate config from the fwstate service via gateway
func (s *AclService) getFwStateConfigFromGateway(name string, instance uint32) (*fwstatepb.GetFwStateConfigResponse, error) {
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

// ListConfigs lists all ACL configurations
func (s *AclService) ListConfigs(
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
		instConfig := response.InstanceConfigs[key.instance]
		instConfig.Configs = append(instConfig.Configs, key.name)
	}

	return response, nil
}

// ShowConfig shows a specific ACL configuration
func (s *AclService) ShowConfig(
	ctx context.Context, request *aclpb.ShowConfigRequest,
) (*aclpb.ShowConfigResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, err
	}

	key := instanceKey{name: name, instance: inst}
	response := &aclpb.ShowConfigResponse{Instance: inst}

	s.mu.Lock()
	defer s.mu.Unlock()

	if config, ok := s.configs[key]; ok {
		pbConfig := &aclpb.Config{
			Rules: make([]*aclpb.Rule, len(config.rules)),
		}

		/*
			// FIXME: Convert rules (stub)
			for i, rule := range config.rules {
				pbConfig.Rules[i] = &aclpb.Rule{
					Id:     rule.ID,
					Action: rule.Action,
				}
			}
		*/

		// Add fwstate map offsets if available
		if config.fwstateConfig != nil {
			pbConfig.Fw4StateOffset = config.fwstateConfig.Fw4StateOffset
			pbConfig.Fw6StateOffset = config.fwstateConfig.Fw6StateOffset
		}

		response.Config = pbConfig
	}

	return response, nil
}

// updateModuleConfig updates the module configuration in the dataplane
func (s *AclService) updateModuleConfig(name string, instance uint32) error {
	s.log.Debugw("update config", zap.String("module", name), zap.Uint32("instance", instance))

	agent := s.agents[instance]
	key := instanceKey{name: name, instance: instance}
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

	// FIXME: Apply rules

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
