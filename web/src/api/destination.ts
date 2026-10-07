import { activeAccount } from "../lib/active-account"
import { dataSource } from "../lib/data-source"

export interface Destination { source: ReturnType<typeof dataSource.get>; account: string; run?: string }
let run: string | undefined
const names = new Map<string, string>([["main", "Practice"]])
export function rememberAccounts(accounts: readonly { id: string; name: string }[]) {
  accounts.forEach(account => names.set(account.id, account.name))
}
// A tick may arrive before a locally requested replay's HTTP response. Hold only
// its notice until the response identifies which run the user actually requested.
let replayChange: { observed?: string } | undefined
const userRuns = new Set<string>()
export function rememberReplay(id: string | undefined) {
  const previous = run
  run = id
  const user = id ? userRuns.delete(id) : false
  if (!id || !previous || id === previous || dataSource.get() !== "replay") return
  if (replayChange) replayChange.observed = id
  else if (!user) announceDestination("The replay run changed in another window.")
}
export async function userReplayChange<T extends { replay: { id?: string } | null }>(request: () => Promise<T>): Promise<T> {
  const change: { observed?: string } = {}
  replayChange = change
  let requested: string | undefined
  try {
    const result = await request()
    requested = result?.replay?.id
    if (requested && requested !== run) userRuns.add(requested)
    return result
  } finally {
    if (replayChange === change) replayChange = undefined
    if (change.observed && change.observed !== requested && dataSource.get() === "replay") {
      announceDestination("The replay run changed in another window.")
    }
  }
}
export function captureDestination(): Destination {
  const source = dataSource.get()
  return { source, account: source === "live" ? activeAccount.get() : "main", run: source === "replay" ? run : undefined }
}
export function destinationLabel(destination: Destination) {
  return `${destination.source} account ${destination.source === "live" ? names.get(destination.account) ?? destination.account : "Replay"}${destination.run ? ` (${destination.run})` : ""}`
}
export class DestinationChangedError extends Error {
  constructor(readonly expected: Destination, readonly current = captureDestination()) {
    super(`Destination changed from ${destinationLabel(expected)} to ${destinationLabel(current)}. Close and reopen this action or explicitly confirm the new destination.`)
    this.name = "DestinationChangedError"
  }
}
export function checkDestination(expected: Destination) {
  const current = captureDestination()
  if (current.source !== expected.source || current.account !== expected.account || current.run !== expected.run) throw new DestinationChangedError(expected, current)
}
// Only set during a synchronous API method invocation. Never retained across await.
let expected: Destination | undefined
export function withDestination<T>(destination: Destination, action: () => T): T {
  const previous = expected
  expected = destination
  try { return action() } finally { expected = previous }
}
export function checkWriteDestination() {
  if (!expected) throw new Error("This action has no opening destination. Close and reopen it before submitting.")
  checkDestination(expected)
}

const notices = new Set<(message: string) => void>()
export function announceDestination(reason: string) {
  notices.forEach(listener => listener(`${reason} Orders now go to ${destinationLabel(captureDestination())}.`))
}
export function subscribeDestinationNotices(listener: (message: string) => void) {
  notices.add(listener)
  return () => { notices.delete(listener) }
}
