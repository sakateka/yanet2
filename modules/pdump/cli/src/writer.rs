use core::{error::Error, time::Duration};
use std::{
    fs,
    io::{self, Write},
};

use pcap_file::{
    DataLink, Endianness,
    pcap::{PcapHeader, PcapPacket, PcapWriter},
    pcapng::{
        PcapNgWriter,
        blocks::{
            Block,
            enhanced_packet::EnhancedPacketBlock,
            interface_description::{InterfaceDescriptionBlock, InterfaceDescriptionOption, TsResolution},
        },
    },
};
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;
use tonic::{Status, codec::Streaming};
use ync::errors::root_cause;

use crate::{args::DumpOutputFormat, pdumppb, printer};

enum PdumpOutput {
    Stdout(io::Stdout),
    File(fs::File),
}

impl PdumpOutput {
    fn new(dst: &str) -> io::Result<PdumpOutput> {
        match dst {
            "-" | "/dev/stdout" => Ok(PdumpOutput::Stdout(io::stdout())),
            _ => Ok(PdumpOutput::File(fs::File::create(dst)?)),
        }
    }
}

impl io::Write for PdumpOutput {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            PdumpOutput::Stdout(stdout) => stdout.write(buf),
            PdumpOutput::File(file) => file.write(buf),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match self {
            PdumpOutput::Stdout(stdout) => stdout.flush(),
            PdumpOutput::File(file) => file.flush(),
        }
    }
}

pub struct Text {
    inner: PdumpOutput,
    pretty: bool,
    base_ts: Option<u64>,
}

pub struct Pcap {
    inner: PcapWriter<PdumpOutput>,
}

pub struct PcapNg {
    inner: PcapNgWriter<PdumpOutput>,
    interface_id: u32,
}

pub enum PdumpWriter {
    Text(Text),
    Pcap(Pcap),
    PcapNg(PcapNg),
}

/// Header snaplen advertised to PCAP and PCAPNG readers, independent of the
/// configured capture snaplen.
///
/// The producer bounds the captured length of every record to a `u16`, so a
/// record can never exceed this value. The PCAP writer still enforces it
/// (`PcapWriter::write_packet` rejects an oversized record with
/// `PacketTooLarge` instead of truncating it); the PCAPNG writer has no such
/// check and relies entirely on the producer's bound.
const HEADER_SNAPLEN: u32 = u16::MAX as u32;

impl PdumpWriter {
    pub fn new(fmt: DumpOutputFormat, dst: &str) -> Result<Self, Box<dyn Error>> {
        let output = PdumpOutput::new(dst)?;

        let writer = match fmt {
            DumpOutputFormat::Text => PdumpWriter::Text(Text {
                inner: output,
                pretty: false,
                base_ts: None,
            }),
            DumpOutputFormat::Pretty => PdumpWriter::Text(Text {
                inner: output,
                pretty: true,
                base_ts: None,
            }),
            DumpOutputFormat::Pcap => {
                let header = PcapHeader {
                    snaplen: HEADER_SNAPLEN,
                    ts_resolution: pcap_file::TsResolution::NanoSecond,
                    endianness: Endianness::Little,
                    ..Default::default()
                };
                let pcap_writer = PcapWriter::with_header(output, header)?;
                PdumpWriter::Pcap(Pcap { inner: pcap_writer })
            }
            DumpOutputFormat::PcapNg => {
                let mut pcapng_writer = PcapNgWriter::with_endianness(output, Endianness::Little)?;

                // Create and write an Interface Description Block
                let interface_block = InterfaceDescriptionBlock {
                    linktype: DataLink::ETHERNET,
                    snaplen: HEADER_SNAPLEN,
                    options: vec![InterfaceDescriptionOption::IfTsResol(TsResolution::NANO.to_raw())],
                };

                // Write the interface description block
                pcapng_writer.write_block(&Block::InterfaceDescription(interface_block))?;

                // Interface ID is 0 for the first (and only) interface
                let interface_id = 0u32;

                PdumpWriter::PcapNg(PcapNg { inner: pcapng_writer, interface_id })
            }
        };
        Ok(writer)
    }

    fn write(&mut self, rec: pdumppb::Record) -> Result<usize, Box<dyn Error>> {
        match self {
            PdumpWriter::Text(writer) => PdumpWriter::write_text(writer, rec),
            PdumpWriter::Pcap(writer) => PdumpWriter::write_pcap(writer, rec),
            PdumpWriter::PcapNg(writer) => PdumpWriter::write_pcapng(writer, rec),
        }
    }

    fn flush(&mut self) -> Result<(), Box<dyn Error>> {
        match self {
            PdumpWriter::Text(writer) => Ok(writer.inner.flush()?),
            PdumpWriter::Pcap(writer) => Ok(writer.inner.flush()?),
            PdumpWriter::PcapNg(writer) => Ok(writer.inner.get_mut().flush()?),
        }
    }

    fn write_text(writer: &mut Text, rec: pdumppb::Record) -> Result<usize, Box<dyn Error>> {
        let mut meta = rec
            .meta
            .ok_or_else(|| -> Box<dyn Error> { "pdump record missing metadata".into() })?;

        let ts = match &writer.base_ts {
            None => {
                // Store the timestamp of the first packet in the writer to establish a
                // baseline.
                writer.base_ts = Some(meta.timestamp);
                0
            }
            // Align timestamps relative to the first packet. Records arrive in
            // per-worker arrival order rather than timestamp order, so a record
            // can predate the baseline -- clamp instead of wrapping.
            Some(v) => meta.timestamp.saturating_sub(*v),
        };
        meta.timestamp = ts;

        if writer.pretty {
            printer::pretty_print_metadata(&mut writer.inner, &meta)?;
            printer::pretty_print_ethernet_frame(&mut writer.inner, &rec.data, meta.packet_len)?;
        } else {
            printer::pretty_print_metadata_concise(&mut writer.inner, &meta)?;
            printer::pretty_print_ethernet_frame_concise(&mut writer.inner, &rec.data, meta.packet_len)?;
        }
        Ok(0)
    }

    fn write_pcap(writer: &mut Pcap, rec: pdumppb::Record) -> Result<usize, Box<dyn Error>> {
        let meta = rec
            .meta
            .ok_or_else(|| -> Box<dyn Error> { "pdump record missing metadata".into() })?;
        let ts = Duration::from_nanos(meta.timestamp);
        let packet = PcapPacket::new(ts, meta.packet_len, rec.data)?;
        Ok(writer.inner.write_packet(&packet)?)
    }

    fn write_pcapng(writer: &mut PcapNg, rec: pdumppb::Record) -> Result<usize, Box<dyn Error>> {
        let meta = rec
            .meta
            .ok_or_else(|| -> Box<dyn Error> { "pdump record missing metadata".into() })?;
        let ts = Duration::from_nanos(meta.timestamp);

        let packet_block = EnhancedPacketBlock {
            interface_id: writer.interface_id,
            timestamp: ts,
            original_len: meta.packet_len,
            data: rec.data.into(),
            options: vec![],
        };

        Ok(writer.inner.write_block(&Block::EnhancedPacket(packet_block))?)
    }
}

/// Failure of the capture's writing side, carried out of the blocking task.
pub type WriteError = Box<dyn Error + Send + Sync>;

/// Writes captured records until a limit, the stream or a failure ends the
/// capture, always flushing. An output that goes away is not a failure.
pub fn pdump_write(
    mut writer: PdumpWriter,
    mut rx: mpsc::Receiver<pdumppb::Record>,
    packet_limit: Option<u64>,
) -> Result<(), WriteError> {
    let mut count = 0;
    let captured = loop {
        if let Some(limit) = packet_limit
            && count >= limit
        {
            log::debug!("stopping writer because the packet capture limit has been reached: {limit}");

            break Ok(());
        }

        let Some(rec) = rx.blocking_recv() else {
            break Ok(());
        };

        if let Err(err) = writer.write(rec) {
            if is_broken_pipe(err.as_ref()) {
                log::debug!("the output is closed, stopping the capture");

                break Ok(());
            }

            break Err(WriteError::from(format!(
                "failed to write record: {}",
                root_cause(err.as_ref())
            )));
        };

        count += 1;
    };

    let flushed = match writer.flush() {
        Ok(()) => Ok(()),
        Err(err) if is_broken_pipe(err.as_ref()) => Ok(()),
        Err(err) => Err(WriteError::from(format!(
            "failed to flush the output: {}",
            root_cause(err.as_ref())
        ))),
    };

    captured.and(flushed)
}

/// Reports whether the output went away, which ends a capture piped into a
/// consumer that stopped reading.
///
/// The pcap writers render every failure as one fixed text, so the cause
/// chain decides.
fn is_broken_pipe(err: &(dyn Error + 'static)) -> bool {
    root_cause(err)
        .downcast_ref::<io::Error>()
        .is_some_and(|err| err.kind() == io::ErrorKind::BrokenPipe)
}

/// Forwards records to the writer until the stream ends or the capture is
/// cancelled. A closed channel is a finished writer, not a failure.
pub async fn pdump_stream_reader(
    mut stream: Streaming<pdumppb::Record>,
    tx: mpsc::Sender<pdumppb::Record>,
    done: CancellationToken,
) -> Result<(), Status> {
    loop {
        tokio::select! {
            biased;
            _ = done.cancelled() => {
                return Ok(());
            }
            message = stream.message() => {
                match message {
                    Err(status) => return Err(status),
                    Ok(None) => return Ok(()),
                    Ok(Some(rec)) => {
                        if let Err(err) = tx.send(rec).await {
                            log::debug!("pdump writer is gone, stopping the reader: {err}");
                            return Ok(());
                        };
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod test {
    use core::sync::atomic::{AtomicU64, Ordering};
    use std::path::PathBuf;

    use pcap_file::{pcap::PcapReader, pcapng::PcapNgReader};

    use super::*;

    /// A path under the system temp directory unique to the calling test,
    /// removed on drop.
    struct TempPath(PathBuf);

    impl TempPath {
        fn new(label: &str) -> Self {
            static COUNTER: AtomicU64 = AtomicU64::new(0);
            let unique = COUNTER.fetch_add(1, Ordering::Relaxed);
            let path =
                std::env::temp_dir().join(format!("pdump-writer-test-{label}-{}-{unique}.bin", std::process::id()));
            Self(path)
        }

        fn as_str(&self) -> &str {
            self.0.to_str().expect("temp path must be valid UTF-8")
        }
    }

    impl Drop for TempPath {
        fn drop(&mut self) {
            let _ = fs::remove_file(&self.0);
        }
    }

    /// Builds a record whose captured payload is `len` bytes, standing in
    /// for a capture taken under a smaller or larger configured snaplen.
    fn record_of_len(len: usize) -> pdumppb::Record {
        pdumppb::Record {
            meta: Some(pdumppb::RecordMeta {
                packet_len: len as u32,
                ..Default::default()
            }),
            data: vec![0xAB; len],
        }
    }

    #[test]
    fn test_pcap_header_snaplen_is_fixed_regardless_of_config_snaplen() {
        let path = TempPath::new("pcap-header-snaplen");
        let mut writer = PdumpWriter::new(DumpOutputFormat::Pcap, path.as_str()).expect("must open");
        writer.flush().expect("must flush");

        let file = fs::File::open(&path.0).expect("must reopen the written file");
        let reader = PcapReader::new(file).expect("must parse the pcap header");

        assert_eq!(reader.header().snaplen, HEADER_SNAPLEN);
    }

    #[test]
    fn test_pcapng_header_snaplen_is_fixed_regardless_of_config_snaplen() {
        let path = TempPath::new("pcapng-header-snaplen");
        let mut writer = PdumpWriter::new(DumpOutputFormat::PcapNg, path.as_str()).expect("must open");
        writer.flush().expect("must flush");

        let file = fs::File::open(&path.0).expect("must reopen the written file");
        let mut reader = PcapNgReader::new(file).expect("must parse the pcapng header");
        reader
            .next_block()
            .expect("missing the interface description block")
            .expect("must parse the interface description block");

        assert_eq!(reader.interfaces()[0].snaplen, HEADER_SNAPLEN);
    }

    #[test]
    fn test_pcap_writes_records_captured_under_different_snaplens_intact() {
        let path = TempPath::new("pcap-records-intact");
        let before = record_of_len(60);
        let after = record_of_len(1500);
        let (before_data, after_data) = (before.data.clone(), after.data.clone());

        let mut writer = PdumpWriter::new(DumpOutputFormat::Pcap, path.as_str()).expect("must open");
        writer.write(before).expect("must write the smaller-snaplen record");
        writer.write(after).expect("must write the larger-snaplen record");
        writer.flush().expect("must flush");

        let file = fs::File::open(&path.0).expect("must reopen the written file");
        let mut reader = PcapReader::new(file).expect("must parse the pcap header");

        let first = reader.next_packet().expect("missing first record").expect("must parse");
        assert_eq!(first.data(), before_data.as_slice());
        let second = reader
            .next_packet()
            .expect("missing second record")
            .expect("must parse");
        assert_eq!(second.data(), after_data.as_slice());
        assert!(reader.next_packet().is_none());
    }

    #[test]
    fn test_pcapng_writes_records_captured_under_different_snaplens_intact() {
        let path = TempPath::new("pcapng-records-intact");
        let before = record_of_len(60);
        let after = record_of_len(1500);
        let (before_data, after_data) = (before.data.clone(), after.data.clone());

        let mut writer = PdumpWriter::new(DumpOutputFormat::PcapNg, path.as_str()).expect("must open");
        writer.write(before).expect("must write the smaller-snaplen record");
        writer.write(after).expect("must write the larger-snaplen record");
        writer.flush().expect("must flush");

        let file = fs::File::open(&path.0).expect("must reopen the written file");
        let mut reader = PcapNgReader::new(file).expect("must parse the pcapng header");

        let mut packets = Vec::new();
        while let Some(block) = reader.next_block() {
            let Block::EnhancedPacket(packet) = block.expect("must parse block") else {
                continue;
            };
            packets.push(packet.data.into_owned());
        }

        assert_eq!(packets, vec![before_data, after_data]);
    }

    #[test]
    fn test_pdump_write_reports_an_output_that_cannot_take_records() {
        let writer = PdumpWriter::new(DumpOutputFormat::Text, "/dev/full").expect("must open");
        let (tx, rx) = mpsc::channel(1);
        tx.try_send(pdumppb::Record {
            meta: Some(pdumppb::RecordMeta::default()),
            data: Vec::new(),
        })
        .expect("must accept the record");
        drop(tx);

        let err = pdump_write(writer, rx, None).expect_err("a full output must fail the capture");

        assert!(err.to_string().starts_with("failed to write record: "), "{err}");
    }

    #[test]
    fn test_broken_pipe_is_recognised_through_a_wrapper() {
        let piped = pcap_file::PcapError::IoError(io::Error::from(io::ErrorKind::BrokenPipe));
        let full = pcap_file::PcapError::IoError(io::Error::from(io::ErrorKind::StorageFull));

        assert!(is_broken_pipe(&piped));
        assert!(!is_broken_pipe(&full));
    }

    /// Verifies that a record the writer cannot encode ends the capture with
    /// an error instead of a log line.
    #[test]
    fn test_pdump_write_reports_a_failed_record() {
        let writer = PdumpWriter::new(DumpOutputFormat::Pcap, "/dev/null").expect("must open");
        let (tx, rx) = mpsc::channel(1);
        tx.try_send(pdumppb::Record { meta: None, data: Vec::new() })
            .expect("must accept the record");
        drop(tx);

        let err = pdump_write(writer, rx, None).expect_err("a record without metadata must fail the capture");

        assert_eq!("failed to write record: pdump record missing metadata", err.to_string());
    }
}
