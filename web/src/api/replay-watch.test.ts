import { afterEach, expect, it, vi } from "vitest"
import { watchReplay } from "./replay-watch"
import type { ReplayListing } from "./types"
let stop: (() => void) | undefined
afterEach(() => { stop?.(); vi.useRealTimers() })
function setup(probe: (signal: AbortSignal) => Promise<ReplayListing>) {
  vi.useFakeTimers()
  vi.setSystemTime(0)
  let seen = 0
  const reconnecting = vi.fn(), leave = vi.fn()
  stop = watchReplay({ now: Date.now, seen: () => seen, probe, reconnecting, leave })
  return { reconnecting, leave, tick: () => { seen = Date.now() } }
}
it("keeps an alive paused run through quiet ticks and clears reconnection on a tick", async () => {
  const probe = vi.fn(async () => ({ replay: { id: "run", paused: true, finished: false } }) as ReplayListing)
  const state = setup(probe)
  await vi.advanceTimersByTimeAsync(150_000)
  expect(probe).toHaveBeenCalled()
  expect(state.leave).not.toHaveBeenCalled()
  expect(state.reconnecting).toHaveBeenLastCalledWith(true)
  state.tick()
  await vi.advanceTimersByTimeAsync(1_000)
  expect(state.reconnecting).toHaveBeenLastCalledWith(false)
})
it.each([null, { finished: true }])("leaves only after the server reports stopped or finished: %j", async replay => {
  const state = setup(async () => ({ replay }) as ReplayListing)
  await vi.advanceTimersByTimeAsync(6_000)
  expect(state.leave).toHaveBeenCalledOnce()
})
it("waits two minutes of failed probes rather than falling back on a stall", async () => {
  const state = setup(async () => { throw new Error("offline") })
  await vi.advanceTimersByTimeAsync(125_000)
  expect(state.leave).not.toHaveBeenCalled()
  await vi.advanceTimersByTimeAsync(1_000)
  expect(state.leave).toHaveBeenCalledOnce()
})
it("times out hung probes and eventually leaves", async () => {
  const state = setup(() => new Promise(() => {}))
  await vi.advanceTimersByTimeAsync(150_000)
  expect(state.leave).toHaveBeenCalled()
})
it("ignores a late stopped response after a new tick", async () => {
  let resolve!: (listing: ReplayListing) => void
  const state = setup(() => new Promise(done => { resolve = done }))
  await vi.advanceTimersByTimeAsync(6_000)
  state.tick()
  resolve({ replay: null } as ReplayListing)
  await vi.advanceTimersByTimeAsync(1)
  expect(state.leave).not.toHaveBeenCalled()
})
