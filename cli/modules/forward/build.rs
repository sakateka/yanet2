use core::error::Error;

pub fn main() -> Result<(), Box<dyn Error>> {
    tonic_build::configure()
        .message_attribute(".", "#[derive(Serialize)]")
        .compile_protos(&["forward/forwardpb/forward.proto"], &["../../../controlplane/modules"])?;

    Ok(())
}
