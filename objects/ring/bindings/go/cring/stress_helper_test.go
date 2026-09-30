package cring_test

import (
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring/internal/ringwriter"
)

func ringwriterValid(rec cring.Record) bool {
	return ringwriter.StressRecordValid(rec.Seqno, rec.Bytes)
}
