use core::error::Error;

use std::net::IpAddr;

use clap::{ArgAction, CommandFactory, Parser};
use clap_complete::CompleteEnv;
use commonpb::TargetModule;
use tonic::transport::Channel;
use ync::logging;

use serde::{Deserialize, Serialize};

use ptree::TreeBuilder;

use aclpb::{
    DeleteConfigRequest, ListConfigsRequest, ShowConfigRequest, ShowConfigResponse,
    UpdateConfigRequest, UpdateFwStateConfigRequest, acl_service_client::AclServiceClient,
};

use args::{DeleteCmd, ModeCmd, OutputFormat, SetFwstateConfigCmd, ShowConfigCmd, UpdateCmd};

mod args;
mod format;

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

/// ACL module.
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

#[derive(Debug, Serialize, Deserialize)]
struct VlanRange {
    from: u32,
    to: u32,
}

impl From<VlanRange> for aclpb::VlanRange {
    fn from(r: VlanRange) -> Self {
        Self { from: r.from, to: r.to }
    }
}

#[derive(Debug, Serialize, Deserialize)]
struct Net {
    addr: String,
    prefix: u32,
}

impl From<Net> for aclpb::IpNet {
    fn from(value: Net) -> Self {
        let net: IpAddr = value.addr.parse().unwrap();
        Self {
            ip: match net {
                IpAddr::V4(ipv4) => ipv4.octets().into(),
                IpAddr::V6(ipv6) => ipv6.octets().into(),
            },
            prefix_len: value.prefix,
        }
    }
}

#[derive(Debug, Serialize, Deserialize)]
struct Range {
    from: u32,
    to: u32,
}

impl From<Range> for aclpb::PortRange {
    fn from(r: Range) -> Self {
        Self { from: r.from, to: r.to }
    }
}

impl From<Range> for aclpb::ProtoRange {
    fn from(r: Range) -> Self {
        Self { from: r.from, to: r.to }
    }
}

impl From<Range> for aclpb::VlanRange {
    fn from(r: Range) -> Self {
        Self { from: r.from, to: r.to }
    }
}

#[derive(Debug, Serialize, Deserialize)]
enum ActionKind {
    Allow,
    Deny,
}

#[derive(Debug, Serialize, Deserialize)]
struct ACLRule {
    srcs: Vec<Net>,
    dsts: Vec<Net>,
    src_ports: Vec<Range>,
    dst_ports: Vec<Range>,
    proto_ranges: Vec<Range>,
    vlan_ranges: Vec<Range>,
    devices: Vec<String>,
    counter: String,
    action: ActionKind,
}

impl From<ACLRule> for aclpb::Rule {
    fn from(acl_rule: ACLRule) -> Self {
        Self {
            counter: acl_rule.counter,
            devices: acl_rule.devices,
            vlan_ranges: acl_rule.vlan_ranges.into_iter().map(|m| m.into()).collect(),
            srcs: acl_rule.srcs.into_iter().map(|m| m.into()).collect(),
            dsts: acl_rule.dsts.into_iter().map(|m| m.into()).collect(),
            src_port_ranges: acl_rule.src_ports.into_iter().map(|m| m.into()).collect(),
            dst_port_ranges: acl_rule.dst_ports.into_iter().map(|m| m.into()).collect(),
            proto_ranges: acl_rule.proto_ranges.into_iter().map(|m| m.into()).collect(),
            keep_state: false,
            action: match acl_rule.action {
                ActionKind::Allow => aclpb::ActionKind::Pass,
                ActionKind::Deny => aclpb::ActionKind::Deny,
            }
            .into(),
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

#[derive(Debug, Serialize, Deserialize)]
pub struct ACLConfig {
    rules: Vec<ACLRule>,
}

impl From<ACLConfig> for Vec<aclpb::Rule> {
    fn from(config: ACLConfig) -> Self {
        config.rules.into_iter().map(From::from).collect()
    }
}

////////////////////////////////////////////////////////////////////////////////

impl ACLConfig {
    pub fn from_file(path: &str) -> Result<Self, Box<dyn Error>> {
        let file = std::fs::File::open(path)?;
        let config = serde_yaml::from_reader(file)?;
        Ok(config)
    }
}

pub struct ACLService {
    client: AclServiceClient<Channel>,
}

impl ACLService {
    pub async fn new(endpoint: String) -> Result<Self, Box<dyn Error>> {
        let client = AclServiceClient::connect(endpoint).await?;
        Ok(Self { client })
    }

    async fn get_dataplane_instances(&mut self) -> Result<Vec<u32>, Box<dyn Error>> {
        let request = ListConfigsRequest {};
        let response = self.client.list_configs(request).await?.into_inner();
        Ok(response.instance_configs.iter().map(|c| c.instance).collect())
    }

    pub async fn delete_config(&mut self, cmd: DeleteCmd) -> Result<(), Box<dyn Error>> {
        let request = DeleteConfigRequest {
            target: Some(TargetModule {
                config_name: cmd.config_name.clone(),
                dataplane_instance: cmd.instance,
            }),
        };
        self.client.delete_config(request).await?;

        Ok(())
    }

    pub async fn update_config(&mut self, cmd: UpdateCmd) -> Result<(), Box<dyn Error>> {
        let config = ACLConfig::from_file(&cmd.rules)?;
        let rules: Vec<aclpb::Rule> = config.into();
        let request = UpdateConfigRequest {
            target: Some(TargetModule {
                config_name: cmd.config_name.clone(),
                dataplane_instance: cmd.instance,
            }),
            rules,
        };
        log::trace!("UpdateConfigRequest: {request:?}");
        let response = self.client.update_config(request).await?.into_inner();
        log::debug!("UpdateConfigResponse: {response:?}");

        Ok(())
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

    pub async fn set_fwstate_config(&mut self, cmd: SetFwstateConfigCmd) -> Result<(), Box<dyn Error>> {
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
            let mut map_config = current_response.fwstate_map.unwrap_or_default();
            let mut sync_config = current_response.fwstate_sync.unwrap_or_default();

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

            let request = UpdateFwStateConfigRequest {
                target: Some(TargetModule {
                    config_name: cmd.config_name.clone(),
                    dataplane_instance: inst,
                }),
                map_config: Some(map_config),
                sync_config: Some(sync_config),
            };
            log::trace!("UpdateFWStateConfigRequest: {request:?}");
            let response = self.client.update_fw_state_config(request).await?.into_inner();
            log::debug!("UpdateFWStateConfigResponse: {response:?}");
            println!("Successfully configured fwstate for instance {}", inst);
        }
        Ok(())
    }
}

async fn run(cmd: Cmd) -> Result<(), Box<dyn Error>> {
    let mut service = ACLService::new(cmd.endpoint).await?;

    match cmd.mode {
        ModeCmd::Delete(cmd) => service.delete_config(cmd).await,
        ModeCmd::Update(cmd) => service.update_config(cmd).await,
        ModeCmd::Show(cmd) => service.show_config(cmd).await,
        ModeCmd::SetFwstateConfig(cmd) => service.set_fwstate_config(cmd).await,
    }
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

pub fn print_json(resp: Vec<ShowConfigResponse>) -> Result<(), Box<dyn Error>> {
    println!("{}", serde_json::to_string(&resp)?);
    Ok(())
}

pub fn print_tree(configs: Vec<ShowConfigResponse>) -> Result<(), Box<dyn Error>> {
    let mut tree = TreeBuilder::new("ACL Configs".to_string());

    for resp in &configs {
        tree.begin_child(format!("Instance {}", resp.instance));

        // Display rules if present
        if let Some(rules_config) = &resp.rules {
            if !rules_config.rules.is_empty() {
                tree.begin_child("Rules".to_string());
                for rule in &rules_config.rules {
                    tree.add_empty_child(format!(
                        "Rule src={:?} dst={:?}: action={}",
                        rule.srcs, rule.dsts, rule.action
                    ));
                }
                tree.end_child();
            }
        }

        // Display fwstate map configuration
        if let Some(map_config) = &resp.fwstate_map {
            tree.begin_child("FwState Map Configuration".to_string());
            tree.add_empty_child(format!("Index Size: {}", map_config.index_size));
            tree.add_empty_child(format!("Extra Bucket Count: {}", map_config.extra_bucket_count));
            tree.end_child();
        }

        // Display fwstate sync configuration
        if let Some(sync_config) = &resp.fwstate_sync {
            tree.begin_child("FwState Sync Configuration".to_string());

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

        tree.end_child();
    }

    let tree = tree.build();
    ptree::print_tree(&tree)?;

    Ok(())
}
