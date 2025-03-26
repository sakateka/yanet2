package yncp

import (
	"context"
	"fmt"
	"reflect"

	"go.uber.org/zap"
	"golang.org/x/sync/errgroup"

	"github.com/yanet-platform/yanet2/controlplane/internal/gateway"
	"github.com/yanet-platform/yanet2/controlplane/modules/decap"
	"github.com/yanet-platform/yanet2/controlplane/modules/route"
)

type ConfigReloader func() (*Config, error)

type options struct {
	Log            *zap.SugaredLogger
	LogLevel       *zap.AtomicLevel
	ConfigReloader ConfigReloader
}

func newOptions() *options {
	return &options{
		Log: zap.NewNop().Sugar(),
	}
}

// DirectorOption is a function that configures the YANET controlplane
// director.
type DirectorOption func(*options)

// WithLog sets the logger for the YANET controlplane director.
func WithLog(log *zap.SugaredLogger) DirectorOption {
	return func(o *options) {
		o.Log = log
	}
}

// WithAtomicLogLevel sets the atomic logger level for the YANET controlplane
// director.
//
// This level can be changed at runtime.
func WithAtomicLogLevel(level *zap.AtomicLevel) DirectorOption {
	return func(o *options) {
		o.LogLevel = level
	}
}

// WithConfigReloader sets the function to reload the entire Director's configuration
//
// Config reloading does not recreate, load, or unload modules.
func WithConfigReloader(reloader ConfigReloader) DirectorOption {
	return func(o *options) {
		o.ConfigReloader = reloader
	}
}

// Director is the YANET controlplane director.
//
// This is an entry point for the YANET controlplane. Its main purposes is to
// initialize basic configuration, set up the Gateway API, sidecar gRPC
// services and run them.
type Director struct {
	cfg     *Config
	gateway *gateway.Gateway
	log     *zap.SugaredLogger
}

// NewDirector creates a new YANET controlplane director using specified
// config.
func NewDirector(cfg *Config, options ...DirectorOption) (*Director, error) {
	opts := newOptions()
	for _, o := range options {
		o(opts)
	}

	log := opts.Log
	log.Infof("initializing YANET controlplane ...")
	log.Debugw("parsed config", zap.Any("config", cfg))

	routeModule, err := route.NewRouteModule(cfg.Modules.Route, log)
	if err != nil {
		return nil, fmt.Errorf("failed to initialize route built-in module: %w", err)
	}

	moduleConfigReloader := newModuleConfigReloader[decap.Config](cfg, opts.ConfigReloader, log.With("name", "decap-config-reloader"))
	decapModule, err := decap.NewDecapModule(cfg.Modules.Decap, moduleConfigReloader, log)
	if err != nil {
		return nil, fmt.Errorf("failed to initialize decap built-in module: %w", err)
	}

	gw := gateway.NewGateway(
		cfg.Gateway,
		gateway.WithBuiltInModule(
			routeModule,
		),
		gateway.WithBuiltInModule(
			decapModule,
		),
		gateway.WithLog(log),
		gateway.WithAtomicLogLevel(opts.LogLevel),
	)

	return &Director{
		cfg:     cfg,
		gateway: gw,
		log:     log,
	}, nil
}

// Close closes the YANET controlplane director.
func (m *Director) Close() error {
	return m.gateway.Close()
}

// Run runs the YANET controlplane director.
func (m *Director) Run(ctx context.Context) error {
	// Serve.
	wg, ctx := errgroup.WithContext(ctx)
	wg.Go(func() error {
		return m.gateway.Run(ctx)
	})

	return wg.Wait()
}

func newModuleConfigReloader[C any](cfg *Config, reloader ConfigReloader, log *zap.SugaredLogger) func(*C) error {
	return func(moduleConfig *C) error {
		targetValue := reflect.ValueOf(moduleConfig)
		if !targetValue.CanSet() {
			return fmt.Errorf("cannot set target module config type: %v", targetValue.Type())
		}

		newCfg := cfg
		var err error
		if reloader != nil {
			newCfg, err = reloader()
		} else {
			log.Warn("config reloader is not defined, operating on current config")
		}
		if err != nil {
			return err
		}

		oldModulesValue := reflect.ValueOf(cfg.Modules)
		newModulesValue := reflect.ValueOf(newCfg.Modules)
		modulesTypeS := newModulesValue.Type()

		for idx := range modulesTypeS.NumField() {
			fieldValue := newModulesValue.Field(idx)

			if fieldValue.Type() != targetValue.Type() {
				continue
			}

			// Update runtime config
			oldModulesValue.Field(idx).Set(fieldValue)
			// Set result config requested by user
			targetValue.Set(fieldValue)
		}
		return fmt.Errorf("cannot find or set module config type: %v", targetValue.Type())
	}
}
