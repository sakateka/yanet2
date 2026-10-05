{
  description = "Self-contained shell for modules/pdump/tests/ab/pdump-ab.sh";

  # Pinned by flake.lock: the benchmark must build with the same toolchain
  # on every box. nixos-25.11 carries meson 1.9.1, gcc 13, go 1.25,
  # protoc-gen-go 1.36 and protoc-gen-go-grpc 1.5.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

  outputs = { nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
    in {
      devShells = nixpkgs.lib.genAttrs systems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          # DPDK's build runs python scripts that import elftools.
          # meson hands them its own interpreter, so PYTHONPATH below
          # exposes pyelftools to that one too.
          python = pkgs.python3.withPackages (ps: [ ps.pyelftools ]);
          caBundle = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
        in {
          # The stdenv brings gcc, binutils (objdump), coreutils, findutils,
          # gawk, gnused, gnugrep and make. gcc 13 is what CI (Ubuntu 24.04)
          # builds with: the tree builds with -Werror, and gcc 14 reports
          # warnings there that gcc 13 does not.
          default = pkgs.mkShell.override { stdenv = pkgs.gcc13Stdenv; } {
            packages = with pkgs; [
              bison
              cacert
              # Builds the rure regex archive lib/counters links; unused by
              # this benchmark but part of a plain `meson setup`.
              cargo
              # cmake and flex/bison build the vendored libpcap submodule,
              # which modules/pdump/api links statically.
              cmake
              curl
              flex
              git
              gnumake
              go
              hostname
              libyaml
              meson
              ninja
              numactl
              pkg-config
              protobuf
              protoc-gen-go
              protoc-gen-go-grpc
              python
              # libibverbs and libmlx5 for the DPDK mlx5 drivers the build
              # enables explicitly.
              rdma-core
              rustc
              # taskset, for pdump-ab.sh's CPU pinning.
              util-linux
              which
            ];

            PYTHONPATH = "${python}/${python.sitePackages}";

            # Build like a plain distribution gcc: the cc-wrapper hardening
            # flags change the generated code the benchmark measures, and
            # the wrapper must not strip the -mcpu/-march=native DPDK asks
            # for.
            hardeningDisable = [ "all" ];
            NIX_ENFORCE_NO_NATIVE = "0";

            # nix develop drops SSL_CERT_FILE from the shell environment, so
            # point git and Go at the pinned CA bundle here.
            shellHook = ''
              export SSL_CERT_FILE=${caBundle}
              export GIT_SSL_CAINFO=${caBundle}
            '';
          };
        });
    };
}
