package rib

import (
	"net/netip"
	"sync"
	"sync/atomic"
	"time"

	"go.uber.org/zap"

	"github.com/yanet-platform/yanet2/controlplane/modules/route/internal/discovery"
	"github.com/yanet-platform/yanet2/controlplane/modules/route/internal/discovery/neigh"
)

type RIB struct {
	mu         sync.RWMutex
	routes     MapTrie[netip.Prefix, netip.Addr, RoutesList]
	neighbours *neigh.NexthopCache
	changedAt  *atomic.Int64
	log        *zap.SugaredLogger
}

func NewRIB(neighbours *neigh.NexthopCache, log *zap.SugaredLogger) *RIB {
	changedAt := atomic.Int64{}
	changedAt.Store(time.Now().UnixNano())
	return &RIB{
		routes:     NewMapTrie[netip.Prefix, netip.Addr, RoutesList](1024),
		neighbours: neighbours,
		changedAt:  &changedAt,
		log:        log,
	}
}

func (m *RIB) NeighboursView() discovery.CacheView[netip.Addr, neigh.NeighbourEntry] {
	return m.neighbours.View()

}

func (m *RIB) AddUnicastRoute(prefix netip.Prefix, nexthopAddr netip.Addr) error {
	m.log.Debugf("adding unicast route %q via %q", prefix, nexthopAddr)

	route := MakeStaticRoute()
	route.Prefix = prefix
	route.NextHop = nexthopAddr

	m.mu.Lock()
	m.routes.InsertOrUpdate(
		route.Prefix,
		func() RoutesList {
			return RoutesList{
				Routes: []*Route{route},
			}
		},
		func(m RoutesList) RoutesList {
			m.Insert(route)
			return m
		},
	)
	m.mu.Unlock()
	m.changedAt.Store(time.Now().UnixNano())

	m.log.Infow("added unicast route",
		zap.Stringer("prefix", prefix),
		zap.Stringer("nexthop_addr", nexthopAddr),
	)

	return nil
}

func (m *RIB) DumpRoutes() map[netip.Prefix]RoutesList {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.routes.Dump()
}

func (m *RIB) BulkUpdate(routes []*Route) {
	m.mu.Lock()
	for _, route := range routes {
		if route.ToRemove {
			m.routes.UpdateOrDelete(
				route.Prefix,
				func(m RoutesList) (RoutesList, bool) {
					m.Remove(route)
					return m, len(m.Routes) == 0
				},
			)
		} else {
			m.routes.InsertOrUpdate(
				route.Prefix,
				func() RoutesList {
					return RoutesList{
						Routes: []*Route{route},
					}
				},
				func(m RoutesList) RoutesList {
					m.Insert(route)
					return m
				},
			)
		}
	}
	m.mu.Unlock()
	m.changedAt.Store(time.Now().UnixNano())
}

func (m *RIB) UpdatedAt() time.Time {
	return time.Unix(0, m.changedAt.Load())
}
