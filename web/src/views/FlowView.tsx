import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { FlowPrint, FlowQuery } from "../api/types"
import { HBarChart } from "../charts/HBarChart"
import { Badge, Empty, Panel, Segmented, Stat } from "../components/ui"
import { clock, count, expiryLabel, fixed, money, price } from "../lib/format"
import { emptyTapeText, flowTags, sideText, strikeBars } from "../lib/flow"
import { matchingPayload } from "../lib/payload"

type Only = "all" | NonNullable<FlowQuery["only"]>
type Kind = "both" | "call" | "put"
type Sides = "all" | "aggressive"

const sideTone: Record<FlowPrint["side"], string> = {
  ask: "text-bullish", bid: "text-bearish", mid: "text-muted", outside: "text-warn", unquoted: "text-faint",
}

/** Time and sales with net premium by expiry and strike. */
export function FlowView({ symbol }: { symbol: string }) {
  const version = useLive().version(symbol)
  const [kind, setKind] = useState<Kind>("both")
  const [only, setOnly] = useState<Only>("all")
  const [sides, setSides] = useState<Sides>("all")
  const [minPremium, setMinPremium] = useState(0)
  const [expiry, setExpiry] = useState<string | null>(null)
  const query: FlowQuery = {
    limit: 200,
    ...(kind !== "both" ? { type: kind } : {}),
    ...(only !== "all" ? { only } : {}),
    ...(sides === "aggressive" ? { side: ["bid", "ask", "outside"] } : {}),
    ...(minPremium ? { min_premium: minPremium } : {}),
  }
  const flow = useQuery({
    queryKey: ["flow", symbol, query, version],
    queryFn: ({ signal }) => api.flow(symbol, query, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol),
    refetchInterval: 5_000,
  })
  const data = matchingPayload(flow.data, symbol)
  if (flow.isError) return <Empty>{String(flow.error)}</Empty>
  if (!data) return <Empty>Loading the tape…</Empty>
  if (data.source === "none") {
    return <Panel title="Time and sales">
      <Empty><div className="max-w-md text-center"><div>{data.message}</div>
        <div className="mt-2 text-xs text-faint">Flow needs a feed that delivers option trades, such as Databento, or a recording or ThetaData import that has them.</div></div></Empty>
    </Panel>
  }
  const shownExpiry = expiry != null && data.expiries.some((e) => e.expiry === expiry) ? expiry : null
  const totals = data.totals
  return (
    <div className="flex flex-col gap-3">
      {data.source === "simulated" && <p role="note" className="rounded-md border border-warn/40 bg-warn/10 px-3 py-2 text-xs text-warn">{data.message}</p>}
      <div className="flex flex-wrap items-center gap-2">
        <Segmented label="Calls or puts" value={kind} onChange={setKind}
          options={[{ value: "both", label: "All" }, { value: "call", label: "Calls" }, { value: "put", label: "Puts" }]} />
        <Segmented label="Sides" value={sides} onChange={setSides}
          options={[{ value: "all", label: "Every print" }, { value: "aggressive", label: "Bid, ask, outside" }]} />
        <Segmented label="Kind of print" value={only} onChange={setOnly}
          options={[{ value: "all", label: "Any" }, { value: "sweep", label: "Sweeps" }, { value: "block", label: "Blocks" }, { value: "multi_leg", label: "Multi-leg" }]} />
        <Segmented label="Minimum premium" value={minPremium} onChange={setMinPremium}
          options={[0, 10_000, 100_000].map((v) => ({ value: v, label: v ? `≥ ${money(v)}` : "Any premium" }))} />
      </div>
      <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-5">
        <Stat label="Net premium" value={money(totals.net)} tone={totals.net < 0 ? "negative" : "positive"} hint="Premium bought at or above the ask minus premium sold at or below the bid" />
        <Stat label="Call net" value={money(totals.call_net)} tone={totals.call_net < 0 ? "negative" : "positive"} hint="Net premium in calls" />
        <Stat label="Put net" value={money(totals.put_net)} tone={totals.put_net < 0 ? "negative" : "positive"} hint="Net premium in puts" />
        <Stat label="Unclassified" value={money(totals.unclassified)} hint="Premium printed between the quotes or without a quote" />
        <Stat label="Prints" value={`${count(data.matched)} / ${count(data.retained)}`} hint={`Matching prints of the last ${count(data.capacity)} kept; ${count(data.total)} seen since start`} />
      </div>
      <div className="grid gap-3 xl:grid-cols-[1fr_2fr]">
        <div className="flex flex-col gap-3">
          <Panel title="Net premium by expiry">
            {data.expiries.length ? (
              <table className="w-full text-xs tabular">
                <thead className="text-muted"><tr>
                  <th className="py-1 text-left font-normal">Expiry</th><th className="py-1 text-right font-normal">Calls</th>
                  <th className="py-1 text-right font-normal">Puts</th><th className="py-1 text-right font-normal">Net</th>
                </tr></thead>
                <tbody>{data.expiries.map((row) => (
                  <tr key={row.expiry} aria-selected={row.expiry === shownExpiry}
                    className={`cursor-pointer border-t border-border/50 ${row.expiry === shownExpiry ? "bg-raised" : "hover:bg-raised/60"}`}
                    onClick={() => setExpiry(row.expiry === shownExpiry ? null : row.expiry)}>
                    <td className="py-1">{expiryLabel(row.expiry, true)}</td>
                    <td className={`py-1 text-right ${row.call_net < 0 ? "text-bearish" : "text-bullish"}`}>{money(row.call_net)}</td>
                    <td className={`py-1 text-right ${row.put_net < 0 ? "text-bearish" : "text-bullish"}`}>{money(row.put_net)}</td>
                    <td className="py-1 text-right">{money(row.net)}</td>
                  </tr>))}</tbody>
              </table>
            ) : <Empty>{emptyTapeText(data)}</Empty>}
          </Panel>
          <Panel title={shownExpiry ? `Net premium by strike · ${expiryLabel(shownExpiry, true)}` : "Net premium by strike · largest"}>
            {data.strikes.length
              ? <HBarChart rows={strikeBars(data, shownExpiry)} signed format={(v) => money(v)} label="Net premium by strike" />
              : <Empty>{emptyTapeText(data)}</Empty>}
            <p className="mt-2 text-[11px] text-muted">Select an expiry to see all its strikes.</p>
          </Panel>
        </div>
        <Panel title="Time and sales" actions={data.as_of && <span className="text-[11px] text-muted">last print {clock(data.as_of)}</span>}>
          {data.prints.length ? (
            <div className="max-h-[32rem] overflow-auto">
              <table className="w-full text-xs tabular">
                <thead className="sticky top-0 bg-panel text-muted"><tr>
                  <th className="py-1 text-left font-normal">Time</th><th className="py-1 text-left font-normal">Contract</th>
                  <th className="py-1 text-right font-normal">Size</th><th className="py-1 text-right font-normal">Price</th>
                  <th className="py-1 text-right font-normal">Bid × Ask</th><th className="py-1 text-left font-normal pl-3">Side</th>
                  <th className="py-1 text-right font-normal">Premium</th><th className="py-1 text-left font-normal pl-3">Tags</th>
                </tr></thead>
                <tbody>{data.prints.map((p) => (
                  <tr key={p.sequence} className="border-t border-border/50">
                    <td className="py-1 whitespace-nowrap">{clock(p.time)}</td>
                    <td className="py-1 whitespace-nowrap">{expiryLabel(p.expiry)} {fixed(p.strike, p.strike % 1 ? 1 : 0)}{p.type === "call" ? "C" : "P"}</td>
                    <td className="py-1 text-right">{count(p.size)}</td>
                    <td className="py-1 text-right">{price(p.price)}</td>
                    <td className="py-1 text-right text-muted whitespace-nowrap">{p.bid != null ? `${price(p.bid)} × ${price(p.ask)}` : "—"}</td>
                    <td className={`py-1 pl-3 whitespace-nowrap ${sideTone[p.side]}`}>{sideText(p)}</td>
                    <td className="py-1 text-right">{money(p.premium)}</td>
                    <td className="py-1 pl-3"><span className="flex flex-wrap gap-1">
                      {flowTags(p).map((tag) => <Badge key={tag} tone={tag === "block" ? "accent" : "warn"}>{tag}</Badge>)}
                      {p.simulated && <Badge title="Generated by the demo market, not a market trade">simulated</Badge>}
                    </span></td>
                  </tr>))}</tbody>
              </table>
            </div>
          ) : <Empty>{emptyTapeText(data)}</Empty>}
          <p className="mt-2 text-[11px] text-muted">
            Each print is compared with the option's quote when it arrived. Sweeps (one contract across venues within a second),
            blocks (≥ {count(data.block_size)} contracts) and multi-leg groups are inferred, not reported by the feed.
          </p>
        </Panel>
      </div>
    </div>
  )
}
