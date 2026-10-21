import { marketNow, useLive } from "../api/live"
import { Panel, FeedBadge } from "../components/ui"
import { AsOf } from "../components/AsOf"
import { UpdateSettings } from "../components/UpdateNotice"
import { Diagnostics } from "../components/Diagnostics"
import { clock, count, fixed, price } from "../lib/format"
import { timestampET } from "../lib/freshness"
import { providerLabel } from "../lib/provider"

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex min-w-0 items-baseline justify-between gap-4 border-b border-border/40 py-1.5 text-sm last:border-0">
      <span className="shrink-0 text-muted">{label}</span>
      <span className="min-w-0 tabular text-right [overflow-wrap:anywhere]">{children}</span>
    </div>
  )
}

function Capability({ on, label }: { on: boolean; label: string }) {
  return <span className={`rounded-full border px-2 py-0.5 text-[11px] ${on ? "border-live/50 text-live" : "border-border text-faint line-through"}`}>{label}</span>
}

export function EngineView() {
  const live = useLive()
  const { status, tick, market, underlyings } = live
  if (!status) return null
  const { provider } = status
  const feed = tick?.feed ?? status.feed
  const engine = { ...status.engine, ...tick?.engine }

  return (
    <div className="grid gap-3 lg:grid-cols-2">
      <UpdateSettings />
      <Diagnostics />
      <Panel title="Provider">
        <Row label="Name">{provider.name}</Row>
        <Row label="Data">{providerLabel(provider, feed.state)}</Row>
        {provider.poll_interval_seconds != null && <Row label="Poll interval">{fixed(provider.poll_interval_seconds, 0)} s</Row>}
        <div className="flex flex-wrap gap-1.5 pt-2">
          <Capability on label="quotes" />
          <Capability on={provider.trades} label="trades" />
          <Capability on={provider.open_interest} label="open interest" />
          <Capability on={provider.vendor_greeks} label="vendor greeks" />
        </div>
      </Panel>

      <Panel title="Feed">
        <Row label="State">
          <FeedBadge state={feed.state} message={feed.message} />
        </Row>
        <Row label="Last message">
          <span className="block space-y-1 text-left text-xs">
            {(feed.message ?? "—").split("; ").map((message, index) => <span key={index} className="block">{message}</span>)}
          </span>
        </Row>
        <Row label="Updated">{clock(status.feed.updated)}</Row>
      </Panel>

      <Panel title="Engine" actions={engine.overloaded === true ? <span className="rounded-full border border-warn px-2 py-0.5 text-[11px] font-medium text-warn">overloaded</span> : undefined}>
        <Row label="Contracts tracked">{count(engine.contracts)}</Row>
        <Row label="Nonstandard contracts">{count(engine.nonstandard_contracts)}</Row>
        <Row label="Events processed">{count(engine.events)}</Row>
        <Row label="Events per second">{fixed(engine.events_per_second, 1)}</Row>
        <Row label="Last analytics pass">{fixed(engine.analytics_ms, 1)} ms</Row>
        <Row label="Queue depth">{count(engine.queue_depth)}</Row>
        <Row label="Coalesced events">{count(engine.coalesced_events)}</Row>
        <Row label="Dropped events">{count(engine.dropped_events)}</Row>
        <Row label="Uptime">{Math.floor(engine.uptime_seconds / 60)} min</Row>
      </Panel>

      <Panel title="Volatility history store">
        <Row label="State">{status.series?.enabled ? "Enabled" : "Disabled"}</Row>
        <Row label="Directory">{status.series?.directory || "—"}</Row>
        <Row label="Rows today">{count(status.series?.rows_today ?? 0)}</Row>
        <Row label="Last write · market time">{status.series?.last_write ? timestampET(status.series.last_write) : "—"}</Row>
        <Row label="Last error"><span className={status.series?.last_error ? "text-warn" : ""}>{status.series?.last_error ?? "—"}</span></Row>
      </Panel>

      <Panel title="Underlyings">
        <div className="overflow-x-auto">
          <table className="w-full text-sm">
            <thead className="text-muted">
              <tr>
                <th className="py-1 text-left font-normal">Symbol</th>
                <th className="py-1 text-right font-normal">Spot</th>
                <th className="py-1 text-right font-normal">Expiries</th>
                <th className="py-1 text-right font-normal">Options priced</th>
                <th className="py-1 text-right font-normal">As of</th>
              </tr>
            </thead>
            <tbody className="tabular">
              {underlyings.map((u) => (
                <tr key={u.symbol} className="border-t border-border/40">
                  <td className="py-1">{u.symbol}</td>
                  <td className="py-1 text-right">{price(u.spot)}</td>
                  <td className="py-1 text-right">{count(u.expiries)}</td>
                  <td className="py-1 text-right">{count(u.options)}</td>
                  <td className="py-1 text-right"><AsOf asOf={u.as_of} delaySeconds={provider.delay_seconds} market={market} session={u.session}
                    now={live.source === "replay" || provider?.simulated ? marketNow(live) : undefined} /></td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </Panel>

      <Panel title="Underlying health" className="lg:col-span-2">
        <div className="overflow-x-auto">
          <table className="w-full text-left text-xs">
            <thead className="text-muted">
              <tr>
                <th scope="col" className="px-2 py-1 font-normal">Symbol</th>
                <th scope="col" className="px-2 py-1 font-normal">State</th>
                <th scope="col" className="px-2 py-1 font-normal">Message</th>
                <th scope="col" className="px-2 py-1 font-normal">Last success</th>
                <th scope="col" className="px-2 py-1 font-normal">Last error</th>
              </tr>
            </thead>
            <tbody>
              {underlyings.map((u) => (
                <tr key={u.symbol} className="border-t border-border/40 align-top">
                  <th scope="row" className="px-2 py-2 font-normal tabular">{u.symbol}</th>
                  <td className="px-2 py-2">{u.state ? <FeedBadge state={u.state} message={u.message} /> : "—"}</td>
                  <td className="min-w-40 px-2 py-2 text-muted break-words">{u.message ?? "—"}</td>
                  <td className="min-w-44 px-2 py-2 tabular">{u.last_success ? <AsOf asOf={u.last_success} showBadge={false} now={provider.simulated ? marketNow(live) : undefined} /> : "—"}</td>
                  <td className="min-w-44 px-2 py-2 break-words">
                    <div className={u.last_error ? "text-warn" : "text-muted"}>{u.last_error ?? "—"}</div>
                    {u.last_error_time && <time dateTime={u.last_error_time} className="text-[11px] text-muted tabular">{timestampET(u.last_error_time)}</time>}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </Panel>

      <Panel title="How the numbers are made" className="lg:col-span-2">
        <ul className="list-disc space-y-1.5 pl-5 text-sm text-muted">
          <li>
            Every provider is normalised into the same contracts, quotes, trades and open interest, so the analytics are identical whichever data
            you plug in. Where a provider publishes its own IVs, the chain shows how closely they agree.
          </li>
          <li>
            Each expiry's forward and discount factor come from a weighted put-call parity fit (C − P = D(F − K)) over near-the-money strikes; short
            expiries borrow the rate fitted on longer ones. No rates or dividend feed needed.
          </li>
          <li>
            Implied vols are solved with Newton's method on log price from a Corrado-Miller start, safeguarded by bisection; each strike's smile vol
            comes from its out-of-the-money side and drives both sides' Greeks.
          </li>
          <li>The C++20 engine re-prices every chain each second on the data's own clock, so delayed feeds get the right time to expiry.</li>
        </ul>
      </Panel>
    </div>
  )
}
