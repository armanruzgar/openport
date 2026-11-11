import { act } from "react"
import { vi } from "vitest"
import { testTimeout } from "./timeout"

// A whole test that renders may take this long: it guards liveness, not speed.
export const renderTimeout = testTimeout

const env = (globalThis as typeof globalThis & { process?: { env: Record<string, string | undefined> } }).process?.env

// How long waitForRender retries its check before failing with the check's own
// error, which says what the page lacks. Renders settle in milliseconds; on a
// heavily loaded machine, set OPENPORT_RENDER_WAIT_MS to wait longer.
export const renderWait = Number(env?.OPENPORT_RENDER_WAIT_MS) || 20_000

export async function waitForRender(check: () => void, timeout = renderWait) {
  try {
    await vi.waitFor(async () => {
      await act(async () => {})
      check()
    }, { timeout })
  } catch (error) {
    if (error instanceof Error) error.message += ` (still failing after waitForRender's ${timeout / 1000} s)`
    throw error
  }
}
