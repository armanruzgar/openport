import type { QueryClient } from "@tanstack/react-query"
import type { Tick } from "./types"
import { writeToken } from "../lib/write-token"

export type Connection = "connecting" | "open" | "closed"

/** Subscribe once per provider; return cleanup for unmount and StrictMode. */
export function connectLive(
  url: string,
  queryClient: QueryClient,
  onTick: (tick: Tick | null) => void,
  onConnection: (connection: Connection) => void,
  onReplayTick: (tick: Tick | null) => void = () => {},
) {
  let socket: WebSocket | null = null
  let retry = 0
  let timer: ReturnType<typeof setTimeout> | undefined
  let disposed = false
  let replayState: string | undefined
  // Until this connection first opens or fails, the cache holds only what the page has
  // just asked for (LiveProvider invalidates everything on mount and on a new token).
  let fresh = true

  const connect = () => {
    const token = writeToken.get()
    const protocols = token ? ["openport", `openport.token.${Array.from(new TextEncoder().encode(token), (byte) => byte.toString(16).padStart(2, "0")).join("")}`] : undefined
    const current = protocols ? new WebSocket(url, protocols) : new WebSocket(url)
    socket = current
    current.onopen = () => {
      if (disposed || socket !== current) return
      retry = 0
      onTick(null)
      onConnection("open")
      // The first open comes moments after the page's own first requests: cancelling
      // and sending them again would only repeat them.
      if (fresh) { fresh = false; return }
      // A restarted server may reuse old version numbers. Invalidate every key,
      // including inactive ones, and cancel responses from the previous session.
      void queryClient.cancelQueries().then(() => {
        if (!disposed && socket === current) return queryClient.invalidateQueries()
      })
    }
    current.onmessage = (event: MessageEvent<string>) => {
      if (disposed || socket !== current) return
      const message = JSON.parse(event.data) as Tick
      if (message.type === "tick") onTick(message)
      else if (message.type === "replay_tick") {
        onReplayTick(message)
        // The switcher and Replay page share this listing. Refresh on each run's
        // start/finish, even when the page is not mounted, rather than every quote.
        if (message.replay) {
          const next = `${message.replay.id ?? message.replay.started}:${message.replay.finished}`
          if (next !== replayState) {
            replayState = next
            void queryClient.invalidateQueries({ queryKey: ["replay-listing"] })
          }
        }
      }
    }
    current.onclose = () => {
      if (disposed || socket !== current) return
      fresh = false
      onTick(null)
      onReplayTick(null)
      onConnection("closed")
      void queryClient.invalidateQueries({ queryKey: ["status"] })
      timer = setTimeout(connect, Math.min(10_000, 500 * 2 ** retry++))
    }
  }
  connect()
  return () => {
    disposed = true
    clearTimeout(timer)
    socket?.close()
  }
}
