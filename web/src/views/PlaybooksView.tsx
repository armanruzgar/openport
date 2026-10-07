import { useActionApi } from "../api/action-client"
import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useState } from "react"

import { useLive } from "../api/live"
import type { ForwardTest, Playbook, PlaybookStats } from "../api/playbook-types"
import { PassOddsCard, StagedOrders, usePlaybooks } from "../components/Playbooks"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Dialog } from "../components/Dialog"
import type { BacktestState } from "../api/backtest-types"
import { timestampET } from "../lib/freshness"
import { Empty, PageHeader, Panel } from "../components/ui"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"

export const newPlaybook: Playbook = {
  id: "put-spread", name: "Morning put spread", description: "Write the setup and its invalidation here.", underlyings: ["SPX"],
  window: { start: "09:45", end: "11:00", weekdays: [1, 2, 3, 4, 5] }, conditions: {},
  structure: { template: { kind: "vertical", type: "put", direction: "credit", target: { mode: "delta", value: 15 }, width: 5 }, expiry: { min: 0, max: 1 } },
  sizing: { units: 1 }, management: { take_profit_percent: 50, stop_credit_multiple: 2, close_by: "15:45" },
  guardrails: { max_entries_per_day: 1, cooldown_minutes: 30 },
}
const pct = (value: number | null) => value == null ? "—" : `${(100 * value).toFixed(1)}%`
function Stats({ rows }: { rows: [string, PlaybookStats][] }) {
  return <div className="overflow-x-auto"><table className="w-full text-left text-xs tabular">
    <thead className="text-muted"><tr>{["Trades", "Count", "Win rate", "Average win", "Average loss", "Expectancy", "Profit factor", "Average R", "Return on BP", "Adherence"].map((label) => <th className="p-2 font-normal" key={label}>{label}</th>)}</tr></thead>
    <tbody>{rows.map(([label, row]) => <tr key={label} className="border-t border-border">
      <th className="p-2 font-normal">{label}</th><td>{row.trades}</td><td>{pct(row.win_rate)}</td><td>{formatMoney(row.average_win)}</td><td>{formatMoney(row.average_loss)}</td>
      <td>{formatMoney(row.expectancy)}</td><td>{row.no_losses ? "∞" : row.profit_factor?.toFixed(2) ?? "—"}</td><td>{row.average_r?.toFixed(2) ?? "—"}</td><td>{pct(row.average_return_on_buying_power ?? null)}</td><td>{pct(row.adherence)}</td>
    </tr>)}</tbody>
  </table></div>
}
function ForwardTestPanel({ definition, forward, runs, error }: { definition: Playbook; forward?: ForwardTest; runs: BacktestState[]; error: boolean }) {
  const version = String(definition.version)
  const current = forward?.versions?.[version]?.all
  const latest = runs.filter(run => run.status === "completed" && (run.playbooks ?? (run.playbook ? [run.playbook] : [])).some(item => item.id === definition.id && item.version === definition.version) && run.summary)
    .sort((a, b) => b.id.localeCompare(a.id))[0]
  const backtestStats = latest?.mode === "joint" ? latest.summary?.per_playbook?.[definition.id] : latest?.summary
  return <section className="mt-4 space-y-2 border-t border-border pt-3" aria-label={`Forward test for ${definition.name}`}>
    <h3 className="text-sm font-medium">Forward test · {forward?.running ? "Running" : "Stopped"}</h3>
    <p className="text-xs text-muted">{forward?.days_running?.toFixed(2) ?? "0"} elapsed market days · {forward?.entries ?? 0} entries · {forward?.time_stops ?? 0} time stops · {forward?.rejected_entries ?? 0} rejected entries</p>
    {forward?.report && <Stats rows={[["All forward trades", forward.report.all]]} />}
    <details className="text-xs"><summary>Forward-test windows ({forward?.windows.length ?? 0})</summary>
      {forward?.windows.map((window, index) => <p key={index} className="mt-2">v{window.version} · {timestampET(window.started)} → {window.ended ? timestampET(window.ended) : "Running"} · {window.actor}</p>)}
    </details>
    <p className="text-xs font-medium">Version {version} · Latest saved backtest{latest ? ` ${latest.id}` : ""}</p>
    {latest?.summary ? <div className="overflow-x-auto"><table className="w-full text-left text-xs tabular">
      <thead><tr>{["Source", "Trades", "Win rate", "Expectancy", "Return on BP"].map(label => <th className="p-2 font-normal" key={label}>{label}</th>)}</tr></thead>
      <tbody>{[["Forward", current], [latest.mode === "joint" ? "Joint backtest" : "Backtest", backtestStats]].map(([label, row]) => {
        const stats = row as typeof current | typeof latest.summary
        return <tr key={String(label)}><th className="p-2 font-normal">{String(label)}</th><td>{stats?.trades ?? 0}</td><td>{pct(stats?.win_rate ?? null)}</td><td>{formatMoney(stats?.expectancy ?? null)}</td><td>{pct(stats?.average_return_on_buying_power ?? null)}</td></tr>
      })}</tbody>
    </table></div> : <p className="text-xs text-muted">{error ? "Saved backtests unavailable." : "No completed saved backtest for this version."}</p>}
    <p className="text-xs text-muted">Simulated results. Forward trades use current markets; backtests use their saved days and plan. Auto follows new versions. Stopping Auto leaves positions and working orders for you to manage.</p>
  </section>
}
export function PlaybooksView() {
  const live = useLive()
  return <Playbooks key={live.accountScope} />
}
function Playbooks() {
  const api = useActionApi()
  const live = useLive()
  const query = usePlaybooks()
  const backtests = useQuery({ queryKey: ["forward-backtests"], queryFn: ({ signal }) => api.backtests(signal),
    enabled: live.source === "live" && live.trading?.enabled === true, refetchInterval: 10000, retry: false })
  const [confirmAuto, setConfirmAuto] = useState<Playbook | null>(null)
  const client = useQueryClient()
  const token = useWriteToken()
  const [selected, setSelected] = useState("")
  const [editing, setEditing] = useState<string | null>(null)
  const [error, setError] = useState<unknown>(null)
  const [busy, setBusy] = useState(false)
  if (!live.trading) return null
  const trading = live.trading
  const blocked = writeBlocked(trading, token)
  const mutate = async (work: () => Promise<unknown>, close = false) => {
    setBusy(true); setError(null)
    try { await work(); if (close) setEditing(null); await client.invalidateQueries({ queryKey: ["playbooks"] }) }
    catch (failure) { setError(failure) } finally { setBusy(false) }
  }
  if (query.error) return <TradingError error={query.error} />
  if (!query.data) return <Empty>{trading.enabled ? "Loading playbooks…" : trading.reason ?? "Paper trading is unavailable"}</Empty>
  const data = query.data
  const definitions = Object.values(data.definitions).filter((item) => !item.deleted).map((item) => item.versions.at(-1)!)
  const archived = Object.values(data.definitions).filter((item) => item.deleted).map((item) => item.versions.at(-1)!)
  const current = data.definitions[selected]?.versions.at(-1)
  const report = data.reports?.[selected]
  return <div className="space-y-4">
    <PageHeader title="Playbooks" subtitle="Written setups, staged orders and adherence for this account.">
      <button className="trade-button" disabled={!!blocked || busy} onClick={() => { setEditing(JSON.stringify(newPlaybook, null, 2)); setError(null) }}>New playbook</button>
    </PageHeader>
    <WriteAccess trading={trading} />
    <p className="text-xs text-muted">Orders and fills are simulated. Auto sends orders automatically on paper, including live accounts. All times are New York time.</p>
    {error != null && <TradingError error={error} />}
    <div className="grid gap-3 lg:grid-cols-2">{definitions.map((definition) => <Panel key={definition.id} title={`${definition.name} · v${definition.version}`}>
      <p className="text-sm">{definition.description}</p><p className="mt-2 text-xs text-muted">{definition.underlyings.join(", ")} · {definition.window.start}–{definition.window.end} ET · {definition.structure.template.kind}</p>
      <div className="mt-3 flex flex-wrap items-center gap-2">
        <label className="text-xs">Mode <select className="trade-input !w-auto" aria-label={`Mode for ${definition.name}`} value={data.modes[definition.id] ?? "off"} disabled={!!blocked || busy}
          onChange={(event) => {
            const mode = event.target.value as "off" | "stage" | "auto"
            if (mode === "auto" && live.source === "live") { setError(null); setConfirmAuto(definition) }
            else void mutate(() => api.playbookMode(definition.id, mode, trading.write))
          }}>
          <option value="off">Off</option><option value="stage">Stage</option><option value="auto" disabled={!data.auto_allowed}>Auto</option>
        </select></label>
        <button className="trade-button" onClick={() => setSelected(definition.id)}>Stats and versions</button>
        <button className="trade-button" disabled={!!blocked || busy} onClick={() => { setEditing(JSON.stringify(definition, null, 2)); setError(null) }}>Edit {definition.name}</button>
        <button className="trade-button" disabled={!!blocked || busy} onClick={() => void mutate(() => api.deletePlaybook(definition.id, definition.version!, trading.write))}>Archive {definition.name}</button>
      </div>
      {Object.entries(data.reasons).filter(([key]) => key.startsWith(`${definition.id}:`)).map(([key, reason]) => <p key={key} className="mt-2 text-xs text-muted">{key.split(":")[1]}: {reason}</p>)}
      {live.source === "live" && <ForwardTestPanel definition={definition} forward={data.forward_tests?.[definition.id]} runs={backtests.data?.runs ?? []} error={!!backtests.error} />}
    </Panel>)}</div>
    {!definitions.length && <Empty>No playbooks yet. Create a setup to stage it on this account.</Empty>}
    {!!archived.length && <details className="text-xs"><summary>Archived playbooks</summary>
      <div className="mt-2 flex flex-wrap gap-2">{archived.map((definition) => <button className="trade-button" key={definition.id} onClick={() => setSelected(definition.id)}>{definition.name} · archived stats and versions</button>)}</div>
    </details>}
    {confirmAuto && <Dialog title="Enable automatic paper trading" onClose={() => { if (!busy) setConfirmAuto(null) }}>
      <p className="text-sm">Enable Auto for {confirmAuto.name} on {live.accounts.find(account => account.id === live.account)?.name ?? live.account} ({live.account})? Orders will be sent automatically on paper, including entries and time-stop closes. Normal risk and account checks apply.</p>
      <p className="text-xs text-muted">Auto resumes after a server restart and follows newly saved versions. Turning it off leaves existing positions and orders for you to manage.</p>
      {error != null && <TradingError error={error} />}
      <div className="flex gap-2"><button className="trade-button" disabled={!!blocked || busy} onClick={() => void mutate(async () => { await api.playbookMode(confirmAuto.id, "auto", trading.write); setConfirmAuto(null) })}>Enable Auto</button>
        <button className="trade-button" disabled={busy} onClick={() => setConfirmAuto(null)}>Cancel</button></div>
    </Dialog>}
    {editing != null && <Panel title="Edit playbook definition">
      <p className="mb-3 text-xs text-muted">This JSON contains the complete rules. Saving an edit creates a new version; old versions remain readable. Use fixed units or floor_share (0–1). Take-profit percent is the closing premium as a percent of the entry credit or debit. Price references use point offsets; IV rank uses fractions. Missing condition inputs prevent entry.</p>
      <label className="trade-label">Definition JSON<textarea aria-label="Playbook definition" className="trade-input min-h-96 font-mono text-xs" value={editing} onChange={(event) => setEditing(event.target.value)} /></label>
      <div className="mt-3 flex gap-2"><button className="trade-button" disabled={!!blocked || busy} onClick={() => void mutate(() => api.savePlaybook(JSON.parse(editing) as Playbook, trading.write), true)}>Save version</button>
        <button className="trade-button" onClick={() => setEditing(null)}>Cancel edit</button></div>
    </Panel>}
    <StagedOrders />
    {current && <>
      {live.source === "live" && data.definitions[selected]?.deleted && <Panel title={current.name}><ForwardTestPanel definition={current} forward={data.forward_tests?.[selected]} runs={backtests.data?.runs ?? []} error={!!backtests.error} /></Panel>}
      <Panel title={`${current.name} · this attempt`}>
        {report && <Stats rows={[["All", report.all], ["Followed rules", report.followed], ["Deviated", report.deviated]]} />}
        <p className="my-3 text-xs text-muted">Unfinished time-stop rules are pending. Missing sizing evidence fails the size check. Trades count once per opening strategy.</p>
        {report?.trades.map((trade) => <details key={trade.order} className="border-t border-border py-2 text-xs">
          <summary>Order {trade.order} · v{trade.version} · {trade.closed ? formatMoney(trade.net) : "Open"} · {trade.followed ? "Rules followed so far" : "Deviation"}</summary>
          <dl className="mt-2 space-y-1">{Object.entries(trade.rules).map(([rule, pass]) => <div className="flex gap-3" key={rule}><dt>{rule.replaceAll("_", " ")}</dt><dd>{pass == null ? "Pending" : pass ? "Pass" : "Fail"}</dd></div>)}</dl>
        </details>)}
        <details className="mt-3 text-xs"><summary>Saved versions</summary>{data.definitions[selected]?.versions.map((definition) => <details key={definition.version} className="mt-2"><summary>Version {definition.version}</summary><pre className="overflow-x-auto p-2">{JSON.stringify(definition, null, 2)}</pre></details>)}</details>
      </Panel>
      <PassOddsCard playbook={selected} />
    </>}
  </div>
}
