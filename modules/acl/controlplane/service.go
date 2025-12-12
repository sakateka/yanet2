package acl

import (
	"context"
	"net/netip"
	"sync"

	"go.uber.org/zap"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/acl/controlplane/aclpb"
)

////////////////////////////////////////////////////////////////////////////////

// ACLService implements the gRPC service for ACL management.
type ACLService struct {
	aclpb.UnimplementedACLServiceServer

	mu      sync.Mutex
	shm     *ffi.SharedMemory
	agents  []*ffi.Agent
	configs map[instanceKey]*instanceConfig

	log *zap.SugaredLogger
}

// NewACLService creates a new ACL service
func NewACLService(shm *ffi.SharedMemory, agents []*ffi.Agent, log *zap.SugaredLogger) *ACLService {
	return &ACLService{
		shm:     shm,
		agents:  agents,
		configs: make(map[instanceKey]*instanceConfig),
		log:     log,
	}
}

////////////////////////////////////////////////////////////////////////////////

type instanceKey struct {
	name     string
	instance uint32
}

type instanceConfig struct {
	acl     *ModuleConfig
	fwstate *FwStateConfig
	// rules         []aclRule      // Keep reference to ACL rules
}

////////////////////////////////////////////////////////////////////////////////

func (m *ACLService) UpdateConfig(
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
				return nil, status.Error(codes.InvalidArgument, "invalid network address length")
			}

			addr, _ := netip.AddrFromSlice(reqSrc.Ip)
			rule.srcs = append(rule.srcs, netip.PrefixFrom(addr, int(reqSrc.PrefixLen)))
		}

		for _, reqDst := range reqRule.Dsts {
			if len(reqDst.Ip) != 4 && len(reqDst.Ip) != 16 {
				return nil, status.Error(codes.InvalidArgument, "invalid network address length")
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
		return nil, status.Errorf(codes.InvalidArgument, "invalid instance id: %d", inst)
	}
	agent := m.agents[inst]
	key := instanceKey{name: name, instance: inst}

	config, err := NewModuleConfig(agent, name)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "failed to create module config: %v", err)
	}

	if err := config.Update(rules); err != nil {
		return nil, status.Errorf(codes.Internal, "failed to update module config: %v", err)
	}

	oldConfigs, ok := m.configs[key]
	if ok && oldConfigs.fwstate != nil {
		// We set fwstate config only if it's already present,
		// effectively enabling firewall state tracking functionality
		m.log.Infow("set fwstate config for ACL module config",
			zap.String("config", name),
			zap.Uint32("instance", inst),
		)
		config.SetFwStateConfig(agent, oldConfigs.fwstate)
	}

	if err := agent.UpdateModules([]ffi.ModuleConfig{config.AsFFIModule()}); err != nil {
		return nil, status.Errorf(codes.Internal, "failed to update module on instance %d: %v", inst, err)
	}

	m.configs[key] = &instanceConfig{
		acl:     config,
		fwstate: oldConfigs.fwstate,
	}

	return &aclpb.UpdateConfigResponse{}, nil
}

// SyncFWStateConfig is the gRPC handler for synchronizing fwstate config
func (s *ACLService) UpdateFWStateConfig(
	ctx context.Context, request *aclpb.UpdateFWStateConfigRequest,
) (*aclpb.UpdateFWStateConfigResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	// Get fwstate configuration from request
	if request.SyncConfig == nil {
		return nil, status.Error(codes.InvalidArgument, "sync_config is required")
	}
	if request.MapConfig == nil {
		return nil, status.Error(codes.InvalidArgument, "map_config is required")
	}

	s.log.Debugw("update fwstate config",
		zap.String("config", name),
		zap.Uint32("instance", inst),
	)

	agent := s.agents[inst]
	key := instanceKey{name: name, instance: inst}

	s.mu.Lock()
	defer s.mu.Unlock()

	config, exists := s.configs[key]
	if !exists {
		config = &instanceConfig{}
		s.configs[key] = config
	}

	config.fwstate, err = NewFWStateModuleConfig(agent, name, config.fwstate)
	if err != nil {
		s.log.Errorw("failed to create fwstate config",
			zap.String("config", name),
			zap.Uint32("instance", inst),
			zap.Error(err),
		)
		return nil, status.Errorf(codes.Internal, "failed to create fwstate config: %v", err)
	}

	if err = config.fwstate.CreateMaps(request.MapConfig, uint16(len(s.agents)), s.log); err != nil {
		s.log.Errorw("failed to create fwstate maps",
			zap.String("config", name),
			zap.Uint32("instance", inst),
			zap.Error(err),
		)
		return nil, status.Errorf(codes.Internal, "failed to create fwstate maps: %v", err)
	}

	// Set sync config
	config.fwstate.SetSyncConfig(request.SyncConfig)
	s.log.Debugw("update fwstate module config",
		zap.String("config", name),
		zap.Uint32("instance", inst),
	)

	// FIXME: update acl module with the fwstate if ACL config is presnet
	// - copy acl config
	// - update acl to the new copy
	// - FIXME(free): look at the free function of the acl config module functions

	// Call updateModuleConfig to apply the configuration to ACL module
	// Update module in dataplane
	if err := agent.UpdateModules([]ffi.ModuleConfig{config.fwstate.asFFIModule()}); err != nil {
		s.log.Errorw("failed to update fwstate module",
			zap.String("config", name),
			zap.Uint32("instance", inst),
			zap.Error(err),
		)
		return nil, status.Errorf(codes.Internal, "failed to update fwstate module: %v", err)
	}

	s.log.Infow("successfully updated FWState module",
		zap.String("config", name),
		zap.Uint32("instance", inst),
	)

	return &aclpb.UpdateFWStateConfigResponse{}, nil
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
		instConfig := response.InstanceConfigs[key.instance]
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
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	key := instanceKey{name: name, instance: inst}
	response := &aclpb.ShowConfigResponse{Instance: inst}

	s.mu.Lock()
	defer s.mu.Unlock()

	config, ok := s.configs[key]
	if !ok {
		return nil, status.Errorf(codes.NotFound, "config %q not found on instance %d", name, inst)
	}

	// Rules stub - empty for now
	response.Rules = &aclpb.Config{
		Rules: make([]*aclpb.Rule, 0),
	}

	// Get fwstate configuration if available
	if config.fwstate != nil {
		response.FwstateMap = config.fwstate.GetMapConfig()
		response.FwstateSync = config.fwstate.GetSyncConfig()
	}

	return response, nil
}
