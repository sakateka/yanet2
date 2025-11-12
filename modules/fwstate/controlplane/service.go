package fwstate

import (
	"context"
	"fmt"
	"sync"
	"unsafe"

	"go.uber.org/zap"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/fwstate/controlplane/fwstatepb"
)

// FwStateService implements the FwState gRPC service
type FwStateService struct {
	fwstatepb.UnimplementedFwStateServiceServer

	mu      sync.Mutex
	shm     *ffi.SharedMemory
	agents  []*ffi.Agent
	configs map[instanceKey]*instanceConfig
	log     *zap.SugaredLogger
}

type instanceKey struct {
	name              string
	dataplaneInstance uint32
}

type instanceConfig struct {
	mapConfig       *MapConfig
	syncConfig      *SyncConfig
	moduleConfigRef *ModuleConfig // Keep reference to the deployed fwstate module config
}

// NewFwStateService creates a new FwState service
func NewFwStateService(shm *ffi.SharedMemory, agents []*ffi.Agent, log *zap.SugaredLogger) *FwStateService {
	return &FwStateService{
		shm:     shm,
		agents:  agents,
		configs: map[instanceKey]*instanceConfig{},
		log:     log,
	}
}

// ListConfigs lists all fwstate configurations
func (s *FwStateService) ListConfigs(
	ctx context.Context, request *fwstatepb.ListConfigsRequest,
) (*fwstatepb.ListConfigsResponse, error) {
	response := &fwstatepb.ListConfigsResponse{
		InstanceConfigs: make([]*fwstatepb.InstanceConfigs, len(s.agents)),
	}
	for inst := range s.agents {
		response.InstanceConfigs[inst] = &fwstatepb.InstanceConfigs{
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

// ShowConfig shows a specific fwstate configuration
func (s *FwStateService) ShowConfig(
	ctx context.Context, request *fwstatepb.ShowConfigRequest,
) (*fwstatepb.ShowConfigResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, err
	}

	key := instanceKey{name: name, dataplaneInstance: inst}
	response := &fwstatepb.ShowConfigResponse{Instance: inst}

	s.mu.Lock()
	defer s.mu.Unlock()

	// Always return a config (with defaults if not configured)
	response.Config = &fwstatepb.Config{}

	if config, ok := s.configs[key]; ok {
		if config.mapConfig != nil {
			response.Config.MapConfig = ConvertMapConfigToPb(config.mapConfig)
		}
		if config.syncConfig != nil {
			response.Config.SyncConfig = ConvertSyncConfigToPb(config.syncConfig)
		}

		// Add fwstate map offsets and sizes if module is configured
		if config.moduleConfigRef != nil {
			fwstateConfigWithOffsets, err := config.moduleConfigRef.GetFwStateConfigWithGlobalOffset(s.shm.AsRawPtr())
			if err == nil {
				response.Config.Fw4StateOffset = uint64(uintptr(unsafe.Pointer(fwstateConfigWithOffsets.fw4state)))
				response.Config.Fw6StateOffset = uint64(uintptr(unsafe.Pointer(fwstateConfigWithOffsets.fw6state)))

				// Get map sizes
				response.Config.Fw4StateSize = config.moduleConfigRef.GetMapSize(false)
				response.Config.Fw6StateSize = config.moduleConfigRef.GetMapSize(true)
			} else {
				s.log.Warnw("failed to get fwstate map offsets",
					zap.String("name", name),
					zap.Uint32("instance", inst),
					zap.Error(err),
				)
			}
		}
	}

	// Provide default map config if not set
	if response.Config.MapConfig == nil {
		defaultMapConfig := DefaultMapConfig()
		response.Config.MapConfig = ConvertMapConfigToPb(&defaultMapConfig)
	}

	// Provide default sync config if not set
	if response.Config.SyncConfig == nil {
		defaultSyncConfig := DefaultSyncConfig()
		response.Config.SyncConfig = ConvertSyncConfigToPb(&defaultSyncConfig)
	}

	return response, nil
}

// SetConfig configures firewall state settings
func (s *FwStateService) SetConfig(
	ctx context.Context, request *fwstatepb.SetConfigRequest,
) (*fwstatepb.SetConfigResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, err
	}

	// Convert protobuf configs to internal format
	var mapConfig *MapConfig
	var syncConfig *SyncConfig

	if pbMapCfg := request.GetMapConfig(); pbMapCfg != nil {
		mapConfig = ConvertPbToMapConfig(pbMapCfg)
	}

	if pbSyncCfg := request.GetSyncConfig(); pbSyncCfg != nil {
		syncConfig = ConvertPbToSyncConfig(pbSyncCfg)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	key := instanceKey{name: name, dataplaneInstance: inst}

	// Prepare module config (this can fail)
	newModuleConfigRef, err := s.prepareModuleConfig(name, inst, mapConfig, syncConfig)
	if err != nil {
		s.log.Errorw("failed to prepare module config",
			zap.String("name", name),
			zap.Uint32("instance", inst),
			zap.Error(err),
		)
		return nil, err
	}

	// Only store config after successful preparation
	if _, ok := s.configs[key]; !ok {
		s.configs[key] = &instanceConfig{
			mapConfig:       mapConfig,
			syncConfig:      syncConfig,
			moduleConfigRef: newModuleConfigRef,
		}
	} else {
		if mapConfig != nil {
			s.configs[key].mapConfig = mapConfig
		}
		if syncConfig != nil {
			s.configs[key].syncConfig = syncConfig
		}
		s.configs[key].moduleConfigRef = newModuleConfigRef
	}

	return &fwstatepb.SetConfigResponse{}, nil
}

// GetFwStateConfig returns the fwstate configuration with map offsets calculated
// from the shared memory base. This allows other modules in different processes
// to access the same maps by adding the offsets to their own shm base address.
func (s *FwStateService) GetFwStateConfig(
	ctx context.Context, request *fwstatepb.GetFwStateConfigRequest,
) (*fwstatepb.GetFwStateConfigResponse, error) {
	name, inst, err := request.GetTarget().Validate(uint32(len(s.agents)))
	if err != nil {
		return nil, err
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	key := instanceKey{name: name, dataplaneInstance: inst}
	config, ok := s.configs[key]
	if !ok || config.moduleConfigRef == nil {
		return nil, status.Error(codes.NotFound, "fwstate module not configured")
	}

	// Get fwstate config with global offsets from shm base
	fwstateConfigWithOffsets, err := config.moduleConfigRef.GetFwStateConfigWithGlobalOffset(s.shm.AsRawPtr())
	if err != nil {
		return nil, status.Errorf(codes.Internal, "failed to get fwstate config with offsets: %v", err)
	}

	// Convert the C struct to protobuf response
	// The fw4state and fw6state fields are already offsets from shm base
	response := &fwstatepb.GetFwStateConfigResponse{
		Fw4StateOffset: uint64(uintptr(unsafe.Pointer(fwstateConfigWithOffsets.fw4state))),
		Fw6StateOffset: uint64(uintptr(unsafe.Pointer(fwstateConfigWithOffsets.fw6state))),
	}

	// Copy sync config if present
	if config.syncConfig != nil {
		response.SyncConfig = ConvertSyncConfigToPb(config.syncConfig)
	}

	return response, nil
}

// prepareModuleConfig prepares and deploys the module configuration in the dataplane
func (s *FwStateService) prepareModuleConfig(name string, instance uint32, mapConfig *MapConfig, syncConfig *SyncConfig) (*ModuleConfig, error) {
	s.log.Debugw("prepare module config", zap.String("module", name), zap.Uint32("instance", instance))

	agent := s.agents[instance]
	key := instanceKey{name: name, dataplaneInstance: instance}
	// Create new module config
	config, err := NewModuleConfig(agent, name)
	if err != nil {
		return nil, fmt.Errorf("failed to create %q module config: %w", name, err)
	}

	existingConfig := s.configs[key]
	if existingConfig != nil && existingConfig.moduleConfigRef != nil {
		// Transfer existing maps from old config to new config
		// Maps are stored as offsets, so they remain valid across reconfigurations
		config.TransferMaps(existingConfig.moduleConfigRef)
		s.log.Debugw("transferred firewall state maps",
			zap.String("module", name),
			zap.Uint32("instance", instance),
		)
		// FIXME: rotate maps if the mapConfig is changed
	} else {
		// No old maps to transfer, create new maps
		// Get map configuration or use defaults
		var indexSize, extraBucketCount uint32
		if mapConfig != nil {
			indexSize = mapConfig.IndexSize
			extraBucketCount = mapConfig.ExtraBucketCount
		} else if existingConfig != nil && existingConfig.mapConfig != nil {
			indexSize = existingConfig.mapConfig.IndexSize
			extraBucketCount = existingConfig.mapConfig.ExtraBucketCount
		}

		// Get worker count from agent's dataplane config
		dpConfig := agent.DPConfig()
		workerCount := uint16(dpConfig.WorkerCount())

		if err := config.CreateMaps(indexSize, extraBucketCount, workerCount); err != nil {
			return nil, fmt.Errorf("failed to create fwstate maps: %w", err)
		}
		s.log.Infow("created new firewall state maps",
			zap.String("module", name),
			zap.Uint32("instance", instance),
			zap.Uint32("index_size", indexSize),
			zap.Uint32("extra_bucket_count", extraBucketCount),
			zap.Uint16("worker_count", workerCount),
		)
	}

	// Apply sync configuration if present
	finalSyncConfig := syncConfig
	if finalSyncConfig == nil && existingConfig != nil {
		finalSyncConfig = existingConfig.syncConfig
	}
	if finalSyncConfig != nil {
		if err := config.SetSyncConfig(finalSyncConfig); err != nil {
			return nil, fmt.Errorf("failed to set sync config: %w", err)
		}
		s.log.Debugw("applied sync configuration",
			zap.String("module", name),
			zap.Uint32("instance", instance),
		)
	}

	// Update module in dataplane
	if err := agent.UpdateModules([]ffi.ModuleConfig{config.AsFFIModule()}); err != nil {
		return nil, fmt.Errorf("failed to update module: %w", err)
	}

	s.log.Infow("successfully updated fwstate module",
		zap.String("name", name),
		zap.Uint32("instance", instance),
	)
	return config, nil
}
