import type { ReplayListing } from "./types"

/** Timer-driven and clock-injected so throttling and reconnects are reproducible. */
export function watchReplay(options: {
  now: () => number
  seen: () => number
  probe: (signal: AbortSignal) => Promise<ReplayListing>
  reconnecting: (value: boolean) => void
  leave: (reason: string) => void
  quietMs?: number
  unreachableMs?: number
}) {
  let failedSince: number | undefined
  let disposed = false
  let busy = false
  let controller: AbortController | undefined
  const timer = setInterval(() => { void check() }, 1_000)
  async function check() {
    if (disposed) return
    if (options.now() - options.seen() <= (options.quietMs ?? 5_000)) {
      failedSince = undefined
      options.reconnecting(false)
      return
    }
    options.reconnecting(true)
    if (busy) return
    busy = true
    const seen = options.seen()
    controller = new AbortController()
    let timeout: ReturnType<typeof setTimeout> | undefined
    try {
      const listing = await Promise.race([
        options.probe(controller.signal),
        new Promise<never>((_, reject) => { timeout = setTimeout(() => { controller?.abort(); reject(new Error("Replay probe timed out")) }, 10_000) }),
      ])
      if (disposed || options.seen() !== seen) return
      failedSince = undefined
      if (!listing.replay || listing.replay.finished) options.leave(listing.replay ? "The replay finished." : "The replay stopped in another window.")
    } catch {
      if (disposed || options.seen() !== seen) return
      failedSince ??= options.now()
      if (options.now() - failedSince >= (options.unreachableMs ?? 120_000)) options.leave("The replay server has been unreachable for two minutes.")
    } finally { clearTimeout(timeout); busy = false }
  }
  return () => { disposed = true; clearInterval(timer); controller?.abort() }
}
