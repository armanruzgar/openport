import { useLive } from "../api/live"
import { replayClock, sessionAt, speedLabel, useReplayControls } from "../views/ReplayView"
import { TradingError } from "./TradingControls"
import { Badge } from "./ui"

/** Always in view while trading a replay: its clock and pace, and the way back to live. */
export function ReplayBanner() {
  const live = useLive()
  const controls = useReplayControls()
  const replay = live.replay
  if (live.source === "live" || !replay) return null
  const sessions = replay.sessions ?? []
  const at = sessions.length > 1 ? sessionAt(sessions, replay.time) : null
  return (
    <div role="status" aria-label="Replay" className="border-b border-warn/40 bg-warn/10 px-4 py-1.5 text-xs">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1">
        <Badge tone={replay.demo ? "accent" : "warn"}>{replay.demo ? "Demo" : "Replay"}</Badge>
        <span className="font-medium tabular">{replayClock(replay.time)}</span>
        {at && <span className="text-muted">{at.open ? `${sessions[at.index]!.session} session` : `closed until the ${sessions[at.index]!.session} session`} · {at.index + 1} of {sessions.length}</span>}
        <span className="text-muted">{replay.demo ? `${replay.file} · ${replay.seed ? `seed ${replay.seed} · ` : ""}Simulated prices, not market data` : `${replay.file} · recording`}</span>
        <span className="text-muted">{live.source.startsWith("history:") ? "read-only replay" : replay.fast_forwarding ? `Preparing start state… ${Math.round((replay.progress ?? 0) * 100)}%` : replay.stepping ? "Stepping…" : replay.finished ? "finished" : replay.paused ? "paused" : speedLabel(replay.speed)}</span>
        {replay.skip_pending && <span className="text-warn">Skip queued for next resume</span>}
        {replay.pause_at && <span>Playing to {replayClock(replay.pause_at)}</span>}
        <span className="ml-auto flex flex-wrap gap-1">
          {!replay.finished && !replay.fast_forwarding && !replay.stepping && <button type="button" className="trade-button !py-0.5" disabled={controls.pending || controls.blocked}
            onClick={() => void controls.pause(!replay.paused)}>{replay.paused ? "Resume" : "Pause"}</button>}
          {!replay.finished && !replay.fast_forwarding && !replay.stepping && !replay.paused && <button type="button" className="trade-button !py-0.5" disabled={controls.pending || controls.blocked}
            onClick={() => void controls.skip()}>Skip gap</button>}
          {(replay.stepping || replay.pause_at) && !live.source.startsWith("history:") && <button type="button" className="trade-button !py-0.5" disabled={controls.interruptPending || controls.blocked} onClick={() => void controls.abort()}>Abort</button>}
          {replay.demo && replay.scenario && replay.seed && !live.source.startsWith("history:") && <button type="button" className="trade-button !py-0.5" disabled={controls.pending || controls.blocked}
            onClick={() => void controls.start({ demo: replay.scenario! }, replay.speed, () => live.switchSource("replay", "user"), { seed: replay.seed!, plan: replay.plan, start_at: replay.start_at || undefined, date: replay.date })}>Replay this seed</button>}
          <button type="button" className="trade-button !py-0.5" onClick={() => live.switchSource("live", "user")}>Back to live</button>
        </span>
      </div>
      <TradingError error={controls.error} />
    </div>
  )
}
