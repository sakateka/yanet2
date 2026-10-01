// Package ringtest holds cgo test helpers for the ring object: they reach
// into the C ring and the config registry in ways no production path does,
// so tests can control timing and state precisely.
//
//   - Writer drives the C ring writer against one worker's ring, to control
//     eviction and commit timing; Stress runs it on its own OS thread.
//   - Hold places an artificial extra reference on a published ring, to
//     reproduce a refused free without generation timing.
//   - LinkRing links a module config to a named ring, to reproduce the
//     refusal to delete a linked ring.
//
// Only tests import this package.
package ringtest
