import { userReplayChange } from "../api/destination"
import { useActionApi } from "../api/action-client"
import { CopySettingsPicker } from "../components/CopySettingsPicker"
import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"
import { type ReplaySource, type ReplayStart } from "../api/client"
import { usePlans } from "../api/trading"
import { Dialog } from "../components/Dialog"
import { formatMoney } from "../lib/trading"
import type { View } from "../lib/route"
import { useLive } from "../api/live"
import type { ReplayRecording, ReplaySession, ReplayState } from "../api/types"
import { TradingError, WriteAccess } from "../components/TradingControls"
import { Badge, Empty, PageHeader, Panel, Segmented } from "../components/ui"
import { joinList } from "../lib/format"
import { useWriteToken } from "../lib/write-token"

export const replaySpeeds = [1, 2, 5, 10, 30, 60, 120, 300, 0] as const
export const speedLabel = (speed: number) => (speed === 0 ? "Max" : `${speed}×`)
const clockFormat = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", weekday: "short", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23" })
/** The replay's clock in New York time, "Tue, Sep 22, 15:04:31 ET". */
export const replayClock = (time: string | null) => (time && Number.isFinite(Date.parse(time)) ? `${clockFormat.format(Date.parse(time))} ET` : "Not started")
const dateFormat = new Intl.DateTimeFormat("en-US", { timeZone: "UTC", weekday: "short", month: "short", day: "numeric" })
const sessionName = (session: ReplaySession["session"]) => (session === "regular" ? "regular" : session === "curb" ? "curb" : "overnight")
/** A run's sessions grouped by trading date: "Wed, Sep 16 regular, curb · Thu, Sep 17 overnight, regular". */
export function sessionsLabel(sessions: ReplaySession[]) {
  const days: { date: string; names: string[] }[] = []
  for (const s of sessions) {
    const last = days.at(-1)
    if (last?.date === s.date) last.names.push(sessionName(s.session))
    else days.push({ date: s.date, names: [sessionName(s.session)] })
  }
  return days.map((d) => `${Number.isFinite(Date.parse(d.date)) ? dateFormat.format(Date.parse(d.date)) : d.date} ${d.names.join(", ")}`).join(" · ")
}
/** Where a run's clock is: inside one of its sessions, or the closed market before the next. */
export function sessionAt(sessions: ReplaySession[], time: string | null) {
  const now = time ? Date.parse(time) : NaN
  if (!Number.isFinite(now)) return null
  const index = sessions.findIndex((s) => now <= Date.parse(s.end))
  if (index < 0) return null
  return { index, open: now >= Date.parse(sessions[index]!.open) }
}
const nyParts = new Intl.DateTimeFormat("en-CA", { timeZone: "America/New_York", year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23" })
/** A time as a New York "YYYY-MM-DDTHH:MM", the value a datetime-local input holds. */
export function newYorkInput(time: string | null) {
  const at = time ? Date.parse(time) : NaN
  if (!Number.isFinite(at)) return ""
  const part = Object.fromEntries(nyParts.formatToParts(at).map((p) => [p.type, p.value]))
  return `${part.year}-${part.month}-${part.day}T${part.hour}:${part.minute}${part.second === "00" ? "" : `:${part.second}`}`
}
const size = (bytes: number) => bytes >= 1e9 ? `${(bytes / 1e9).toFixed(1)} GB` : bytes >= 1e6 ? `${(bytes / 1e6).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1e3))} KB`

/** Speed, pause, skip and stop for the running replay, from the page or the banner. */
export function useReplayControls() {
  const api = useActionApi()
  const { status } = useLive()
  const writeStatus = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), staleTime: 5_000 })
  const trading = status?.trading
  const token = useWriteToken()
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const busy = useRef(false)
  const interruptBusy = useRef(false)
  const [interruptPending, setInterruptPending] = useState(false)
  const [stepping, setStepping] = useState(false)
  const mode = writeStatus.data?.write ?? trading?.write ?? "disabled"
  const blocked = mode === "disabled" || (mode === "token" && !token)
  async function run(request: () => Promise<unknown>, read = false) {
    if (busy.current || (!read && blocked)) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try { await request() } catch (failure) { setError(failure) } finally { busy.current = false; setPending(false) }
  }
  async function interrupt(request: () => Promise<unknown>) {
    if (interruptBusy.current || blocked) return
    interruptBusy.current = true
    setInterruptPending(true)
    setError(undefined)
    try { await request() } catch (failure) { setError(failure) } finally { interruptBusy.current = false; setInterruptPending(false) }
  }
  return {
    error, pending, blocked, interruptPending, stepping,
    speed: (speed: number) => run(() => api.controlReplay({ speed }, mode)),
    pause: (paused: boolean) => (paused ? interrupt : run)(() => api.controlReplay({ paused }, mode)),
    skip: (skip = true) => run(() => api.controlReplay({ skip }, mode)),
    step: (until: string, then: () => void = () => {}) => run(async () => {
      setStepping(true)
      try { await api.controlReplay({ until }, mode) } finally { setStepping(false) }
      then()
    }),
    playTo: (play_until: string, speed: number) => run(() => api.controlReplay({ play_until, speed }, mode)),
    abort: () => interrupt(() => api.controlReplay({ abort: true }, mode)),
    stop: () => interrupt(() => api.stopReplay(mode)),
    start: (source: ReplaySource, speed: number, then: () => void, options?: ReplayStart) => run(async () => { await userReplayChange(() => api.startReplay(source, speed, mode, options)); then() }),
    resume: (id: string, speed: number, then: () => void) => run(async () => { await userReplayChange(() => api.resumeReplay(id, speed, mode)); then() }),
    restart: (id: string, at: string | undefined, then: () => void) => run(async () => { await userReplayChange(() => api.restartReplay(id, at, mode)); then() }),
    verify: (id: string, then: () => void) => run(async () => { await api.verifyReplay(id, mode); then() }),
    receipt: (id: string) => run(() => api.downloadVerificationReceipt(id), true),
    remove: (id: string, then: () => void) => run(async () => { await api.deleteReplay(id, mode); then() }),
  }
}

function Controls({ replay }: { replay: ReplayState }) {
  const controls = useReplayControls()
  const [until, setUntil] = useState("")
  const stepping = replay.stepping || controls.stepping
  const idle = controls.pending || controls.blocked || replay.finished || replay.fast_forwarding || stepping
  const target = until || newYorkInput(replay.settled_through ?? replay.time)
  return <div className="space-y-3">
    <div className="flex flex-wrap items-center gap-2">
      <fieldset disabled={idle}>
        <Segmented label="Replay speed" value={replay.speed} onChange={(speed) => void controls.speed(speed)}
          options={replaySpeeds.map((speed) => ({ value: speed, label: speedLabel(speed) }))} />
      </fieldset>
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked || replay.finished || replay.fast_forwarding || replay.stepping}
        onClick={() => void controls.pause(!replay.paused)}>{replay.paused ? "Resume" : "Pause"}</button>
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked || replay.finished || replay.fast_forwarding || stepping}
        title="Play the next event now, skipping a closed market" onClick={() => void controls.skip(!replay.skip_pending)}>{replay.skip_pending ? "Cancel skip" : "Skip gap"}</button>
      <button type="button" className="trade-button" disabled={controls.interruptPending || controls.blocked} onClick={() => void controls.stop()}>Stop</button>
      {(stepping || replay.pause_at) && <button type="button" className="trade-button" disabled={controls.interruptPending || controls.blocked} onClick={() => void controls.abort()}>Abort</button>}
    </div>
    {replay.skip_pending && <p role="status" className="text-xs text-warn">Skip queued for the next resume.</p>}
    {replay.pause_at && <p role="status" className="text-xs">Playing to {replayClock(replay.pause_at)} at {speedLabel(replay.speed)}.</p>}
    <div className="flex flex-wrap gap-2">
      {([['+15s', '+15s'], ['+1m', '+1m'], ['+5m', '+5m'], ['next', 'Next snapshot']] as const).map(([value, label]) =>
        <button key={value} type="button" className="trade-button" disabled={idle} onClick={() => void controls.step(value)}>{label}</button>)}
    </div>
    <form className="flex flex-wrap items-end gap-2 text-xs" onSubmit={(event) => { event.preventDefault(); if (until && !idle) void controls.step(until, () => setUntil("")) }}>
      <label>Step to (New York)<input aria-label="Step to" type="datetime-local" step="1" className="trade-input" value={target}
        onChange={(event) => setUntil(event.target.value.replace(/\.000$/, ""))} /></label>
      <button type="submit" className="trade-button" disabled={idle || !until} title="Play as fast as possible through this date and time, then pause">Step</button>
      <button type="button" className="trade-button" disabled={idle || !until} onClick={() => void controls.playTo(until, replay.speed)}>Play to</button>
    </form>
    <TradingError error={controls.error} />
  </div>
}

/** A run of several sessions: each one by date, the one playing marked. */
function RunSessions({ sessions, time }: { sessions: ReplaySession[]; time: string | null }) {
  const at = sessionAt(sessions, time)
  return <ol aria-label="Run sessions" className="mb-3 flex flex-wrap gap-1 text-[11px]">
    {sessions.map((s, index) => {
      const current = at?.index === index
      return <li key={`${s.date}-${s.session}`} className={`rounded border px-1.5 py-0.5 ${current && at.open ? "border-accent text-foreground" : "border-border text-muted"}`}
        aria-current={current && at.open ? "step" : undefined}>
        {sessionsLabel([s])}{current && !at.open ? " · next" : ""}
      </li>
    })}
  </ol>
}

/** What is playing: the demo's simulated day, or a recording from a provider. */
export function replayTitle(replay: ReplayState) {
  return replay.demo ? `${replay.file}${replay.seed ? ` · seed ${replay.seed}` : ""} · simulated prices, not market data` : `${replay.file} · ${replay.provider}${replay.imported ? " · imported" : ""} · ${replay.symbols.join(", ")}`
}

/**
 * Replays run a recorded day beside the live feed, with their own paper account and
 * chart: pick a recording, or the demo market's simulated day, set the pace, then
 * trade it from every page as if it were that day.
 */
export function ReplayView({ onNavigate }: { onNavigate?: (view: View) => void }) {
  const api = useActionApi()
  const live = useLive()
  const listing = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), refetchInterval: (query) => query.state.data?.history?.some((run) => run.verification?.status === "running") ? 1_000 : 5_000 })
  const controls = useReplayControls()
  const [speed, setSpeed] = useState(10)
  const plans = usePlans()
  const [plan, setPlan] = useState("practice")
  const [copyFrom, setCopyFrom] = useState("")
  const [startAt, setStartAt] = useState("")
  const [paused, setPaused] = useState(false)
  const [seedMode, setSeedMode] = useState("fresh")
  const [seed, setSeed] = useState("")
  const [deleting, setDeleting] = useState<string>()
  const [restarting, setRestarting] = useState<string>()
  const [restartAt, setRestartAt] = useState("")
  const options = (scenario: boolean): ReplayStart => ({ plan, ...(copyFrom ? { copy_settings_from: copyFrom } : {}), ...(startAt ? { start_at: startAt } : {}), paused,
    ...(scenario && seedMode !== "fresh" ? { seed: seedMode === "scenario" ? "scenario" : seed } : {}) })
  const badSeed = seedMode === "typed" && (!/^\d{1,20}$/.test(seed) || BigInt(seed || "0") > 18446744073709551615n)
  const replay = (live.source.startsWith("history:") ? null : live.replay) ?? listing.data?.replay ?? null
  const verifying = listing.data?.history?.some((run) => run.verification?.status === "running") ?? false
  const recordings = listing.data?.recordings ?? []
  const demos = listing.data?.demos?.length ? listing.data.demos : listing.data?.demo ? [listing.data.demo] : []
  const started = () => { live.switchSource("replay", "user"); void listing.refetch() }
  return <div className="min-w-0 space-y-4">
    <PageHeader title="Replay" subtitle="Trade a recorded day with its own paper account, at the pace you choose">
      {(demos.length > 0 || recordings.length > 0) && <Segmented label="Starting speed" value={speed} onChange={setSpeed}
        options={[1, 10, 60, 0].map((value) => ({ value, label: speedLabel(value) }))} />}
      {live.status?.trading && <WriteAccess trading={live.status.trading} />}
    </PageHeader>
    <Panel title="Start controls">
      <div className="flex flex-wrap items-end gap-3 text-xs">
        <label>Plan<select aria-label="Replay plan" className="trade-input" value={plan} onChange={(event) => setPlan(event.target.value)}>
          <option value="practice">Practice · practice</option>
          {(plans.data?.plans ?? []).filter((p) => p.id !== "practice" && !p.unlocked_by && p.rules.phase !== "funded").map((p) => <option key={p.id} value={p.id}>{p.name} · {p.id}</option>)}
        </select></label>
        <label>Start at (New York)<input aria-label="Start at" type="time" step="1" className="trade-input" value={startAt} onChange={(event) => setStartAt(event.target.value)} /></label>
        <label>Scenario seed<select aria-label="Seed mode" className="trade-input" value={seedMode} onChange={(event) => setSeedMode(event.target.value)}>
          <option value="fresh">Fresh seed</option><option value="scenario">Scenario’s seed</option><option value="typed">Type a seed</option>
        </select></label>
        {seedMode === "typed" && <label>Seed<input aria-label="Seed" inputMode="numeric" className="trade-input" value={seed} onChange={(event) => setSeed(event.target.value)} /></label>}
        <label className="flex items-center gap-2"><input type="checkbox" checked={paused} onChange={(event) => setPaused(event.target.checked)} />Pause at start</label>
      </div>
      <div className="mt-3"><CopySettingsPicker value={copyFrom} onChange={setCopyFrom} disabled={controls.pending} /></div>
      <p className="mt-2 text-xs text-muted">Leave the time blank for the open. Overnight times from 20:15 belong to the evening before the session date; morning times belong to that date.
        A scenario of several sessions starts at the time’s first occurrence in them; step to a later day once it runs.</p>
      {badSeed && <p className="mt-2 text-xs text-warn">Enter a whole seed from 0 to 18446744073709551615.</p>}
      {controls.pending && <p role="status" className="mt-2 text-xs text-muted">Preparing replay…</p>}
      <TradingError error={controls.error} />
    </Panel>
    {replay ? (
      <Panel title="Now replaying" actions={live.source === "replay"
        ? <button type="button" className="trade-button" onClick={() => live.switchSource("live", "user")}>Back to live</button>
        : <button type="button" className="trade-button border-accent" onClick={() => live.switchSource("replay", "user")}>Trade this replay</button>}>
        <div className="mb-3 flex flex-wrap items-baseline gap-x-4 gap-y-1">
          <span className="text-lg font-medium tabular">{replayClock(replay.time)}</span>
          <span className="text-sm text-muted">{replayTitle(replay)}</span>
          {replay.finished ? <Badge tone="neutral">finished</Badge> : replay.paused ? <Badge tone="warn">paused</Badge> : <Badge tone="positive">{speedLabel(replay.speed)}</Badge>}
        </div>
        {replay.sessions && replay.sessions.length > 1 && <RunSessions sessions={replay.sessions} time={replay.time} />}
        {replay.stepping && <div role="status" className="mb-3 text-sm">Stepping to the requested time; orders wait until it settles.</div>}
        {replay.fast_forwarding && <div role="status" className="mb-3 text-sm">Preparing start state… {Math.round((replay.progress ?? 0) * 100)}%<progress className="ml-2" max={1} value={replay.progress ?? 0} aria-label="Fast-forward progress" /></div>}
        <Controls replay={replay} />
        {replay.id && replay.durable !== false && <button type="button" className="trade-button mt-2" disabled={controls.pending || controls.blocked || replay.fast_forwarding || replay.stepping}
          onClick={() => { setRestarting(replay.id); setRestartAt(newYorkInput(replay.settled_through ?? replay.time)) }}>Restart from…</button>}
        {replay.restarted_from && <p className="mt-2 text-xs text-muted">Restarted from {replay.restarted_from.id} through {replayClock(replay.restarted_from.at)}.</p>}
        {replay.demo && replay.scenario && replay.seed && <button type="button" className="trade-button mt-2" disabled={controls.pending || controls.blocked}
          onClick={() => void controls.start({ demo: replay.scenario! }, replay.speed, started, { plan: replay.plan, seed: replay.seed!, start_at: replay.start_at || undefined, date: replay.date })}>Replay this seed</button>}
      </Panel>
    ) : null}
    {demos.length > 0 && <Panel title="Demo market">
      <p className="mb-3 text-sm">Simulated trading days in {joinList([...new Set(demos.flatMap((demo) => demo.symbols))])} options: prices are generated on this server, not
        market data. Each plays like a recording, with its own paper account, so every page works while markets are closed.</p>
      <div className="grid gap-2 md:grid-cols-2 xl:grid-cols-3">
        {demos.map((d) => {
          const playing = replay?.demo && replay.file === `Demo market: ${d.title}`
          return <div key={d.id ?? "demo"} className="flex flex-col gap-2 rounded-md border border-border p-3">
            <div className="flex items-baseline justify-between gap-2">
              <span className="font-medium">{d.title ?? "Demo market"}</span>
              <span className="text-[11px] text-muted">{d.symbols.join(", ")}</span>
            </div>
            <span className="text-[11px] text-muted">simulated · {d.sessions && d.sessions.length > 1 ? sessionsLabel(d.sessions) : d.session === "overnight" ? "overnight" : "regular session"}</span>
            {d.goal && <p className="text-xs">Goal: {d.goal}</p>}
            {d.description && <p className="text-xs text-muted">{d.description}</p>}
            <button type="button" className="trade-button mt-auto self-start border-accent text-foreground" disabled={controls.pending || controls.blocked || badSeed}
              aria-label={`${playing ? "Restart" : "Start"} ${d.title ?? "the demo"}`}
              onClick={() => void controls.start({ demo: d.id ?? true }, speed, started, options(true))}>{playing ? "Restart" : "Start"}</button>
          </div>
        })}
      </div>
    </Panel>}
    <Panel title="Recordings">
      <TradingError error={listing.error ?? controls.error} />
      {!listing.data ? <Empty>Loading recordings…</Empty> : recordings.length === 0 ? (
        <div className="space-y-2 text-sm text-muted">
          <p>No recordings in <code className="text-foreground">{listing.data.directory || "the recordings directory"}</code> yet.</p>
          <p>Start openportd with <code className="text-foreground">--record-dir {listing.data.directory || "~/.openport/recordings"}</code> to record each session there;
            every run becomes a day you can replay and trade again.</p>
        </div>
      ) : (
        <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Recordings">
          <table className="w-full text-left text-xs tabular">
            <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
              {["Recording", "Provider", "Symbols", "Started", "Size", ""].map((h) => <th key={h} scope="col" className="px-2 py-2 font-normal">{h}</th>)}
            </tr></thead>
            <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40">
              {recordings.map((recording: ReplayRecording) => <tr key={recording.file}>
                <td className="font-medium">{recording.file}{recording.error && <div className="text-[10px] text-warn">{recording.error}</div>}</td>
                <td>{recording.provider ?? "—"}{recording.simulated ? " · simulated" : recording.imported ? " · imported" : " · recording"}</td>
                <td>{recording.symbols?.join(", ") ?? "—"}</td>
                <td>{recording.started ? replayClock(recording.started) : "—"}</td>
                <td>{size(recording.bytes)}</td>
                <td className="text-right"><button type="button" className="trade-button" disabled={!!recording.error || controls.pending || controls.blocked}
                  aria-label={`Replay ${recording.file}`}
                  onClick={() => void controls.start({ file: recording.file }, speed, started, options(false))}>
                  {replay?.file === recording.file && !replay.demo ? "Restart" : "Replay"}</button></td>
              </tr>)}
            </tbody>
          </table>
        </div>
      )}
    </Panel>
    <Panel title="Finished runs">
      {!listing.data?.history?.length ? <Empty>No finished runs yet.</Empty> : <div className="space-y-2">
        {listing.data.history.map((run) => <div key={run.id} className="flex flex-wrap items-center gap-3 rounded-md border border-border p-3 text-xs">
          <span className="font-medium">{run.file}</span><Badge tone="neutral">replay</Badge>
          <span>{run.demo ? "simulated" : "recording"}{run.seed ? ` · seed ${run.seed}` : ""}</span>
          <span>{run.date} · {run.start_at || "open"} · {run.plan_name ?? run.plan}{run.plan_id ? ` · ${run.plan_id}` : ""}</span>
          <Badge tone={run.result === "pass" ? "positive" : run.result === "fail" ? "warn" : "neutral"}>{run.result}</Badge>
          <span>P&amp;L {run.pnl === null ? "—" : formatMoney(run.pnl)}{run.valuation_complete === false ? " (incomplete marks)" : ""}</span>
          {run.journal_size && <span>{size(run.journal_size.bytes)} journal · {run.journal_size.records.toLocaleString()} records</span>}
          {run.journal_size?.warning && <p className="w-full text-warn">{run.journal_size.warning}</p>}
          {run.verification_cost && <p className="w-full text-muted">Verification estimate: about {run.verification_cost.estimated_seconds} seconds or more.
            {run.verification_cost.warning && <span className="text-warn"> {run.verification_cost.warning}</span>}</p>}
          {run.error && <span className="text-warn">{run.error}</span>}
          {run.interrupted && <Badge tone="warn">interrupted</Badge>}
          {(run.torn || run.truncated || run.mismatch) && <div className="w-full text-warn" role="alert">
            <strong>{run.torn ? "Torn journal" : run.truncated ? "Truncated journal" : "Journal mismatch"}</strong>
            {run.integrity_message && <p className="mt-1 break-all">{run.integrity_message}</p>}
          </div>}
          {run.verification && <div className="w-full space-y-1" role="status">
            {run.verification.status !== "idle" && <Badge tone={run.verification.status === "passed" ? "positive" : run.verification.status === "failed" ? "warn" : "neutral"}>
              {run.verification.status === "running" ? "Verifying" : `Verification ${run.verification.status}`}
              {run.verification.status === "running" && run.verification.progress !== undefined ? ` · ${Math.round(run.verification.progress * 100)}%` : ""}
            </Badge>}
            <p className="whitespace-pre-line break-all text-muted">{run.verification.message}</p>
          </div>}
          <span className="ml-auto flex flex-wrap gap-2">
            <button type="button" className="trade-button" disabled={controls.blocked || controls.pending || !!run.error || run.verification?.status === "running"}
              onClick={() => { setRestarting(run.id); setRestartAt("") }}>Restart from…</button>
            <button type="button" className="trade-button" disabled={!run.finished || controls.blocked || controls.pending || verifying}
              onClick={() => void controls.verify(run.id, () => { void listing.refetch() })}>Verify</button>
            {(run.verification?.status === "passed" || run.verification?.status === "failed") &&
              <button type="button" className="trade-button" disabled={controls.pending} onClick={() => void controls.receipt(run.id)}>Download receipt</button>}
            {run.interrupted && <button type="button" className="trade-button" disabled={controls.blocked || controls.pending || run.verification?.status === "running"}
            title="Replay its recorded orders and continue, paused where it stopped" onClick={() => void controls.resume(run.id, speed, started)}>Resume</button>}{(["journal", "dashboard"] as const).map((view) => <button key={view} type="button" className="trade-button" disabled={!!run.error}
            onClick={() => { live.switchSource(`history:${run.id}`, "user"); onNavigate?.(view) }}>Open {view}</button>)}
            <button type="button" className="trade-button" disabled={controls.blocked || controls.pending || run.verification?.status === "running"} onClick={() => setDeleting(run.id)}>Delete</button></span>
        </div>)}
      </div>}
    </Panel>
    {restarting && <Dialog title="Restart with the same commands" onClose={() => setRestarting(undefined)}>
      <p className="text-sm">Start a new run with this run’s settings and commands through the chosen market time, including commands at that time. The source journal is kept.</p>
      <label className="mt-3 block text-xs">Restart at (New York)<input aria-label="Restart at" type="datetime-local" step="1" className="trade-input" value={restartAt} onChange={(event) => setRestartAt(event.target.value.replace(/\.000$/, ""))} /></label>
      <p className="my-2 text-xs text-muted">Leave blank for the source run’s start. The new run starts paused.</p>
      <TradingError error={controls.error} />
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked} onClick={() => void controls.restart(restarting, restartAt || undefined, () => { setRestarting(undefined); started() })}>Restart run</button>
      <button type="button" className="trade-button" onClick={() => setRestarting(undefined)}>Cancel</button>
    </Dialog>}
    {deleting && <Dialog title="Delete replay run?" onClose={() => setDeleting(undefined)}>
      <p className="text-sm">This permanently deletes the run’s journal and trades. This cannot be undone.</p>
      <TradingError error={controls.error} />
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked} onClick={() => void controls.remove(deleting, () => { if (live.source === `history:${deleting}`) live.switchSource("live", "user"); setDeleting(undefined); void listing.refetch() })}>Delete run</button>
      <button type="button" className="trade-button" onClick={() => setDeleting(undefined)}>Cancel</button>
    </Dialog>}
    <p className="text-[11px] text-muted">
      A replay runs on the recording's own clock, so sessions, the feed delay and paper-trading rules behave as they did that day.
      Each run starts a separate account on the chosen plan. Its journal is kept when storage and writes are enabled; finished runs open read-only. Your live accounts keep running.
    </p>
  </div>
}
