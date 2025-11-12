use clap::{Parser, ValueEnum};

#[derive(Debug, Clone, Parser)]
pub enum ModeCmd {
    Show(ShowConfigCmd),
    SyncFwstateConfig(SyncFwstateConfigCmd),
}

#[derive(Debug, Clone, Parser)]
pub struct ShowConfigCmd {
    /// ACL module name to operate on.
    #[arg(long = "cfg", short)]
    pub config_name: Option<String>,
    /// Instance where the changes should be applied, optionally
    /// repeated.
    #[arg(long, required = false)]
    pub instances: Vec<u32>,
    /// Output format.
    #[clap(long, value_enum, default_value_t = OutputFormat::Tree)]
    pub format: OutputFormat,
}

#[derive(Debug, Clone, Parser)]
pub struct SyncFwstateConfigCmd {
    /// ACL module name to sync fwstate config for.
    #[arg(long = "cfg", short)]
    pub config_name: String,
    /// FwState config name to sync from.
    #[arg(long = "fwstate-cfg")]
    pub fwstate_config_name: String,
    /// Instance where the sync should be applied, optionally
    /// repeated. If not specified, syncs all instances.
    #[arg(long, required = false)]
    pub instances: Vec<u32>,
}

/// Output format options.
#[derive(Debug, Clone, ValueEnum)]
pub enum OutputFormat {
    /// Tree structure with colored output (default).
    Tree,
    /// JSON format.
    Json,
}
