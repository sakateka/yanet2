proto_dir = meson.current_source_dir()
root_dir = meson.project_source_root()
proto_files = [
    join_paths(proto_dir, 'route.proto'),
    join_paths(proto_dir, 'neighbour.proto'),
    join_paths(proto_dir, 'macaddr.proto'),
]

protoc_gen = custom_target(
    'route-protoc',
    output: [
        'route.pb.go',
        'route_grpc.pb.go',
        'neighbour.pb.go',
        'neighbour_grpc.pb.go',
        'macaddr.pb.go',
    ],
    input: proto_files,
    command: [
        protoc,
        '-I', proto_dir,
        '-I', root_dir,
        '--go_out=paths=source_relative:' + proto_dir,
        '--go-grpc_out=paths=source_relative:' + proto_dir,
        '@INPUT@',
    ],
    depends: [common_protoc_gen],
    build_by_default: true,
)
route_protoc_gen = protoc_gen
