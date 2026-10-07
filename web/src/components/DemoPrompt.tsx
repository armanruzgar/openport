import { useQuery } from "@tanstack/react-query"
import { isSandboxToken, useWriteToken } from "../lib/write-token"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { demoPrompt, idleReason, nothingTrades, useDemoPromptHidden } from "../lib/demo"
import { joinList } from "../lib/format"
import type { View } from "../lib/route"
import { useReplayControls } from "../views/ReplayView"
import { TradingError } from "./TradingControls"

/** The server's demo market, offered while nothing on the live feed can be traded. */
export function useDemoOffer() {
  const live = useLive()
  const sandbox = isSandboxToken(useWriteToken())
  const idle = !sandbox && live.source === "live" && nothingTrades(live.underlyings)
  const listing = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), enabled: idle, staleTime: 30_000 })
  const controls = useReplayControls()
  const demo = listing.data?.demo
  return {
    offered: idle && !!demo && !listing.data?.replay,
    reason: idle ? idleReason(live.underlyings) : "",
    symbols: demo?.symbols ?? [],
    /** Every simulated day, the default first. */
    days: listing.data?.demos?.length ? listing.data.demos : demo ? [demo] : [],
    controls,
    /**
     * Starts a day (the default without one) at 10× and trades it, then calls
     * `started`; without write access, `blocked` shows where to get it.
     */
    start(blocked: () => void, day?: string, started?: () => void) {
      if (controls.blocked) blocked()
      else void controls.start({ demo: day ?? true }, 10, () => { live.switchSource("replay", "user"); started?.() })
    },
  }
}

/** A line under the header while nothing trades: markets closed or the feed stalled. */
export function DemoPrompt({ onNavigate }: { onNavigate: (view: View) => void }) {
  const hidden = useDemoPromptHidden()
  const offer = useDemoOffer()
  const [chosen, setChosen] = useState<string>()
  if (hidden || !offer.offered) return null
  const day = offer.days.find((d) => d.id === chosen) ?? offer.days[0]
  return (
    <div role="status" aria-label="Demo market" className="border-b border-border bg-raised/60 px-4 py-1.5 text-xs">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1">
        <span>{offer.reason}. The demo market plays a simulated day in {joinList(day?.symbols ?? offer.symbols)} options that you can trade.</span>
        <span className="ml-auto flex flex-wrap items-center gap-1">
          {offer.days.length > 1 && <select aria-label="Demo day" className="trade-input !py-0.5 text-xs" value={day?.id}
            title={day?.description} onChange={(event) => setChosen(event.target.value)}>
            {offer.days.map((d) => <option key={d.id} value={d.id}>{d.title}</option>)}
          </select>}
          <button type="button" className="trade-button border-accent !py-0.5 text-foreground" disabled={offer.controls.pending}
            onClick={() => offer.start(() => onNavigate("replay"), day?.id)}>{offer.controls.pending ? "Starting…" : "Try the demo"}</button>
          <button type="button" className="trade-button !py-0.5" onClick={() => demoPrompt.hide()}>Not now</button>
        </span>
      </div>
      {day?.description && <p className="mt-1 text-muted">{day.description} {day.goal && `Goal: ${day.goal}`} {day.session === "overnight" && "Overnight session."}</p>}
      <button type="button" className="trade-button mt-1 !py-0.5" onClick={() => onNavigate("replay")}>Choose a plan, seed and start time</button>
      <TradingError error={offer.controls.error} />
    </div>
  )
}
