use core::error::Error;

use clap::{ArgAction, CommandFactory, Parser};
use clap_complete::CompleteEnv;
use ptree::TreeBuilder;
use tonic::transport::Channel;
use ync::logging;

use args::{ModeCmd, OutputFormat, SetConfigCmd, ShowConfigCmd};
use commonpb::TargetModule;
use fwstatepb::{
    ListConfigsRequest, SetConfigRequest, ShowConfigRequest, ShowConfigResponse,
    fw_state_service_client::FwStateServiceClient,
};

mod args;
mod format;

#[allow(non_snake_case)]
pub mod fwstatepb {
    use serde::Serialize;

    tonic::include_proto!("fwstatepb");
}

#[allow(non_snake_case)]
pub mod commonpb {
    use serde::Serialize;

    tonic::include_proto!("commonpb");
}

/// FwState module - manage firewall state synchronization and timeouts.
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
    let mut service = FwStateService::new(cmd.endpoint).await?;

    match cmd.mode {
        ModeCmd::Show(cmd) => service.show_config(cmd).await,
        ModeCmd::Set(cmd) => service.set_config(cmd).await,
    }
}

pub struct FwStateService {
    client: FwStateServiceClient<Channel>,
}

impl FwStateService {
    pub async fn new(endpoint: String) -> Result<Self, Box<dyn Error>> {
        let client = FwStateServiceClient::connect(endpoint).await?;
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
        let mut tree = TreeBuilder::new("FwState Configs".to_string());
        for instance_config in response.instance_configs {
            tree.begin_child(format!("Instance {}", instance_config.instance));
            for config in instance_config.configs {
                tree.add_empty_child(config);
            }
            tree.end_child();
        }
        let tree = tree.build();
        ptree::print_tree(&tree)?;
        Ok(())
    }

    pub async fn set_config(&mut self, cmd: SetConfigCmd) -> Result<(), Box<dyn Error>> {
        for inst in cmd.instances.clone() {
            // First, fetch the current config to merge with new values
            let current_request = ShowConfigRequest {
                target: Some(TargetModule {
                    config_name: cmd.config_name.clone(),
                    dataplane_instance: inst,
                }),
            };
            let current_response = self.client.show_config(current_request).await?.into_inner();

            // Start with existing config or create a new one
            let current_config = current_response.config.unwrap_or_default();
            let mut map_config = current_config.map_config.unwrap_or_default();
            let mut sync_config = current_config.sync_config.unwrap_or_default();

            // Update map config fields if provided
            if let Some(index_size) = cmd.index_size {
                map_config.index_size = index_size;
            }

            if let Some(extra_bucket_count) = cmd.extra_bucket_count {
                map_config.extra_bucket_count = extra_bucket_count;
            }

            // Update only the fields that were provided
            if let Some(ref src_addr) = cmd.src_addr {
                sync_config.src_addr = format::parse_ipv6(src_addr)?;
            }

            if let Some(ref dst_ether) = cmd.dst_ether {
                sync_config.dst_ether = format::parse_mac(dst_ether)?;
            }

            if let Some(ref dst_addr_multicast) = cmd.dst_addr_multicast {
                sync_config.dst_addr_multicast = format::parse_ipv6(dst_addr_multicast)?;
            }

            if let Some(port_multicast) = cmd.port_multicast {
                sync_config.port_multicast = port_multicast;
            }

            if let Some(ref dst_addr_unicast) = cmd.dst_addr_unicast {
                sync_config.dst_addr_unicast = format::parse_ipv6(dst_addr_unicast)?;
            }

            if let Some(port_unicast) = cmd.port_unicast {
                sync_config.port_unicast = port_unicast;
            }

            // Convert timeouts from Duration to nanoseconds if provided
            if let Some(tcp_syn_ack) = cmd.tcp_syn_ack {
                sync_config.tcp_syn_ack = tcp_syn_ack.as_nanos() as u64;
            }

            if let Some(tcp_syn) = cmd.tcp_syn {
                sync_config.tcp_syn = tcp_syn.as_nanos() as u64;
            }

            if let Some(tcp_fin) = cmd.tcp_fin {
                sync_config.tcp_fin = tcp_fin.as_nanos() as u64;
            }

            if let Some(tcp) = cmd.tcp {
                sync_config.tcp = tcp.as_nanos() as u64;
            }

            if let Some(udp) = cmd.udp {
                sync_config.udp = udp.as_nanos() as u64;
            }

            if let Some(default) = cmd.default {
                sync_config.default = default.as_nanos() as u64;
            }

            let request = SetConfigRequest {
                target: Some(TargetModule {
                    config_name: cmd.config_name.clone(),
                    dataplane_instance: inst,
                }),
                map_config: Some(map_config),
                sync_config: Some(sync_config),
            };
            log::trace!("SetConfigRequest: {request:?}");
            let response = self.client.set_config(request).await?.into_inner();
            log::debug!("SetConfigResponse: {response:?}");
            println!("Successfully configured fwstate for instance {}", inst);
        }
        Ok(())
    }
}

pub fn print_json(resp: Vec<ShowConfigResponse>) -> Result<(), Box<dyn Error>> {
    println!("{}", serde_json::to_string(&resp)?);
    Ok(())
}

pub fn print_tree(configs: Vec<ShowConfigResponse>) -> Result<(), Box<dyn Error>> {
    let mut tree = TreeBuilder::new("FwState Configs".to_string());

    for resp in &configs {
        tree.begin_child(format!("Instance {}", resp.instance));

        if let Some(config) = &resp.config {
            // Display map configuration
            if let Some(map_config) = &config.map_config {
                tree.begin_child("Map Configuration".to_string());
                tree.add_empty_child(format!("Index Size: {}", map_config.index_size));
                tree.add_empty_child(format!("Extra Bucket Count: {}", map_config.extra_bucket_count));
                tree.end_child();
            }

            // Display fwstate map offsets
            tree.begin_child("FwState Map Offsets".to_string());
            if config.fw4state_offset != 0 {
                tree.add_empty_child(format!("IPv4: 0x{:x}", config.fw4state_offset));
                tree.add_empty_child(format!("  Size: {} entries", config.fw4state_size));
            } else {
                tree.add_empty_child("IPv4: null".to_string());
            }
            if config.fw6state_offset != 0 {
                tree.add_empty_child(format!("IPv6: 0x{:x}", config.fw6state_offset));
                tree.add_empty_child(format!("  Size: {} entries", config.fw6state_size));
            } else {
                tree.add_empty_child("IPv6: null".to_string());
            }
            tree.end_child();

            if let Some(sync_config) = &config.sync_config {
                tree.begin_child("Sync Configuration".to_string());

                tree.add_empty_child(format!(
                    "Source Address: {}",
                    format::format_ipv6(&sync_config.src_addr)
                ));
                tree.add_empty_child(format!(
                    "Destination MAC: {}",
                    format::format_mac(&sync_config.dst_ether)
                ));
                tree.add_empty_child(format!(
                    "Multicast Address: {}",
                    format::format_ipv6(&sync_config.dst_addr_multicast)
                ));
                tree.add_empty_child(format!("Multicast Port: {}", sync_config.port_multicast));
                tree.add_empty_child(format!(
                    "Unicast Address: {}",
                    format::format_ipv6(&sync_config.dst_addr_unicast)
                ));
                tree.add_empty_child(format!("Unicast Port: {}", sync_config.port_unicast));

                tree.begin_child("Timeouts".to_string());
                tree.add_empty_child(format!(
                    "TCP SYN-ACK: {:?}",
                    std::time::Duration::from_nanos(sync_config.tcp_syn_ack)
                ));
                tree.add_empty_child(format!(
                    "TCP SYN: {:?}",
                    std::time::Duration::from_nanos(sync_config.tcp_syn)
                ));
                tree.add_empty_child(format!(
                    "TCP FIN: {:?}",
                    std::time::Duration::from_nanos(sync_config.tcp_fin)
                ));
                tree.add_empty_child(format!("TCP: {:?}", std::time::Duration::from_nanos(sync_config.tcp)));
                tree.add_empty_child(format!("UDP: {:?}", std::time::Duration::from_nanos(sync_config.udp)));
                tree.add_empty_child(format!(
                    "Default: {:?}",
                    std::time::Duration::from_nanos(sync_config.default)
                ));
                tree.end_child();

                tree.end_child();
            }
        }

        tree.end_child();
    }

    let tree = tree.build();
    ptree::print_tree(&tree)?;

    Ok(())
}
