import type { Status } from "../api/types"
import type { Connection } from "../api/connection"

// Provider errors can contain URLs, credentials and local paths. Export only
// known error categories, never the original message or a whole status object.
function errorCategory(message: string): string {
  const http = /\bHTTP[_ ]([45][0-9]{2})\b/.exec(message)
  if (http) return `HTTP_${http[1]}`
  for (const code of ["JOURNAL_LOCKED", "JOURNAL_DAMAGED", "DISK_FULL", "RATE_LIMITED", "TIMEOUT"])
    if (message.startsWith(code)) return code
  return "error reported (details omitted)"
}

export function diagnostics(status: Status, feed: Pick<Status["feed"], "state" | "message">, connection: Connection): string {
  const provider = /^(cboe|databento|massive|thetadata|tradier|tastytrade|demo|replay)(?:\b|$)/i.exec(status.provider.name)?.[1]?.toLowerCase() ?? "other"
  const errors: string[] = []
  const add = (source: string, message: string | null | undefined) => {
    if (message) errors.push(`${source}: ${errorCategory(message)}`)
  }
  if (feed.state === "error") add("feed", feed.message)
  for (const underlying of status.underlyings) add("underlying feed", underlying.last_error)
  add("history store", status.series?.last_error)
  add("circuit breaker", status.circuit_breaker?.error)
  for (const channel of status.notifications?.channels ?? []) add("notification delivery", channel.last_error)
  const recent = [...new Set(errors)].slice(0, 10)
  return [
    `OpenPort: ${status.version ?? "unavailable"}`,
    `Build: ${status.build?.revision ?? "unavailable"}`,
    `Compiler: ${status.build?.compiler ?? "unavailable"}`,
    `Server platform: ${status.build?.platform ?? "unavailable"}`,
    `Provider: ${provider}${status.provider.simulated ? " (simulated)" : ""}`,
    `Feed: ${feed.state}`,
    `Browser connection: ${connection}`,
    "Recent errors (last reported by the server; message details omitted):",
    ...(recent.length ? recent.map(error => `- ${error}`) : ["- none reported"]),
  ].join("\n")
}

export function diagnosticsIssueUrl(report: string, version?: string): string {
  const query = new URLSearchParams({ template: "bug_report.yml", version: version ?? "unavailable", diagnostics: report })
  return `https://github.com/armanruzgar/openport/issues/new?${query}`
}
