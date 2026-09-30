use clap::Parser;
use clap_complete::engine::ArgValueCandidates;

#[derive(Debug, Clone, Parser)]
pub enum ModeCmd {
    /// Create a named ring and publish it.
    Create(CreateCmd),
    /// List registered ring objects.
    List,
    /// Show the capacity and worker count of a named ring.
    Show(ShowCmd),
    /// Delete a named ring.
    Delete(DeleteCmd),
}

impl ModeCmd {
    pub(crate) fn action(&self) -> &'static str {
        match self {
            Self::Create(..) => "create",
            Self::List => "list",
            Self::Show(..) => "show",
            Self::Delete(..) => "delete",
        }
    }
}

/// Parses a per-worker capacity such as 1MiB or 4096 into bytes; the
/// service checks that it is a power of two in range.
pub(crate) fn parse_capacity(raw: &str) -> Result<u64, String> {
    raw.parse::<bytesize::ByteSize>()
        .map(|size| size.as_u64())
        .map_err(|_| format!("expected a size such as 1MiB, got {raw:?}"))
}

#[derive(Debug, Clone, Parser)]
pub struct CreateCmd {
    /// Name of the ring to create.
    #[arg(long = "name", short = 'n')]
    pub name: String,

    /// Per-worker buffer size, such as 1MiB (the service checks that it is
    /// a power of two in range).
    #[arg(long, value_parser = parse_capacity)]
    pub capacity: u64,
}

#[derive(Debug, Clone, Parser)]
pub struct ShowCmd {
    /// Name of the ring to show.
    #[arg(long = "name", short = 'n', add = ArgValueCandidates::new(crate::ring_candidates))]
    pub name: String,
}

#[derive(Debug, Clone, Parser)]
pub struct DeleteCmd {
    /// Name of the ring to delete.
    #[arg(long = "name", short = 'n', add = ArgValueCandidates::new(crate::ring_candidates))]
    pub name: String,
}
