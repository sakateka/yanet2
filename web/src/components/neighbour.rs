use core::{
    fmt::{self, Display, Formatter},
    net::{AddrParseError, IpAddr},
    time::Duration,
};

use leptos::prelude::*;
use leptos_router::hooks::query_signal;
use leptos_struct_table::*;
use wasmtimer::std::{SystemTime, UNIX_EPOCH};

use crate::{
    api::neighbour::{code, NeighbourClient},
    components::common::{
        center::Center,
        error::ErrorView,
        header::Header,
        input::Input,
        spinner::{Spinner, SpinnerSize},
        viewport::{Viewport, ViewportContent},
    },
    xxx::{accesskey::accesskey, fuzz::fuzz_match},
};

#[component]
pub fn NeighbourView() -> impl IntoView {
    // Use query parameters as a state.
    let (search, set_search) = query_signal::<String>("s");

    let neighbours = LocalResource::new(move || async move {
        let service = NeighbourClient::new();

        // TODO: docs.
        service.list_neighbours().await.map_err(Error::from)
    });

    let neighbours = move || -> Option<_> {
        let neighbours = neighbours.get()?.take().map(|n| {
            n.into_iter()
                .map(|n| NeighbourEntry::try_from(n).map_err(Error::from))
                .collect::<Result<Vec<_>, _>>()
        });

        // TODO: docs.
        let neighbours = match neighbours {
            Ok(neighbours) => match neighbours {
                Ok(neighbours) => Ok(neighbours),
                Err(err) => Err(err),
            },
            Err(err) => Err(err),
        };

        Some(neighbours)
    };

    // TODO: docs.
    let neighbours = move || -> Option<Result<Vec<NeighbourEntry>>> {
        let search = search.get().unwrap_or_default();
        let neighbours = neighbours();

        if search.is_empty() {
            return neighbours;
        }

        // TODO: docs.
        neighbours.map(|neighbours| {
            neighbours.map(|neighbours| {
                neighbours
                    .into_iter()
                    .filter(|neigh| fuzz_match(&search, &neigh.next_hop.to_string()))
                    .collect()
            })
        })
    };

    let search_hotkey = accesskey('s').unwrap_or("s".to_string());
    let search_placeholder = format!("Type '{search_hotkey}' to filter (fuzzy mode)");

    view! {
        <Viewport>
            <Header class="neighbour-ph">
                <h1>"Neighbours"</h1>

                <Input
                    value=Signal::derive(move || search.get().unwrap_or_default())
                    on_input=move |s| set_search.set(if s.is_empty() { None } else { Some(s) })
                    accesskey="s"
                    placeholder=search_placeholder
                />
            </Header>

            <ViewportContent class="neighbour-pc">
                <Suspense fallback=move || {
                    view! {
                        <Center>
                            <Spinner size=SpinnerSize::Large />
                        </Center>
                    }
                }>
                    <ErrorBoundary fallback=move |errors| {
                        // Join all errors into a single string.
                        let error_details = errors.get()
                            .into_iter()
                            .map(|(.., err)| err.to_string())
                            .collect::<Vec<_>>()
                            .join("\n");

                        view! {
                            <ErrorView
                                title="Error while fetching neighbours"
                                details=error_details
                            />
                        }
                    }>
                        {move || {
                            let n = neighbours();

                            n.map(|neighbours| {
                                neighbours.map(|neighbours| {
                                    view! {
                                        <table class="noc-table">
                                            <TableContent
                                                rows=neighbours
                                                scroll_container="html"
                                                sorting_mode=SortingMode::SingleColumn
                                            />
                                        </table>
                                    }
                                })
                            })
                        }}
                    </ErrorBoundary>
                </Suspense>
            </ViewportContent>
        </Viewport>
    }
}

// TODO: docs.
#[derive(Clone, TableRow)]
#[table(impl_vec_data_provider, sortable)]
struct NeighbourEntry {
    next_hop: IpAddr,
    link_addr: MacAddr,
    hardware_addr: MacAddr,
    state: State,
    age: Age,
}

impl TryFrom<code::NeighbourEntry> for NeighbourEntry {
    type Error = AddrParseError;

    fn try_from(entry: code::NeighbourEntry) -> Result<Self, Self::Error> {
        let next_hop = entry.next_hop.parse()?;
        let age = Duration::from_secs(entry.updated_at as u64);

        let link_addr = entry.link_addr.map(|v| v.addr).unwrap_or_default();
        let hardware_addr = entry.hardware_addr.map(|v| v.addr).unwrap_or_default();

        let entry = Self {
            next_hop,
            link_addr: link_addr.into(),
            hardware_addr: hardware_addr.into(),
            state: State(entry.state),
            age: Age(age),
        };

        Ok(entry)
    }
}

// TODO: docs.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct State(pub i32);

impl Display for State {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        let v = match self {
            Self(0x00) => "NONE",
            Self(0x01) => "INCOMPLETE",
            Self(0x02) => "REACHABLE",
            Self(0x04) => "STALE",
            Self(0x08) => "DELAY",
            Self(0x10) => "PROBE",
            Self(0x20) => "FAILED",
            Self(0x40) => "NOARP",
            Self(0x80) => "PERMANENT",
            Self(..) => "UNKNOWN",
        };

        write!(f, "{v}")
    }
}

impl CellValue for State {
    type RenderOptions = ();

    fn render_value(self, _options: Self::RenderOptions) -> impl IntoView {
        view! { <>{self.to_string()}</> }
    }
}

// TODO: docs.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct Age(pub Duration);

impl Display for Age {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        let now = SystemTime::now();
        let duration = match self {
            Self(age) => {
                let timestamp = UNIX_EPOCH + *age;
                now.duration_since(timestamp).unwrap_or_default()
            }
        };

        write!(f, "{:.2?}", duration)
    }
}

impl CellValue for Age {
    type RenderOptions = ();

    fn render_value(self, _options: Self::RenderOptions) -> impl IntoView {
        view! { <>{self.to_string()}</> }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct MacAddr(pub netip::MacAddr);

impl From<u64> for MacAddr {
    fn from(addr: u64) -> Self {
        Self(netip::MacAddr::from(addr))
    }
}

impl Display for MacAddr {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        match self {
            Self(addr) => write!(f, "{addr}"),
        }
    }
}

impl CellValue for MacAddr {
    type RenderOptions = ();

    fn render_value(self, _options: Self::RenderOptions) -> impl IntoView {
        view! { <>{self.to_string()}</> }
    }
}
