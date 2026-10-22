import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { Account, Plan, RulePack, RulePackValidation, RuleReplaySet, RulesCompareResponse } from "../api/trading-types"
import { decisionLabel } from "../lib/plan-rules"
import { packFile, packPlans, useRulePacks, withPack } from "../lib/rule-packs"
import { formatMoney, ruleMoney } from "../lib/trading"
import { TradingError } from "./TradingControls"
import { Badge, Panel } from "./ui"

const outcomeMark: Record<string, { mark: string; label: string; className: string }> = {
  active: { mark: "·", label: "Active", className: "text-muted" },
  locked: { mark: "L", label: "Day locked", className: "bg-warn/20 text-warn" },
  passed: { mark: "P", label: "Passed", className: "bg-gex-positive/20 text-gex-positive" },
  failed: { mark: "F", label: "Failed", className: "bg-gex-negative/20 text-gex-negative" },
  after: { mark: "", label: "Decided earlier", className: "text-faint" },
}

/** A rule set's verdict in a few words. */
export function replayVerdict(set: RuleReplaySet): string {
  if (set.status === "active") return set.locked_days.length ? `Active · ${set.locked_days.length} locked ${set.locked_days.length === 1 ? "day" : "days"}` : "Active"
  return `${set.status === "passed" ? "Passed" : "Failed"} ${set.decided_day ?? ""}${set.code ? ` · ${decisionLabel(set.code) ?? set.code}` : ""}`
}

/** Replays the attempt's recorded days against other rule sets, side by side: where each would pass, fail or lock. */
export function RulesComparePanel({ plans }: { plans: Plan[] }) {
  const live = useLive()
  const [packs] = useRulePacks()
  const imported = packPlans(packs)
  const choices = [...plans, ...imported]
  const [selected, setSelected] = useState<string[]>([])
  const [result, setResult] = useState<RulesCompareResponse>()
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  if (live.source !== "live") return <Panel title="Compare rules"><p className="text-sm text-muted">Rule comparisons replay a live account's recorded attempt; switch to the live market to compare.</p></Panel>
  const toggle = (id: string) => setSelected((current) => current.includes(id) ? current.filter((v) => v !== id) : [...current, id].slice(0, 7))
  async function compare() {
    setPending(true)
    setError(undefined)
    try {
      const custom = imported.filter((p) => selected.includes(p.id))
      setResult(await api.rulesCompare({
        plans: selected.filter((id) => plans.some((p) => p.id === id)),
        rules: custom.map((p) => ({ name: p.name, initial_cash: p.initial_cash, rules: { ...p.rules, plan: p.name, plan_id: null } })),
      }))
    } catch (failure) { setError(failure) } finally { setPending(false) }
  }
  return <Panel title="Compare rules">
    <p className="text-sm text-muted">Replays this attempt's recorded days (each day's open, recorded low and high in time order, and close) against other plans,
      as the same dollar P&amp;L from each plan's own starting balance. Trades are not re-executed, so rules on orders and positions are listed as not replayed.</p>
    <fieldset className="mt-2 flex flex-wrap gap-x-4 gap-y-1 text-xs" aria-label="Plans to compare">
      {choices.map((p) => <label key={p.id} className="flex items-center gap-1">
        <input type="checkbox" checked={selected.includes(p.id)} onChange={() => toggle(p.id)} disabled={!selected.includes(p.id) && selected.length >= 7} />
        {p.name}{p.id.startsWith("pack:") && <Badge>imported</Badge>}
      </label>)}
    </fieldset>
    <button type="button" className="trade-button mt-2" disabled={pending} onClick={() => void compare()}>{pending ? "Replaying…" : "Compare with my rules"}</button>
    <TradingError error={error} />
    {result && <RulesCompareResult result={result} />}
  </Panel>
}

export function RulesCompareResult({ result }: { result: RulesCompareResponse }) {
  return <div className="mt-3 space-y-3">
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Rule comparison">
      <table className="w-full text-left text-xs whitespace-nowrap">
        <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
          {["Rules", "Start", "Outcome", "Closest to floor", "Not replayed"].map((h) => <th key={h} className="px-2 py-1 font-normal">{h}</th>)}
        </tr></thead>
        <tbody>{result.sets.map((set, i) => <tr key={`${set.name}-${i}`} className="border-t border-border/40">
          <td className="px-2 py-1 font-medium">{set.name}{i === 0 && <span className="ml-1 text-faint">(yours)</span>}</td>
          <td className="px-2 py-1 tabular">{ruleMoney(set.initial_cash)}</td>
          <td className="px-2 py-1" title={set.message ?? undefined}>{replayVerdict(set)}</td>
          <td className="px-2 py-1 tabular">{set.closest_floor ? formatMoney(set.closest_floor) : "—"}</td>
          <td className="px-2 py-1 text-muted">{set.not_replayed.length ? set.not_replayed.join(", ").replaceAll("_", " ") : "—"}</td>
        </tr>)}</tbody>
      </table>
    </div>
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Where the rules part">
      <table className="text-center text-[11px] tabular">
        <thead><tr><th className="px-2 text-left font-normal text-muted">Day</th>
          {result.days.map((day) => <th key={day} className="px-1 font-normal text-muted" title={day}>{day.slice(5)}</th>)}</tr></thead>
        <tbody>{result.sets.map((set, i) => <tr key={`${set.name}-${i}`}>
          <td className="px-2 text-left">{set.name}</td>
          {set.days.map((day) => {
            const mark = outcomeMark[day.outcome] ?? outcomeMark.active!
            return <td key={day.day} className={`px-1 ${mark.className}`} title={`${day.day}: ${mark.label}, close ${formatMoney(day.close)}`}>
              <span aria-label={`${set.name} ${day.day} ${mark.label}`}>{mark.mark}</span></td>
          })}
        </tr>)}</tbody>
      </table>
    </div>
    <p className="text-[11px] text-faint">{result.label}. {result.basis}. P passed, F failed, L the day's loss limit locked it; a lock ends that day at its level.</p>
  </div>
}

/** A pack template from the account's own rules, to fill in and import. */
export function packTemplate(account: Account, today: string): RulePack {
  const rules = { ...account.rules }
  delete rules.plan_id
  return {
    format: "openport.rule-pack", version: 1, id: "my-rules", name: "My rules", effective_from: today,
    source: "https://example.com/where-these-terms-come-from", verify: "Say how and when you checked these terms against their source",
    plans: [{ id: "plan-1", name: "My plan", initial_cash: account.evaluation.starting_balance, rules }],
  }
}

function download(name: string, text: string) {
  const url = URL.createObjectURL(new Blob([text], { type: "application/json" }))
  const link = document.createElement("a")
  link.href = url
  link.download = name
  link.click()
  URL.revokeObjectURL(url)
}

/** Imports versioned rule packs after the server validates them; they stay in this browser and apart from the built-in presets. */
export function RulePacksPanel({ account }: { account: Account }) {
  const [packs, setPacks] = useRulePacks()
  const [text, setText] = useState("")
  const [checked, setChecked] = useState<RulePackValidation>()
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  async function validate(source: string) {
    setPending(true)
    setError(undefined)
    setChecked(undefined)
    try {
      let parsed: unknown
      try { parsed = JSON.parse(source) } catch { throw new Error("The pack is not valid JSON") }
      const result = await api.validateRulePack(parsed)
      setChecked(result)
      if (result.valid && result.pack) { setPacks(withPack(packs, result.pack)); setText("") }
    } catch (failure) { setError(failure) } finally { setPending(false) }
  }
  return <Panel title="Rule packs">
    <p className="text-sm text-muted">A rule pack is a JSON plan file with an effective date, a source URL and a note on how its terms were checked.
      Imported terms are the pack author's: this server checks only that they are well formed. Start an attempt from one in a custom plan; the built-in presets stay separate and illustrative.</p>
    <textarea className="trade-input mt-2 h-28 w-full font-mono text-[11px]" aria-label="Rule pack JSON" value={text} placeholder='{"format": "openport.rule-pack", …}'
      onChange={(event) => setText(event.target.value)} />
    <div className="mt-2 flex flex-wrap items-center gap-2">
      <button type="button" className="trade-button" disabled={pending || !text.trim()} onClick={() => void validate(text)}>{pending ? "Checking…" : "Validate and import"}</button>
      <label className="trade-button cursor-pointer">Import file
        <input type="file" accept="application/json,.json" className="sr-only" onChange={(event) => {
          const file = event.target.files?.[0]
          if (file) void file.text().then(validate)
          event.target.value = ""
        }} /></label>
      <button type="button" className="trade-button" onClick={() => setText(packFile(packTemplate(account, new Date().toISOString().slice(0, 10))))}>Template from my rules</button>
    </div>
    <TradingError error={error} />
    {checked && !checked.valid && <ul role="alert" className="mt-2 list-disc pl-5 text-xs text-warn">
      {checked.errors.map((e, i) => <li key={i}><span className="font-medium">{e.where}</span>: {e.message}</li>)}
    </ul>}
    {checked?.valid && <p role="status" className="mt-2 text-xs text-muted">Imported {checked.pack?.name}.{checked.warnings.map((w) => ` ${w}.`).join("")}</p>}
    {packs.length > 0 && <ul className="mt-3 space-y-2" aria-label="Imported rule packs">
      {packs.map((pack) => <li key={pack.id} className="rounded-md border border-border p-2 text-xs">
        <div className="flex flex-wrap items-center gap-2">
          <span className="text-sm font-medium">{pack.name}</span><Badge>effective {pack.effective_from}</Badge>
          <a className="text-accent underline" href={pack.source} target="_blank" rel="noreferrer noopener">source</a>
          <span className="ml-auto flex gap-2">
            <button type="button" className="trade-button" onClick={() => download(`${pack.id}.json`, packFile(pack))}>Export</button>
            <button type="button" className="trade-button" onClick={() => setPacks(packs.filter((p) => p.id !== pack.id))}>Remove</button>
          </span>
        </div>
        <p className="mt-1 text-muted">Verify: {pack.verify}</p>
        <p className="mt-1 text-faint">{pack.plans.map((p) => `${p.name} (${ruleMoney(p.initial_cash)})`).join(" · ")}</p>
      </li>)}
    </ul>}
  </Panel>
}

/** Challenge economics on the pass-odds estimate: the program's expected value and break-even pass rate. */
export function ChallengeEconomicsPanel() {
  const live = useLive()
  const [odds, setOdds] = useState("")
  const [payout, setPayout] = useState("")
  const override = { ...(odds.trim() !== "" && Number.isFinite(Number(odds)) ? { payout_odds: Math.min(1, Math.max(0, Number(odds) / 100)) } : {}),
    ...(payout.trim() !== "" ? { payout: payout.trim() } : {}) }
  const query = useQuery({ queryKey: ["pass-economics", live.accountScope, live.trading?.account_version, override.payout_odds, override.payout],
    queryFn: ({ signal }) => api.passEconomics(20, 1000, override, signal), enabled: live.trading?.enabled === true, retry: false })
  const e = query.data?.economics
  const percent = (value: number | null | undefined) => value == null ? "—" : `${(value * 100).toFixed(1)}%`
  const dollars = (value: number | undefined) => value == null ? "—" : formatMoney(value.toFixed(2))
  return <Panel title="Challenge economics">
    <p className="text-xs text-muted">Illustrative estimate from past results, not a prediction: each program step's pass odds from a fresh attempt on this account's recorded days,
      its purchase, reset and activation fees, and the funded step's first payout.</p>
    <div className="mt-2 flex flex-wrap gap-3 text-xs">
      <label className="space-y-1"><span className="block text-muted">First payout odds (%)</span>
        <input className="trade-input w-28" inputMode="decimal" value={odds} placeholder="simulated" onChange={(event) => setOdds(event.target.value)} /></label>
      <label className="space-y-1"><span className="block text-muted">Your share of it ($)</span>
        <input className="trade-input w-28" inputMode="decimal" value={payout} placeholder="first cap" onChange={(event) => setPayout(event.target.value)} /></label>
    </div>
    {query.error ? <p role="status" className="mt-2 text-xs text-muted">{query.error.message}</p>
      : !query.data ? <p className="mt-2 text-xs text-muted">Estimating…</p>
      : !e?.available ? <p role="status" className="mt-2 text-xs text-muted">{e?.reason ?? "This server does not estimate challenge economics."}</p>
      : <>
        <dl className="mt-3 grid grid-cols-2 gap-3 text-sm tabular sm:grid-cols-4">
          <div><dt className="text-xs text-muted">Expected value</dt><dd className={e.expected_value != null && e.expected_value < 0 ? "text-gex-negative" : ""}>{dollars(e.expected_value)}</dd></div>
          <div><dt className="text-xs text-muted">Expected cost</dt><dd>{dollars(e.expected_cost)}</dd></div>
          <div><dt className="text-xs text-muted">Expected payout</dt><dd>{dollars(e.expected_payout)}</dd></div>
          <div><dt className="text-xs text-muted">Break-even pass rate</dt><dd>{e.break_even_pass == null ? "None" : percent(e.break_even_pass)}</dd></div>
        </dl>
        <table className="mt-3 w-full text-left text-xs tabular" aria-label="Program steps">
          <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
            {["Step", "Pass odds", "Within resets", "Expected attempts", "Expected cost"].map((h) => <th key={h} className="py-1 font-normal">{h}</th>)}
          </tr></thead>
          <tbody>{(e.steps ?? []).map((step) => <tr key={step.plan} className="border-t border-border/40">
            <td className="py-1">{step.plan}</td><td>{percent(step.pass)}</td><td>{percent(step.eventual_pass)}</td>
            <td>{step.expected_attempts.toFixed(2)}</td><td>{dollars(step.expected_cost)}</td>
          </tr>)}</tbody>
        </table>
        <p className="mt-2 text-[11px] text-faint">First payout odds {percent(e.payout_odds)} ({e.payout_odds_source}), worth {dollars(e.payout)} to you ({e.payout_source}). {e.assumption}.</p>
      </>}
  </Panel>
}
