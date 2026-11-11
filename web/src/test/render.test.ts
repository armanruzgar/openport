import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import workflow from "../../../.github/workflows/ci.yml?raw"
import { renderWait, waitForRender } from "./render"
import { testTimeout } from "./timeout"

beforeEach(() => { vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true) })
afterEach(() => { vi.unstubAllGlobals() })

describe("waitForRender", () => {
  it("fails with the check's own message once its wait runs out", async () => {
    const started = Date.now()
    const failure = await waitForRender(() => expect("rendered text").toContain("Morning put spread"), 300)
      .then(() => undefined, (error: unknown) => error)
    expect(failure).toBeInstanceOf(Error)
    // The check's assertion, with its diff, rather than a bare "Timed out in waitFor!".
    expect(String(failure)).toMatch(/^AssertionError: expected .rendered text. to contain .Morning put spread./)
    expect(String(failure)).toContain("still failing after waitForRender's 0.3 s")
    expect(Date.now() - started).toBeLessThan(renderWait)
  })

  it("retries until the check passes", async () => {
    let checks = 0
    await waitForRender(() => expect(++checks).toBeGreaterThanOrEqual(3))
    expect(checks).toBe(3)
  })

  it("gives up before a test's deadline, which ends well before CI cancels the web job", () => {
    const job = /^ {2}web:\n(?: {4}.*\n)*? {4}timeout-minutes: (\d+)$/m.exec(workflow)
    expect(job?.[1], "the web job's timeout-minutes in .github/workflows/ci.yml").toBeDefined()
    expect(renderWait).toBeLessThan(testTimeout)
    expect(testTimeout).toBeLessThanOrEqual(Number(job?.[1]) * 60_000 / 2)
  })
})
