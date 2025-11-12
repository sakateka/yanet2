use core::error::Error;

use clap::{ArgAction, CommandFactory, Parser};
use clap_complete::CompleteEnv;
use ptree::TreeBuilder;
use tonic::transport::Channel;
use ync::logging;

use aclpb::{
    ListConfigsRequest, ShowConfigRequest, ShowConfigResponse, SyncFwStateConfigRequest,
    acl_service_client::AclServiceClient,
};
use args::{ModeCmd, OutputFormat, ShowConfigCmd, SyncFwstateConfigCmd};
use commonpb::TargetModule;

mod args;

#[allow(non_snake_case)]
pub mod aclpb {
    use serde::Serialize;

    tonic::include_proto!("aclpb");
}

#[allow(non_snake_case)]
pub mod commonpb {
    use serde::Serialize;

    tonic::include_proto!("commonpb");
}

/// ACL module - manage Access Control Lists.
#[derive(Debug, Clone, Parser)]
#[command(version, about)]
#[command(flatten_help = true)]
pub struct Cmd {
    #[clap(subcommand)]
    pub mode: ModeCmd,
    /// Gateway endpoint.
    #[clap(long, default_value = "grpc://[::1]:8080", global = true)]
    pub endpoint: String,
    /// Log verbosity level.
    #[clap(short, action = ArgAction::Count, global = true)]
    pub verbose: u8,
}

#[tokio::main(flavor = "current_thread")]
pub async fn main() {
    CompleteEnv::with_factory(Cmd::command).complete();
    let cmd = Cmd::parse();
    logging::init(cmd.verbose as usize).expect("initialize logging");

    if let Err(err) = run(cmd).await {
        log::error!("ERROR: {err}");
        std::process::exit(1);
    }
}

async fn run(cmd: Cmd) -> Result<(), Box<dyn Error>> {
    let mut service = AclService::new(cmd.endpoint).await?;

    match cmd.mode {
        ModeCmd::Show(cmd) => service.show_config(cmd).await,
        ModeCmd::SyncFwstateConfig(cmd) => service.sync_fwstate_config(cmd).await,
    }
}

pub struct AclService {
    client: AclServiceClient<Channel>,
}

impl AclService {
    pub async fn new(endpoint: String) -> Result<Self, Box<dyn Error>> {
        let client = AclServiceClient::connect(endpoint).await?;
        Ok(Self { client })
    }

    async fn get_configs(
        &mut self,
        name: &str,
        instances: Vec<u32>,
    ) -> Result<Vec<ShowConfigResponse>, Box<dyn Error>> {
        let mut responses = Vec::new();
        for instance in instances {
            let request = ShowConfigRequest {
                target: Some(TargetModule {
                    config_name: name.to_owned(),
                    dataplane_instance: instance,
                }),
            };
            log::trace!("show config request on dataplane instance {instance}: {request:?}");
            let response = self.client.show_config(request).await?.into_inner();
            log::debug!("show config response on dataplane instance {instance}: {response:?}");
            responses.push(response);
        }
        Ok(responses)
    }

    pub async fn show_config(&mut self, cmd: ShowConfigCmd) -> Result<(), Box<dyn Error>> {
        let Some(name) = cmd.config_name else {
            self.print_config_list().await?;
            return Ok(());
        };

        let mut instances = cmd.instances;
        if instances.is_empty() {
            instances = self.get_dataplane_instances().await?;
        }
        let configs = self.get_configs(&name, instances).await?;

        match cmd.format {
            OutputFormat::Json => print_json(configs)?,
            OutputFormat::Tree => print_tree(configs)?,
        }

        Ok(())
    }

    async fn get_dataplane_instances(&mut self) -> Result<Vec<u32>, Box<dyn Error>> {
        let request = ListConfigsRequest {};
        let response = self.client.list_configs(request).await?.into_inner();
        Ok(response.instance_configs.iter().map(|c| c.instance).collect())
    }

    async fn print_config_list(&mut self) -> Result<(), Box<dyn Error>> {
        let request = ListConfigsRequest {};
        let response = self.client.list_configs(request).await?.into_inner();
        let mut tree = TreeBuilder::new("ACL Configs".to_string());
        for instance_config in response.instance_configs {
            tree.begin_child(format!("Instance {}", instance_config.instance));
            for config in instance_config.configs {
                tree.add_empty_child(config);
            }
        }
        let tree = tree.build();
        ptree::print_tree(&tree)?;
        Ok(())
    }

    pub async fn sync_fwstate_config(&mut self, cmd: SyncFwstateConfigCmd) -> Result<(), Box<dyn Error>> {
        let mut instances = cmd.instances;
        if instances.is_empty() {
            instances = self.get_dataplane_instances().await?;
        }

        for instance in instances {
            let request = SyncFwStateConfigRequest {
                target: Some(TargetModule {
                    config_name: cmd.config_name.clone(),
                    dataplane_instance: instance,
                }),
                fwstate_config_name: cmd.fwstate_config_name.clone(),
            };
            log::trace!("sync fwstate config request on dataplane instance {instance}: {request:?}");
            let response = self.client.sync_fw_state_config(request).await?.into_inner();
            log::info!(
                "sync fwstate config response on dataplane instance {instance}: {}",
                response.status
            );
            println!("Instance {}: {}", instance, response.status);
        }

        Ok(())
    }
}

pub fn print_json(resp: Vec<ShowConfigResponse>) -> Result<(), Box<dyn Error>> {
    println!("{}", serde_json::to_string(&resp)?);
    Ok(())
}

pub fn print_tree(configs: Vec<ShowConfigResponse>) -> Result<(), Box<dyn Error>> {
    let mut tree = TreeBuilder::new("ACL Configs".to_string());

    for resp in &configs {
        tree.begin_child(format!("Instance {}", resp.instance));

        if let Some(config) = &resp.config {
            // Display fwstate map offsets
            tree.begin_child("FwState Map Global(Shm) Offsets".to_string());
            if config.fw4state_offset != 0 {
                tree.add_empty_child(format!("IPv4: 0x{:x}", config.fw4state_offset));
            } else {
                tree.add_empty_child("IPv4: null".to_string());
            }
            if config.fw6state_offset != 0 {
                tree.add_empty_child(format!("IPv6: 0x{:x}", config.fw6state_offset));
            } else {
                tree.add_empty_child("IPv6: null".to_string());
            }
            tree.end_child();

            if !config.rules.is_empty() {
                tree.begin_child("Rules".to_string());
                for rule in &config.rules {
                    tree.add_empty_child(format!("Rule {}: action={}", rule.id, rule.action));
                }
                tree.end_child();
            }
        }

        tree.end_child();
    }

    let tree = tree.build();
    ptree::print_tree(&tree)?;

    Ok(())
}
