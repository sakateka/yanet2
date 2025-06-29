package stage

// DataplaneInstanceIdx is the index of a dataplane instance.
type DataplaneInstanceIdx uint32

// Config represents a stage config in the configuration process.
//
// Each stage is a single configuration block that describes both modules with
// their configurations and the pipeline.
// A stage is applied atomically.
type Config struct {
	// Name is the unique identifier for this stage.
	Name string `yaml:"name"`
	// Configuration for each dataplane instance.
	Instances map[DataplaneInstanceIdx]DpInstanceConfig `yaml:"instance"`
}

// DpInstanceConfig contains the configuration for a specific dataplane instance.
type DpInstanceConfig struct {
	// Modules configuration for this dataplane instance.
	Modules map[string]ModuleConfig `yaml:"modules,omitempty"`
	// Pipelines configuration for this dataplane instance.
	Pipelines []PipelineConfig `yaml:"pipelines,omitempty"`
	// Devices configuration for this dataplane instance.
	Devices []DeviceConfig `yaml:"devices,omitempty"`
}

// ModuleConfig contains configuration for a specific module.
type ModuleConfig struct {
	// ConfigName is the name of the configuration for this module.
	ConfigName string `yaml:"config_name"`
	// ConfigPath is the path to the module configuration file.
	ConfigPath string `yaml:"config_path"`
}

// PipelineConfig represents a pipeline configuration.
type PipelineConfig struct {
	// Name is the unique identifier for this pipeline.
	Name string `yaml:"name"`
	// Chain define the processing chain in this pipeline.
	Chain []NodeConfig `yaml:"chain"`
}

// NodeConfig represents a module node in a processing chain.
type NodeConfig struct {
	// ModuleName is the name of the module.
	ModuleName string `yaml:"module_name"`
	// ConfigName is the configuration name for this module.
	ConfigName string `yaml:"config_name"`
}

// DeviceConfig represents a device configuration.
type DeviceConfig struct {
	// ID is the ID of the device.
	ID uint32 `yaml:"id"`
	// Pipelines is the list of pipelines to assign to the device.
	Pipelines []DevicePipelineConfig `yaml:"pipelines"`
}

// DevicePipelineConfig represents a pipeline configuration for a device.
type DevicePipelineConfig struct {
	// Name is the name of the pipeline.
	Name string `yaml:"name"`
	// Weight is the weight of the pipeline.
	Weight uint64 `yaml:"weight"`
}
