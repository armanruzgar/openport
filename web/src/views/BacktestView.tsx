import { useActionApi } from "../api/action-client"
import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"

import type { BacktestComparison, BacktestDayInput, BacktestIdentity, BacktestPlaybookBreakdown, BacktestReport, BacktestResult, BacktestStart } from "../api/backtest-types"
import { useLive } from "../api/live"
import { Dialog } from "../components/Dialog"
import { LineChart } from "../charts/LineChart"
import { BarChart } from "../charts/BarChart"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Empty, Meter, PageHeader, Panel, Stat } from "../components/ui"
import { formatMoney, signedMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"

const label = "Simulated trading on recorded or generated days. Not a prediction. Not investment advice."
const percent = (value: number | null) => value == null ? "—" : `${(value * 100).toFixed(1)}%`
export function backtestHistogram(values: string[]) {
  const numbers = values.map(Number).filter(Number.isFinite)
  if (!numbers.length) return []
  const low = Math.min(...numbers), high = Math.max(...numbers)
  if (low === high) return [{ x: low, value: numbers.length }]
  const count = Math.min(10, numbers.length), width = (high - low) / count
  const bars = Array.from({ length: count }, (_, index) => ({ x: low + (index + .5) * width, value: 0 }))
  for (const number of numbers) bars[Math.min(count - 1, Math.floor((number - low) / width))]!.value++
  return bars
}
function playbookNames(value: BacktestIdentity) {
  return (value.playbooks ?? (value.playbook ? [value.playbook] : [])).map((item) => `${item.name ?? item.id}@${item.version ?? "latest"}`).join(", ") || "—"
}
function PlaybookBreakdown({ rows }: { rows?: Record<string, BacktestPlaybookBreakdown> }) {
  if (!rows) return null
  return <div className="overflow-x-auto"><table aria-label="Per-playbook breakdown" className="w-full text-left text-xs tabular">
    <thead><tr>{["Playbook", "Closed / open trades", "Expectancy", "Win rate", "Realised P&L", "Marked P&L", "Entry reasons / rules"].map((title) => <th className="p-2" key={title}>{title}</th>)}</tr></thead>
    <tbody>{Object.entries(rows).map(([id, row]) => <tr className="border-t border-border" key={id}>
      <td className="p-2">{id}</td><td className="p-2">{row.trades} / {row.open_trades}</td><td className="p-2">{signedMoney(row.expectancy)}</td>
      <td className="p-2">{percent(row.win_rate)}</td><td className="p-2">{signedMoney(row.realised_pnl)}</td><td className="p-2">{signedMoney(row.marked_pnl)}</td>
      <td className="p-2">{Object.entries(row.entry_reasons).map(([key, reason]) => <p key={key}>{key}: {reason}</p>)}
        {row.rule_trips.map((trip, index) => <p key={index}>{trip.time} · {trip.playbook ?? "Account"} · {trip.type} · {JSON.stringify(trip.detail)}</p>)}</td>
    </tr>)}</tbody>
  </table></div>
}
function Details({ result }: { result: BacktestResult }) {
  return <details className="text-xs"><summary>Trades, fills and rules</summary>
    <p className="my-2">Observed {result.started} to {result.ended}.</p>
    <PlaybookBreakdown rows={result.per_playbook} />
    <p className="my-2">Journal: <code>{result.journal}</code></p>
    {!result.valuation_complete && <p className="text-warn">Incomplete marks. Last-mark P&amp;L: {signedMoney(result.last_mark_pnl)}.</p>}
    {result.open_positions > 0 && <p>{result.open_positions} positions remain open at the final observation.</p>}
    {result.quality_flags.length > 0 && <p className="text-warn">{result.quality_flags.join(", ")}</p>}
    {result.rule_trips.map((trip, index) => <p key={index}>{trip.time} · {trip.type} · {JSON.stringify(trip.detail)}</p>)}
    {Object.entries(result.entry_reasons).map(([key, reason]) => <p key={key}>{key}: {reason}</p>)}
    {result.trades.map((trade) => <p key={trade.order}>Order {trade.order} · {trade.closed ? signedMoney(trade.net) : "Open trade"} · {Object.entries(trade.rules).map(([rule, pass]) => `${rule}: ${pass == null ? "pending" : pass ? "pass" : "fail"}`).join(" · ")}</p>)}
    {result.stock_trades.map((trade, index) => <p key={`stock-${index}`}>{trade.symbol} shares · {trade.closed ? signedMoney(trade.net) : "Open share trade"}</p>)}
    {result.stock_fills.map((fill) => <p key={`stock-fill-${fill.id}`}>{fill.time} · {fill.shares} {fill.symbol} shares at {formatMoney(fill.price)}</p>)}
    {result.fills.map((fill) => <p key={fill.id}>{fill.time} · {fill.side} {fill.quantity} {fill.symbol} at {formatMoney(fill.price)} · fee {formatMoney(fill.fee)}</p>)}
  </details>
}
export function BacktestReportView({ report }: { report: BacktestReport }) {
  const summary = report.summary
  return <div className="space-y-4">
    <p className="text-sm text-muted">{report.label} {report.status !== "completed" && "Partial report; unfinished days and attempts are excluded."}</p>
    {report.mode === "joint" && <Panel title="Joint account backtest"><p className="text-sm">{playbookNames(report)}</p>
      <p className="my-2 text-xs text-muted">One account shares cash, buying power and plan rules. Entries run in ascending playbook ID order. Attribution covers option trades; share delivery and dividends remain in account results.</p>
      <PlaybookBreakdown rows={summary.per_playbook} /></Panel>}
    {report.errors.map((error, index) => <p role="alert" key={index}>{error.day == null ? "Attempt" : `Day ${error.day + 1}`}: {error.message}</p>)}
    <div className="grid gap-3 sm:grid-cols-5">
      <Stat label="Attempt pass rate" value={percent(summary.pass_rate)} hint="Passed / decided attempts. Open attempts are excluded." />
      <Stat label="Attempts" value={`${summary.passed} passed · ${summary.failed} failed · ${summary.open} open`} />
      <Stat label="Trade expectancy" value={signedMoney(summary.expectancy)} hint="Mean net result per closed strategy in independent daily runs." />
      <Stat label="Trade win rate" value={percent(summary.win_rate)} hint={`${summary.trades} closed strategies; wins over decided ones, breakevens left out.`} />
      <Stat label="Return on buying power" value={percent(summary.average_return_on_buying_power ?? null)}
        hint="Mean of each closed strategy's net over the buying power its entry needed: a credit spread's width less its credit, not its premium." />
    </div>
    <p className="text-xs text-muted">{summary.completed_days} independent days, {summary.marked_days} with complete final marks. Day win rate: {percent(summary.day_win_rate)}. Attempts replay the ordered days on one account until a pass or failure, then start fresh on the next day.</p>
    <div className="grid gap-4 lg:grid-cols-2">{([
      ["Daily P&L distribution", summary.daily_pnl], ["Daily maximum drawdown distribution", summary.daily_drawdown],
    ] as const).map(([title, distribution]) => <Panel key={title} title={title}>
      <p className="text-xs text-muted">Min {formatMoney(distribution.min)} · Q1 {formatMoney(distribution.p25)} · Median {formatMoney(distribution.median)} · Q3 {formatMoney(distribution.p75)} · Max {formatMoney(distribution.max)}</p>
      <BarChart bars={backtestHistogram(distribution.values)} formatX={(value) => formatMoney(String(value), 0)} formatY={(value) => String(value)} height={220} />
      <p className="text-xs text-muted">Simulated results · horizontal: dollars; vertical: days</p>
    </Panel>)}</div>
    <Panel title="Worst days"><div className="flex flex-wrap gap-4 text-sm">{summary.worst_days.map((index) => {
      const day = report.days[index]
      return day && <button className="text-accent" key={index} onClick={() => document.getElementById(`backtest-day-${index}`)?.scrollIntoView({ block: "center" })}>{day.date} · {signedMoney(day.pnl)}</button>
    })}</div></Panel>
    <Panel title="Independent daily results"><div className="overflow-x-auto"><table className="w-full text-left text-xs tabular">
      <thead><tr>{["Day / source", "P&L", "Max drawdown", "Minimum floor room", "Adherence", "Fills / trips", "Review"].map((title) => <th className="p-2" key={title}>{title}</th>)}</tr></thead>
      <tbody>{report.days.map((day, index) => day && <tr id={`backtest-day-${index}`} className="border-t border-border" key={index}>
        <td className="p-2">{day.date}<div className="text-muted">{day.input.kind === "scenario" ? `Generated · ${day.input.id} · seed ${day.input.seed}` : `Recorded · ${day.input.name}`}</div></td>
        <td className="p-2">{signedMoney(day.pnl)}</td><td className="p-2">{formatMoney(day.max_drawdown)}</td><td className="p-2">{formatMoney(day.min_floor_distance)}</td>
        <td className="p-2">{percent(day.adherence)}</td><td className="p-2">{day.fills.length + day.stock_fills.length} / {day.rule_trips.length}</td><td className="p-2"><Details result={day} /></td>
      </tr>)}</tbody>
    </table></div></Panel>
    <Panel title="Evaluation attempts">{report.attempts.map((attempt, index) => <div className="space-y-2 border-b border-border py-2 text-sm" key={index}>
      <p>Attempt {index + 1} · {attempt.outcome} · days {attempt.first_day + 1}–{attempt.last_day + 1} · {signedMoney(attempt.pnl)}</p>
      {attempt.day_rows && <details><summary>Daily evaluation ({attempt.day_rows.length} days)</summary><div className="overflow-x-auto"><table className="w-full text-left text-xs tabular">
        <thead><tr>{["Day", "Balance start → end", "Equity start → end", "Day P&L", "Floor / room", "Target / progress", "Peak", "Opened / closed", "Status / rules"].map((title) => <th className="p-2" key={title}>{title}</th>)}</tr></thead>
        <tbody>{attempt.day_rows.map((day) => <tr className="border-t border-border" key={day.day_index}>
          <td className="p-2">{day.day_index + 1} · {day.date}<div className="text-muted">As of {day.ended}</div></td>
          <td className="p-2">{formatMoney(day.start_balance)} → {formatMoney(day.end_balance)}</td>
          <td className="p-2">{formatMoney(day.start_equity)} → {formatMoney(day.end_equity)}{!day.valuation_complete && " · Incomplete marks"}</td>
          <td className="p-2">{signedMoney(day.pnl)}</td><td className="p-2">{formatMoney(day.floor)} / {formatMoney(day.floor_distance)}</td>
          <td className="p-2">{formatMoney(day.target)} / {signedMoney(day.target_progress)}</td><td className="p-2">{formatMoney(day.peak)}</td>
          <td className="p-2">{day.trades_opened} / {day.trades_closed}</td><td className="p-2">{day.outcome}{day.day_lock !== "NONE" && ` · ${day.day_lock}`}
            {day.decision && <p>{day.decision_code} · {day.decision}</p>}{day.rule_trips.map((trip, tripIndex) => <p key={tripIndex}>{trip.time} · {trip.type} · {JSON.stringify(trip.detail)}</p>)}</td>
        </tr>)}</tbody>
      </table></div></details>}
      {attempt.decision && <p className="text-xs text-muted">{attempt.decision}</p>}<Details result={attempt} />
    </div>)}</Panel>
  </div>
}
export function BacktestCompareView({ comparison }: { comparison: BacktestComparison }) {
  const { runs, combined } = comparison
  return <Panel title="Compare saved runs">
    {comparison.apples_to_oranges && <p className="mb-3 text-warn">Apples-to-oranges comparison: {[
      comparison.different_inputs && "different inputs", comparison.different_plans && "different plans", comparison.incomplete_inputs && "incomplete input identities",
    ].filter(Boolean).join("; ")}.</p>}
    <div className="overflow-x-auto"><table className="w-full text-left text-xs tabular">
      <thead><tr><th className="p-2">Metric / date</th>{runs.map((run) => <th className="p-2" key={run.id}>{run.id} · {playbookNames(run)}</th>)}</tr></thead>
      <tbody>
        <tr><th className="p-2">Plan / status</th>{runs.map((run) => <td className="p-2" key={run.id}>{run.plan.rules?.plan ?? "Custom"} · {run.status}<details><summary>Plan and input identity</summary><pre className="max-w-lg whitespace-pre-wrap">{JSON.stringify({ plan: run.plan, inputs: run.input_set }, null, 2)}</pre></details></td>)}</tr>
        {([['Mean daily P&L', (run) => signedMoney(run.summary.daily_pnl.mean)], ['Trade expectancy', (run) => signedMoney(run.summary.expectancy)],
          ['Day win rate', (run) => percent(run.summary.day_win_rate)], ['Pass rate', (run) => percent(run.summary.pass_rate)],
          ['Completed days', (run) => String(run.summary.completed_days)]] as [string, (run: BacktestComparison["runs"][number]) => string][]).map(([title, value]) =>
          <tr key={title}><th className="p-2">{title}</th>{runs.map((run) => <td className="p-2" key={run.id}>{value(run)}</td>)}</tr>)}
        {comparison.daily.map((day) => <tr className="border-t border-border" key={day.date}><th className="p-2">{day.date}</th>{runs.map((run) => <td className="p-2" key={run.id}>{signedMoney(day.pnl[run.id] ?? null)}</td>)}</tr>)}
      </tbody>
    </table></div>
    <h3 className="mt-4 text-sm font-medium">Combined independent daily P&amp;L</h3><p className="my-2 text-xs text-muted">{combined.label} Missing dates contribute nothing; unmarked or unfinished supplied days leave a gap in the curve.</p>
    <LineChart series={[{ id: "combined", label: "Combined cumulative P&L", color: "var(--accent)", points: [
      { x: -1, y: 0 }, ...combined.curve.map((day, index) => ({ x: index, y: day.cumulative == null ? null : Number(day.cumulative) })),
    ] }]} formatX={(value) => value < 0 ? "Start" : combined.curve[Math.round(value)]?.date ?? ""} formatY={(value) => formatMoney(String(value), 0)} height={240} />
    <p className="text-xs">Maximum drawdown {formatMoney(combined.max_drawdown)} · Day win rate {percent(combined.day_win_rate)} · Mean {signedMoney(combined.daily_pnl.mean)} · Median {signedMoney(combined.daily_pnl.median)}</p>
    <BarChart bars={backtestHistogram(combined.daily_pnl.values)} formatX={(value) => formatMoney(String(value), 0)} formatY={String} height={180} />
    <p className="text-xs">Worst days: {combined.worst_days.map((day) => `${day.date}: ${signedMoney(day.pnl)}`).join(" · ")}</p>
  </Panel>
}
export function BacktestView() {
  const api = useActionApi()
  const live = useLive(), token = useWriteToken()
  const definitions = useQuery({ queryKey: ["backtest-playbooks", live.accountScope], queryFn: ({ signal }) => api.backtestPlaybooks(signal) })
  const plans = useQuery({ queryKey: ["backtest-plans"], queryFn: ({ signal }) => api.backtestPlans(signal), staleTime: Infinity })
  const sources = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal) })
  const accountTrading = live.status?.trading ?? live.trading
  const trading = accountTrading && { ...accountTrading, write: sources.data?.write ?? accountTrading.write }
  const blocked = trading ? writeBlocked(trading, token) : "Paper trading is unavailable."
  const listing = useQuery({ queryKey: ["backtests"], queryFn: ({ signal }) => api.backtests(signal), refetchInterval: 1000 })
  const [selected, setSelected] = useState("")
  const [compareIds, setCompareIds] = useState<string[]>([]), [deleting, setDeleting] = useState("")
  const comparison = useQuery({ queryKey: ["backtest-compare", compareIds], queryFn: ({ signal }) => api.compareBacktests(compareIds, signal), enabled: compareIds.length >= 2 })
  const activeId = selected || listing.data?.active || listing.data?.runs[0]?.id || ""
  const job = useQuery({ queryKey: ["backtest", activeId], queryFn: ({ signal }) => api.backtest(activeId, signal), enabled: !!activeId,
    refetchInterval: (query) => ["running", "cancelling"].includes(query.state.data?.status ?? "running") ? 1000 : false })
  const versions = Object.values(definitions.data?.definitions ?? {}).flatMap((record) => record.versions.map((definition) => ({ ...definition, archived: record.deleted })))
  const [playbook, setPlaybook] = useState(""), [additional, setAdditional] = useState<string[]>([]), [plan, setPlan] = useState("eod-50k")
  const [kind, setKind] = useState("scenarios"), [count, setCount] = useState("10"), [seed, setSeed] = useState("1"), [scenario, setScenario] = useState("")
  const [files, setFiles] = useState<string[]>([]), [manifest, setManifest] = useState("[]")
  const [pending, setPending] = useState(false), [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const chosen = playbook || (versions[0] ? `${versions[0].id}@${versions[0].version}` : "")
  const chosenIds = [chosen, ...additional].map((value) => value.split("@")[0])
  const badPlaybooks = additional.some((value) => !value) || new Set(chosenIds).size !== chosenIds.length
  const mode = trading?.write ?? "disabled"
  const badSeed = !/^\d{1,20}$/.test(seed) || BigInt(seed || "0") > 18446744073709551615n
  const badCount = !/^\d+$/.test(count) || Number(count) < 1 || Number(count) > 252
  async function mutate(action: () => Promise<void>, refreshJob = true) {
    if (busy.current) return
    busy.current = true; setPending(true); setError(undefined)
    try { await action(); await listing.refetch(); if (activeId && refreshJob) await job.refetch() }
    catch (failure) { setError(failure) } finally { busy.current = false; setPending(false) }
  }
  async function start() {
    let days: BacktestDayInput[] = []
    if (kind === "recordings") days = [...files].sort((left, right) => {
      const rows = sources.data?.recordings ?? []
      return (rows.find((row) => row.file === left)?.started ?? "").localeCompare(rows.find((row) => row.file === right)?.started ?? "") || left.localeCompare(right)
    }).map((file) => ({ file }))
    if (kind === "manifest") {
      const parsed: unknown = JSON.parse(manifest)
      if (!Array.isArray(parsed) || !parsed.length) throw new Error("The day manifest must be a nonempty JSON array.")
      if (parsed.some((entry) => entry && typeof entry === "object" && "seed" in entry && typeof entry.seed !== "string"))
        throw new Error("Write scenario seeds as quoted decimal strings to preserve all 64 bits.")
      days = parsed as BacktestDayInput[]
    }
    const selection = additional.length ? { playbooks: [chosen, ...additional] } : { playbook: chosen }
    const request: BacktestStart = kind === "scenarios" ? { ...selection, plan, scenarios: Number(count), seed, ...(scenario ? { scenario } : {}) }
      : { ...selection, plan, days }
    const started = await api.startBacktest(request, mode)
    setSelected(started.id)
  }
  return <div className="space-y-4">
    <PageHeader title="Backtest" subtitle={label}>{trading && <WriteAccess trading={trading} />}</PageHeader>
    <TradingError error={error ?? comparison.error ?? listing.error ?? job.error ?? definitions.error ?? sources.error ?? plans.error} />
    <Panel title="Run playbooks">
      <div className="grid gap-3 sm:grid-cols-3">
        <label className="trade-label">Playbook version<select className="trade-input" value={chosen} onChange={(event) => setPlaybook(event.target.value)}>{versions.map((definition) => <option key={`${definition.id}@${definition.version}`} value={`${definition.id}@${definition.version}`}>{definition.name} · v{definition.version}{definition.archived ? " · archived" : ""}</option>)}</select></label>
        <label className="trade-label">Evaluation plan<select className="trade-input" value={plan} onChange={(event) => setPlan(event.target.value)}>{plans.data?.plans.filter((item) => !item.unlocked_by && item.rules.phase !== "funded" && item.rules.profit_target != null && Number(item.rules.profit_target) > 0).map((item) => <option key={item.id} value={item.id}>{item.name}</option>)}</select></label>
        <label className="trade-label">Days<select className="trade-input" value={kind} onChange={(event) => setKind(event.target.value)}><option value="scenarios">Generated scenarios</option><option value="recordings">Recordings, including imports</option><option value="manifest">Mixed day manifest</option></select></label>
      </div>
      <div className="mt-3 space-y-2">{additional.map((value, index) => <div className="flex items-end gap-2" key={index}>
        <label className="trade-label">{`Playbook version ${index + 2}`}<select className="trade-input" value={value} onChange={(event) => setAdditional(additional.map((item, i) => i === index ? event.target.value : item))}>
          <option value="">Choose a playbook</option>{versions.map((definition) => <option key={`${definition.id}@${definition.version}`} value={`${definition.id}@${definition.version}`} disabled={chosenIds.some((id, i) => i !== index + 1 && id === definition.id)}>{definition.name} · v{definition.version}</option>)}
        </select></label><button className="trade-button" onClick={() => setAdditional(additional.filter((_, i) => i !== index))}>Remove playbook {index + 2}</button>
      </div>)}<button className="trade-button" disabled={additional.length >= 7 || new Set(versions.map((item) => item.id)).size <= additional.length + 1} onClick={() => setAdditional([...additional, ""])}>Add playbook</button></div>
      {!!additional.length && <p className="my-2 text-xs text-muted">Joint mode: 2–8 distinct playbooks share one account. Evaluation order is ascending playbook ID, regardless of selection order.</p>}
      {badPlaybooks && <p className="text-xs text-warn">Choose a different playbook ID in every row.</p>}
      {kind === "scenarios" && <div className="mt-3 grid gap-3 sm:grid-cols-3">
        <label className="trade-label">Scenario<select className="trade-input" value={scenario} onChange={(event) => setScenario(event.target.value)}><option value="">Cycle regular scenarios</option>{sources.data?.demos?.map((item) => <option key={item.id} value={item.id}>{item.title}</option>)}</select></label>
        <label className="trade-label">Day count<input className="trade-input" type="number" min="1" max="252" value={count} onChange={(event) => setCount(event.target.value)} /></label>
        <label className="trade-label">Starting seed<input className="trade-input" inputMode="numeric" value={seed} onChange={(event) => setSeed(event.target.value)} /></label>
      </div>}
      {kind === "recordings" && <div className="my-3 space-y-2">{sources.data?.recordings.map((recording) => <label className="flex gap-2 text-sm" key={recording.file}>
        <input type="checkbox" disabled={!!recording.error} checked={files.includes(recording.file)} onChange={(event) => setFiles(event.target.checked ? [...files, recording.file] : files.filter((file) => file !== recording.file))} />{recording.file}{recording.error && ` · ${recording.error}`}
      </label>)}{!sources.data?.recordings.length && <Empty>No recordings available.</Empty>}</div>}
      {kind === "manifest" && <label className="trade-label mt-3">Day manifest JSON<textarea className="trade-input min-h-32 font-mono" value={manifest} onChange={(event) => setManifest(event.target.value)} placeholder={'[{"scenario":"reversal","date":"2026-09-16","seed":"1"},{"file":"next-day.oprec"}]'} /></label>}
      <p className="my-3 text-xs text-muted">Choose distinct, increasing trading dates. Generated runs advance one trading day and one seed per entry. Missing condition inputs prevent entry. Maximum 252 days. Journals are retained for verification.</p>
      {blocked && <p className="mb-2 text-xs text-warn">{blocked}</p>}
      <button className="trade-button" disabled={!!blocked || pending || !!listing.data?.active || !chosen || badPlaybooks || (kind === "scenarios" && (badSeed || badCount)) || (kind === "recordings" && !files.length)} onClick={() => void mutate(start)}>Start backtest</button>
    </Panel>
    {!!listing.data?.runs.length && <Panel title="Saved runs"><p className="mb-2 text-xs text-muted">Select 2–8 finished reports to compare. Keep exempts a run from automatic retention.</p><div className="overflow-x-auto"><table className="w-full text-left text-sm">
      <thead><tr>{["Compare", "Run", "Playbook", "Disk size", "Retention", "Delete"].map((title) => <th className="p-2" key={title}>{title}</th>)}</tr></thead>
      <tbody>{listing.data.runs.map((run) => <tr className="border-t border-border" key={run.id}>
        <td className="p-2"><input type="checkbox" aria-label={`Compare run ${run.id}`} checked={compareIds.includes(run.id)} disabled={!run.summary || ["running", "cancelling"].includes(run.status) || (compareIds.length >= 8 && !compareIds.includes(run.id))} onChange={(event) => setCompareIds(event.target.checked ? [...compareIds, run.id] : compareIds.filter((id) => id !== run.id))} /></td>
        <td className="p-2"><button className="text-accent" onClick={() => setSelected(run.id)}>{run.id} · {run.status}</button></td>
        <td className="p-2">{run.mode === "joint" && "Joint · "}{playbookNames(run)}</td><td className="p-2">{run.bytes == null ? "—" : `${(run.bytes / 1048576).toFixed(2)} MiB`}</td>
        <td className="p-2"><label><input type="checkbox" aria-label={`Keep run ${run.id}`} checked={run.keep ?? false} disabled={!!blocked || pending} onChange={(event) => { const keep = event.target.checked; void mutate(async () => { await api.keepBacktest(run.id, keep, mode) }) }} /> Keep</label></td>
        <td className="p-2"><button className="trade-button" aria-label={`Delete run ${run.id}`} disabled={!!blocked || pending || ["running", "cancelling"].includes(run.status)} onClick={() => setDeleting(run.id)}>Delete</button></td>
      </tr>)}</tbody>
    </table></div></Panel>}
    {comparison.data && compareIds.length >= 2 && <BacktestCompareView comparison={comparison.data} />}
    {deleting && <Dialog title="Delete saved backtest?" onClose={() => setDeleting("")}>
      <p className="text-sm">Permanently delete run {deleting}, including its report and journals? Kept runs can also be deleted. This cannot be undone.</p>
      <TradingError error={error} />
      <button className="trade-button" disabled={!!blocked || pending} onClick={() => void mutate(async () => {
        await api.deleteBacktest(deleting, mode); setCompareIds(compareIds.filter((id) => id !== deleting));
        if (activeId === deleting) setSelected(""); setDeleting("")
      }, false)}>Delete saved run</button>
      <button className="trade-button" disabled={pending} onClick={() => setDeleting("")}>Cancel</button>
    </Dialog>}
    {!!listing.data?.runs.length && <label className="trade-label">Saved report<select className="trade-input" value={activeId} onChange={(event) => setSelected(event.target.value)}>{listing.data.runs.map((run) => <option key={run.id} value={run.id}>{run.id} · {run.status}</option>)}</select></label>}
    {job.data && <Panel title={`Run ${job.data.id} · ${job.data.status}`}>
      <p className="mb-2 text-sm" role="status">{job.data.phase} · {job.data.completed} / {job.data.total} day runs</p>
      <Meter value={job.data.total ? job.data.completed / job.data.total : 0} label="Backtest progress" />
      {job.data.directory && <p className="my-2 text-xs text-muted">Reports and journals: <code>{job.data.directory}</code></p>}
      {job.data.error && <p role="alert">{job.data.error}</p>}
      {["running", "cancelling"].includes(job.data.status) && <button className="trade-button mt-3" disabled={!!blocked || pending || job.data.status === "cancelling"} onClick={() => void mutate(async () => { await api.cancelBacktest(job.data!.id, mode) })}>Cancel backtest</button>}
    </Panel>}
    {job.data?.report && <BacktestReportView report={job.data.report} />}
  </div>
}
