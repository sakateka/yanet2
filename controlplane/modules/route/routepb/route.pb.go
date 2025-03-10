// Code generated by protoc-gen-go. DO NOT EDIT.
// versions:
// 	protoc-gen-go v1.36.5
// 	protoc        v3.12.4
// source: route.proto

package routepb

import (
	protoreflect "google.golang.org/protobuf/reflect/protoreflect"
	protoimpl "google.golang.org/protobuf/runtime/protoimpl"
	reflect "reflect"
	sync "sync"
	unsafe "unsafe"
)

const (
	// Verify that this generated code is sufficiently up-to-date.
	_ = protoimpl.EnforceVersion(20 - protoimpl.MinVersion)
	// Verify that runtime/protoimpl is sufficiently up-to-date.
	_ = protoimpl.EnforceVersion(protoimpl.MaxVersion - 20)
)

// InsertRouteRequest is the request to insert a route.
type InsertRouteRequest struct {
	state protoimpl.MessageState `protogen:"open.v1"`
	// ModuleName is the name of the module into which the route should be
	// inserted.
	ModuleName string `protobuf:"bytes,1,opt,name=module_name,json=moduleName,proto3" json:"module_name,omitempty"`
	// The destination prefix of the route.
	//
	// The prefix must be an IPv4 or IPv6 address followed by "/" and the
	// length of the prefix.
	Prefix string `protobuf:"bytes,2,opt,name=prefix,proto3" json:"prefix,omitempty"`
	// The IP address of the nexthop router.
	//
	// The address must be either an IPv4 or IPv6 address.
	//
	// Example: "fe80::1", "192.168.1.1"
	NexthopAddr string `protobuf:"bytes,3,opt,name=nexthop_addr,json=nexthopAddr,proto3" json:"nexthop_addr,omitempty"`
	// Numa specifies NUMA nodes that should be affected.
	//
	// Empty means all NUMA nodes.
	Numa          []uint32 `protobuf:"varint,4,rep,packed,name=numa,proto3" json:"numa,omitempty"`
	unknownFields protoimpl.UnknownFields
	sizeCache     protoimpl.SizeCache
}

func (x *InsertRouteRequest) Reset() {
	*x = InsertRouteRequest{}
	mi := &file_route_proto_msgTypes[0]
	ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
	ms.StoreMessageInfo(mi)
}

func (x *InsertRouteRequest) String() string {
	return protoimpl.X.MessageStringOf(x)
}

func (*InsertRouteRequest) ProtoMessage() {}

func (x *InsertRouteRequest) ProtoReflect() protoreflect.Message {
	mi := &file_route_proto_msgTypes[0]
	if x != nil {
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		if ms.LoadMessageInfo() == nil {
			ms.StoreMessageInfo(mi)
		}
		return ms
	}
	return mi.MessageOf(x)
}

// Deprecated: Use InsertRouteRequest.ProtoReflect.Descriptor instead.
func (*InsertRouteRequest) Descriptor() ([]byte, []int) {
	return file_route_proto_rawDescGZIP(), []int{0}
}

func (x *InsertRouteRequest) GetModuleName() string {
	if x != nil {
		return x.ModuleName
	}
	return ""
}

func (x *InsertRouteRequest) GetPrefix() string {
	if x != nil {
		return x.Prefix
	}
	return ""
}

func (x *InsertRouteRequest) GetNexthopAddr() string {
	if x != nil {
		return x.NexthopAddr
	}
	return ""
}

func (x *InsertRouteRequest) GetNuma() []uint32 {
	if x != nil {
		return x.Numa
	}
	return nil
}

// InsertRouteResponse is the response of "InsertRoute" request.
type InsertRouteResponse struct {
	state         protoimpl.MessageState `protogen:"open.v1"`
	unknownFields protoimpl.UnknownFields
	sizeCache     protoimpl.SizeCache
}

func (x *InsertRouteResponse) Reset() {
	*x = InsertRouteResponse{}
	mi := &file_route_proto_msgTypes[1]
	ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
	ms.StoreMessageInfo(mi)
}

func (x *InsertRouteResponse) String() string {
	return protoimpl.X.MessageStringOf(x)
}

func (*InsertRouteResponse) ProtoMessage() {}

func (x *InsertRouteResponse) ProtoReflect() protoreflect.Message {
	mi := &file_route_proto_msgTypes[1]
	if x != nil {
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		if ms.LoadMessageInfo() == nil {
			ms.StoreMessageInfo(mi)
		}
		return ms
	}
	return mi.MessageOf(x)
}

// Deprecated: Use InsertRouteResponse.ProtoReflect.Descriptor instead.
func (*InsertRouteResponse) Descriptor() ([]byte, []int) {
	return file_route_proto_rawDescGZIP(), []int{1}
}

var File_route_proto protoreflect.FileDescriptor

var file_route_proto_rawDesc = string([]byte{
	0x0a, 0x0b, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x2e, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x12, 0x07, 0x72,
	0x6f, 0x75, 0x74, 0x65, 0x70, 0x62, 0x22, 0x84, 0x01, 0x0a, 0x12, 0x49, 0x6e, 0x73, 0x65, 0x72,
	0x74, 0x52, 0x6f, 0x75, 0x74, 0x65, 0x52, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74, 0x12, 0x1f, 0x0a,
	0x0b, 0x6d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x18, 0x01, 0x20, 0x01,
	0x28, 0x09, 0x52, 0x0a, 0x6d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x4e, 0x61, 0x6d, 0x65, 0x12, 0x16,
	0x0a, 0x06, 0x70, 0x72, 0x65, 0x66, 0x69, 0x78, 0x18, 0x02, 0x20, 0x01, 0x28, 0x09, 0x52, 0x06,
	0x70, 0x72, 0x65, 0x66, 0x69, 0x78, 0x12, 0x21, 0x0a, 0x0c, 0x6e, 0x65, 0x78, 0x74, 0x68, 0x6f,
	0x70, 0x5f, 0x61, 0x64, 0x64, 0x72, 0x18, 0x03, 0x20, 0x01, 0x28, 0x09, 0x52, 0x0b, 0x6e, 0x65,
	0x78, 0x74, 0x68, 0x6f, 0x70, 0x41, 0x64, 0x64, 0x72, 0x12, 0x12, 0x0a, 0x04, 0x6e, 0x75, 0x6d,
	0x61, 0x18, 0x04, 0x20, 0x03, 0x28, 0x0d, 0x52, 0x04, 0x6e, 0x75, 0x6d, 0x61, 0x22, 0x15, 0x0a,
	0x13, 0x49, 0x6e, 0x73, 0x65, 0x72, 0x74, 0x52, 0x6f, 0x75, 0x74, 0x65, 0x52, 0x65, 0x73, 0x70,
	0x6f, 0x6e, 0x73, 0x65, 0x32, 0x53, 0x0a, 0x05, 0x52, 0x6f, 0x75, 0x74, 0x65, 0x12, 0x4a, 0x0a,
	0x0b, 0x49, 0x6e, 0x73, 0x65, 0x72, 0x74, 0x52, 0x6f, 0x75, 0x74, 0x65, 0x12, 0x1b, 0x2e, 0x72,
	0x6f, 0x75, 0x74, 0x65, 0x70, 0x62, 0x2e, 0x49, 0x6e, 0x73, 0x65, 0x72, 0x74, 0x52, 0x6f, 0x75,
	0x74, 0x65, 0x52, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74, 0x1a, 0x1c, 0x2e, 0x72, 0x6f, 0x75, 0x74,
	0x65, 0x70, 0x62, 0x2e, 0x49, 0x6e, 0x73, 0x65, 0x72, 0x74, 0x52, 0x6f, 0x75, 0x74, 0x65, 0x52,
	0x65, 0x73, 0x70, 0x6f, 0x6e, 0x73, 0x65, 0x22, 0x00, 0x42, 0x4d, 0x5a, 0x4b, 0x67, 0x69, 0x74,
	0x68, 0x75, 0x62, 0x2e, 0x63, 0x6f, 0x6d, 0x2f, 0x79, 0x61, 0x6e, 0x65, 0x74, 0x2d, 0x70, 0x6c,
	0x61, 0x74, 0x66, 0x6f, 0x72, 0x6d, 0x2f, 0x79, 0x61, 0x6e, 0x65, 0x74, 0x32, 0x2f, 0x63, 0x6f,
	0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x70, 0x6c, 0x61, 0x6e, 0x65, 0x2f, 0x6d, 0x6f, 0x64, 0x75, 0x6c,
	0x65, 0x73, 0x2f, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x2f, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x70, 0x62,
	0x3b, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x70, 0x62, 0x62, 0x06, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x33,
})

var (
	file_route_proto_rawDescOnce sync.Once
	file_route_proto_rawDescData []byte
)

func file_route_proto_rawDescGZIP() []byte {
	file_route_proto_rawDescOnce.Do(func() {
		file_route_proto_rawDescData = protoimpl.X.CompressGZIP(unsafe.Slice(unsafe.StringData(file_route_proto_rawDesc), len(file_route_proto_rawDesc)))
	})
	return file_route_proto_rawDescData
}

var file_route_proto_msgTypes = make([]protoimpl.MessageInfo, 2)
var file_route_proto_goTypes = []any{
	(*InsertRouteRequest)(nil),  // 0: routepb.InsertRouteRequest
	(*InsertRouteResponse)(nil), // 1: routepb.InsertRouteResponse
}
var file_route_proto_depIdxs = []int32{
	0, // 0: routepb.Route.InsertRoute:input_type -> routepb.InsertRouteRequest
	1, // 1: routepb.Route.InsertRoute:output_type -> routepb.InsertRouteResponse
	1, // [1:2] is the sub-list for method output_type
	0, // [0:1] is the sub-list for method input_type
	0, // [0:0] is the sub-list for extension type_name
	0, // [0:0] is the sub-list for extension extendee
	0, // [0:0] is the sub-list for field type_name
}

func init() { file_route_proto_init() }
func file_route_proto_init() {
	if File_route_proto != nil {
		return
	}
	type x struct{}
	out := protoimpl.TypeBuilder{
		File: protoimpl.DescBuilder{
			GoPackagePath: reflect.TypeOf(x{}).PkgPath(),
			RawDescriptor: unsafe.Slice(unsafe.StringData(file_route_proto_rawDesc), len(file_route_proto_rawDesc)),
			NumEnums:      0,
			NumMessages:   2,
			NumExtensions: 0,
			NumServices:   1,
		},
		GoTypes:           file_route_proto_goTypes,
		DependencyIndexes: file_route_proto_depIdxs,
		MessageInfos:      file_route_proto_msgTypes,
	}.Build()
	File_route_proto = out.File
	file_route_proto_goTypes = nil
	file_route_proto_depIdxs = nil
}
