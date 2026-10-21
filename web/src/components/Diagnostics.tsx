import { useState } from "react"
import { useLive } from "../api/live"
import { diagnostics, diagnosticsIssueUrl } from "../lib/diagnostics"
import { Panel } from "./ui"

export function Diagnostics() {
  const { status, tick, connection } = useLive()
  const [report, setReport] = useState("")
  const [issueUrl, setIssueUrl] = useState("")
  const [message, setMessage] = useState("")
  async function copy() {
    if (!status) return
    const text = diagnostics(status, tick?.feed ?? status.feed, connection)
    setReport(text)
    setIssueUrl(diagnosticsIssueUrl(text, status.version))
    try {
      await navigator.clipboard.writeText(text)
      setMessage("Diagnostics copied. Review them before reporting a bug.")
    } catch {
      setMessage("Clipboard unavailable. Select and copy the diagnostics below.")
    }
  }
  return (
    <Panel title="Diagnostics">
      <p className="mb-3 text-xs text-muted">Version, build, server platform, provider, feed state and recent error categories. No tokens, paths, account data or raw error messages. Nothing is sent automatically.</p>
      <button type="button" disabled={!status} onClick={() => { void copy() }} className="rounded border border-border px-3 py-1.5 text-sm hover:bg-raised disabled:opacity-50">Copy diagnostics</button>
      {report && <>
        <p role="status" className="my-2 text-xs text-muted">{message}</p>
        <textarea aria-label="Diagnostics to share" readOnly value={report} rows={10} onFocus={event => event.currentTarget.select()}
          className="w-full rounded border border-border bg-panel p-2 font-mono text-xs" />
        <a href={issueUrl} target="_blank" rel="noopener noreferrer" className="mt-2 inline-block text-sm text-accent underline decoration-dotted">Report a bug on GitHub</a>
      </>}
    </Panel>
  )
}
