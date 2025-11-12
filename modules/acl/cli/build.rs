use core::error::Error;

pub fn main() -> Result<(), Box<dyn Error>> {
    println!("cargo:rerun-if-changed=../controlplane/aclpb/acl.proto");
    println!("cargo:rerun-if-changed=../../../common/proto/target.proto");
    println!("cargo:rerun-if-changed=../../fwstate/controlplane/fwstatepb/fwstate.proto");

    tonic_build::configure()
        .emit_rerun_if_changed(false)
        .build_server(false)
        .message_attribute(".", "#[derive(Serialize)]")
        .compile_protos(
            &[
                "common/proto/target.proto",
                "aclpb/acl.proto",
                "fwstatepb/fwstate.proto",
            ],
            &["../../..", "../controlplane", "../../fwstate/controlplane"],
        )?;

    Ok(())
}
