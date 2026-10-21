import type { ReactNode } from "react"
import type { Volatility } from "../api/types"
import { LineChart } from "../charts/LineChart"
import { Panel, Stat } from "./ui"
import { IvHistoryBadge } from "./VolatilityHistory"
import { expiryLabel } from "../lib/format"
import { coneSeries, metricMark, volPoints as vp } from "../lib/volatility"

const note = "† Truncated strike strip. ≈ Interpolated smile, ATM proxy or estimator fallback; hover for details. Volatility and skew are in vol points."

export function VolatilitySummary({ data }: { data: Volatility }) {
  const mfiv = data.mfiv.constant.find(v => v.days === 30)
  const mfiv9 = data.mfiv.constant.find(v => v.days === 9)
  const mfiv93 = data.mfiv.constant.find(v => v.days === 93)
  const atm = data.atm.constant.find(v => v.days === 30)
  const rv = data.realized.windows.find(v => v.sessions === 21)?.close_to_close
  return <div className="xl:col-span-2">
    <div className="grid grid-cols-2 gap-2 md:grid-cols-4 2xl:grid-cols-8">
      <Stat label="Model-free IV · 30d" value={vp(mfiv?.vol) + metricMark(mfiv)} hint={mfiv?.reason ?? data.sources.mfiv} />
      <Stat label="ATM IV · 30d" value={vp(atm?.vol) + metricMark(atm)} hint={atm?.reason ?? data.sources.atm} />
      <Stat label="25Δ RR / BF · 30d" value={`${vp(data.skew.delta25.rr)} / ${vp(data.skew.delta25.bf)}${metricMark(data.skew)}`} hint={data.skew.reason ?? data.skew.delta_convention} />
      <Stat label="Close RV · 21 sessions" value={vp(rv?.vol)} hint={rv?.reason ?? data.sources.realized} />
      <Stat label="VRP · spread / ratio" value={`${vp(data.vrp.spread)} / ${vp(data.vrp.ratio)}${metricMark(data.vrp)}`} hint={data.vrp.reason ?? data.sources.vrp} />
      <Stat label="Model-free IV · 9d/30d" value={vp(data.term.mfiv9_30, 3) + metricMark({ truncated: mfiv9?.truncated || mfiv?.truncated, proxy: mfiv9?.proxy || mfiv?.proxy })} hint={data.sources.term} />
      <Stat label="Model-free IV · 30d/93d" value={vp(data.term.mfiv30_93, 3) + metricMark({ truncated: mfiv?.truncated || mfiv93?.truncated, proxy: mfiv?.proxy || mfiv93?.proxy })} hint={data.sources.term} />
      <Stat label="ATM IV · 30d − 7d" value={vp(data.term.atm30_7) + metricMark({ proxy: atm?.proxy || data.atm.constant.find(v => v.days === 7)?.proxy })} />
    </div>
    <div className="mt-2"><IvHistoryBadge data={data} /></div>
    <p className="mt-2 text-[11px] text-muted">{note}</p>
  </div>
}

function Table({ headings, children, label }: { headings: string[]; children: ReactNode; label: string }) {
  return <div className="max-h-80 overflow-auto"><table aria-label={label} className="w-full whitespace-nowrap text-xs">
    <thead className="sticky top-0 bg-panel text-muted"><tr>{headings.map((h, i) => <th key={h} className={`px-2 py-1 font-normal ${i === 0 ? "text-left" : "text-right"}`}>{h}</th>)}</tr></thead>
    <tbody className="tabular">{children}</tbody>
  </table></div>
}
function Cell({ children, title, first = false }: { children: ReactNode; title?: string; first?: boolean }) {
  return <td className={`px-2 py-1 ${first ? "text-left" : "text-right"}`} title={title}>{children}</td>
}

export function VolatilityDetails({ data }: { data: Volatility }) {
  return <div className="grid gap-3 xl:col-span-2 xl:grid-cols-2">
    <Panel title="Skew by expiry">
      <p className="mb-2 text-[11px] text-muted">{data.skew.delta_convention}. Slope and curvature are at ln(K/F) = 0.</p>
      <Table label="Expiry skew" headings={["Expiry / source", "25Δ RR", "25Δ BF", "10Δ RR", "10Δ BF", "Slope", "Curvature"]}>
        {data.skew.expiries.map(e => <tr key={e.id} className="border-t border-border/40">
          <Cell first title={e.source}>{expiryLabel(e.id, true)}{e.source === "svi" ? " · SVI" : " ≈ smile"}</Cell>
          <Cell title={e.delta25.call.reason ?? e.delta25.put.reason ?? undefined}>{vp(e.delta25.rr)}</Cell><Cell>{vp(e.delta25.bf)}</Cell>
          <Cell title={e.delta10.call.reason ?? e.delta10.put.reason ?? undefined}>{vp(e.delta10.rr)}</Cell><Cell>{vp(e.delta10.bf)}</Cell>
          <Cell>{vp(e.slope)}</Cell><Cell>{vp(e.curvature)}</Cell>
        </tr>)}
      </Table>
      <details className="mt-3 text-xs text-muted"><summary className="cursor-pointer">Model-free IV strip coverage</summary>
        <Table label="Model-free IV coverage" headings={["Expiry", "IV", "K₀", "Strike range", "Count", "Stops · put/call", "EEP"]}>
          {data.mfiv.expiries.map(e => <tr key={e.id} className="border-t border-border/40"><Cell first>{expiryLabel(e.id, true)}</Cell>
            <Cell title={e.reason ?? undefined}>{vp(e.vol)}{metricMark(e)}</Cell><Cell>{vp(e.k0)}</Cell>
            <Cell>{vp(e.low, 0)}–{vp(e.high, 0)}</Cell><Cell>{e.strikes}</Cell><Cell>{e.lower_stop} / {e.upper_stop}</Cell><Cell>{e.eep}</Cell>
          </tr>)}
        </Table>
      </details>
    </Panel>
    <Panel title="Realized volatility">
      <Table label="Realized volatility estimators" headings={["Sessions", "Close–close", "Parkinson", "Garman–Klass", "Yang–Zhang"]}>
        {data.realized.windows.map(w => <tr key={w.sessions} className="border-t border-border/40"><Cell first>{w.sessions}</Cell>
          {(["close_to_close", "parkinson", "garman_klass", "yang_zhang"] as const).map(key => <Cell key={key} title={w[key].reason ?? undefined}>{vp(w[key].vol)}{metricMark(w[key])}</Cell>)}
        </tr>)}
      </Table>
      <p className="mt-2 text-[11px] text-muted">Daily bars through {data.realized.daily_as_of ?? "—"} · 252 sessions/year · ≈ uses close-to-close for an invalid OHLC window.</p>
      <details className="mt-3 text-xs"><summary className="cursor-pointer text-muted">5-minute RV · today {vp(data.realized.today)}</summary>
        <Table label="Intraday realized volatility" headings={["Session", "RV", "Minutes observed", "Coverage"]}>
          {data.realized.intraday.map(d => <tr key={d.date}><Cell first>{d.date}</Cell><Cell title={d.reason ?? undefined}>{vp(d.vol)}</Cell><Cell>{d.observed_minutes} / {d.session_minutes}</Cell><Cell>{d.partial ? "partial" : "full"}</Cell></tr>)}
        </Table>
      </details>
    </Panel>
    <Panel title="Volatility cones">
      <LineChart series={coneSeries(data.realized.cones)} height={250} formatX={x => `${x}d`} formatY={x => vp(x, 1)} xTicks={data.realized.cones.map(c => c.sessions)} />
      <p className="mt-2 text-[11px] text-muted">Sessions on x-axis. Bands: 10–90% and 25–75%; line: median; the extremes are in the table. <span style={{ color: "var(--chart-2)" }}>Dots: current RV.</span> <span style={{ color: "var(--chart-3)" }}>Dots: ATM IV to the matching session close.</span> Missing brackets have no marker.</p>
      <Table label="Vol cone observations" headings={["Sessions", "Days used", "Rolling windows", "Min", "Max", "Current percentile", "ATM IV"]}>
        {data.realized.cones.map(c => <tr key={c.sessions}><Cell first>{c.sessions}</Cell><Cell>{c.days_used}</Cell><Cell>{c.observations}</Cell><Cell>{vp(c.min, 1)}</Cell><Cell>{vp(c.max, 1)}</Cell><Cell>{vp(c.current_percentile, 1)}{c.current_percentile == null ? "" : "%"}</Cell>
          <Cell title={c.implied_reason ?? `${vp(c.implied_days, 2)} calendar days`}>{vp(c.implied_vol)}{metricMark({ proxy: c.implied_proxy })}</Cell></tr>)}
      </Table>
    </Panel>
    <Panel title="Implied move by session">
      <p className="mb-2 text-[11px] text-muted" title={data.implied_moves.today_reason ?? undefined}>Today remaining: {vp(data.implied_moves.today_points)}{metricMark({ proxy: data.implied_moves.today_proxy, truncated: data.implied_moves.today_truncated })} points / {vp(data.implied_moves.today_percent)}%. {data.implied_moves.today_calendar_arbitrage && "Calendar arbitrage flagged. "}One standard deviation; shared intervals do not isolate an event's premium.</p>
      <Table label="Implied moves" headings={["Session / event", "Points", "% of forward", "Allocation"]}>
        {data.implied_moves.sessions.map(m => <tr key={m.date} className={`border-t border-border/40 ${m.calendar_arbitrage ? "text-warn" : ""}`}>
          <Cell first><div>{m.date}</div>{m.label && <div className="max-w-48 whitespace-normal text-muted">{m.label}</div>}</Cell>
          <Cell title={m.reason ?? undefined}>{vp(m.points)}{metricMark(m)}</Cell><Cell>{vp(m.percent)}</Cell>
          <Cell>{m.calendar_arbitrage ? "Calendar arbitrage" : m.reason ? m.reason.replaceAll("_", " ") : m.shared ? "Shared" : "Single session"}</Cell>
        </tr>)}
      </Table>
      <p className="mt-2 text-[11px] text-muted">{data.sources.implied_moves}</p>
    </Panel>
    {!!data.event_volatility?.length && <Panel title="Event volatility · derived two-expiry estimate" className="xl:col-span-2">
      <Table label="Event volatility" headings={["Event / session", "Implied event move", "Points", "Expiries / status"]}>
        {data.event_volatility.map((e, i) => <tr key={`${e.date}-${i}`} className="border-t border-border/40">
          <Cell first><div>{e.label}</div><div className="text-muted">{e.date} · move session {e.session}{e.timing === "session" && " · timing assumed at open"}</div></Cell>
          <Cell>{vp(e.percent)}{e.percent == null ? "" : "%"}{metricMark(e)}</Cell><Cell>{vp(e.points)}</Cell>
          <Cell title={`Baseline variance ${vp(e.baseline_variance, 6)}; premium ${vp(e.variance_premium, 6)}`}>
            {e.near && e.next && <div>{e.near} / {e.next}</div>}{e.reason?.replaceAll("_", " ") ?? "Derived event premium"}
          </Cell>
        </tr>)}
      </Table>
      <p className="mt-2 text-[11px] text-muted">One standard deviation from excess variance above a constant business-session baseline, using adjacent expiries on both sides of the event. AMC belongs to the next session. Other known events in the solve, missing brackets or negative premiums leave the move unavailable. This model estimate is not an observed or forecast event return.</p>
    </Panel>}
  </div>
}
