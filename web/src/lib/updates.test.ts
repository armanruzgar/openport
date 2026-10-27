import { describe, expect, it, vi } from "vitest"
import { createUpdateStore, isNewerVersion, releaseEndpoint, updateInterval } from "./updates"

const unlocked = (check: () => Promise<void>) => check()
const response = (tag = "v0.4.0") => new Response(JSON.stringify({ tag_name: tag, draft: false, prerelease: false }))
function setup() {
  const items = new Map<string, string>()
  const storage = () => ({ getItem: (key: string) => items.get(key) ?? null, setItem: (key: string, value: string) => { items.set(key, value) } })
  const fetcher = vi.fn<typeof fetch>(async () => response())
  let time = Date.UTC(2026, 8, 27)
  const now = () => time
  const create = () => createUpdateStore(storage, fetcher, now, unlocked)
  return { items, storage, fetcher, now, create, store: create(), advance: (delta: number) => { time += delta } }
}

describe("release versions", () => {
  it.each([
    ["v0.10.0", "0.9.9", true], ["1.0.0", "0.99.99", true], ["0.3.1", "0.3.0", true],
    ["v0.3.0", "0.3.0", false], ["0.2.9", "0.3.0", false], ["0.3.0+build.2", "v0.3.0+build.1", false],
    ["0.3.0", "0.3.0-rc.1", true], ["0.3.0-rc.2", "0.3.0", false], ["0.3.0-rc.10", "0.3.0-rc.9", true],
    ["0.3.0-beta", "0.3.0-alpha", true], ["0.3.0-alpha.1", "0.3.0-alpha", true],
    ["0.3.0-alpha", "0.3.0-alpha.1", false], ["0.3.0-a", "0.3.0-1", true],
    ["0.3.0-1", "0.3.0-a", false], ["garbage", "0.3.0", false], ["0.4.0", "unknown", false],
    ["0.4", "0.3.0", false], ["00.4.0", "0.3.0", false], ["0.4.0-01", "0.3.0", false],
  ])("compares %s to %s", (candidate, current, newer) => {
    expect(isNewerVersion(candidate, current)).toBe(newer)
  })
})

describe("browser update checks", () => {
  it("is off by default, remembers opt-in and sends only the public release request", async () => {
    const { store, fetcher, create } = setup()
    expect(store.get().enabled).toBe(false)
    await store.check("0.3.0")
    expect(fetcher).not.toHaveBeenCalled()
    store.setEnabled(true)
    expect(create().get().enabled).toBe(true)
    await store.check("0.3.0")
    expect(fetcher).toHaveBeenCalledExactlyOnceWith(releaseEndpoint, {
      credentials: "omit", referrerPolicy: "no-referrer", redirect: "error", cache: "no-store", signal: expect.any(AbortSignal),
    })
    expect(store.get().release?.url).toBe("https://github.com/armanruzgar/openport/releases/tag/v0.4.0")
    store.setEnabled(false)
    expect(create().get().enabled).toBe(false)
  })

  it("waits for a known running version, including on older servers", async () => {
    const { store, fetcher } = setup()
    store.setEnabled(true)
    await store.check(undefined)
    await store.check("unknown")
    expect(fetcher).not.toHaveBeenCalled()
  })

  it("caches for 24 hours across reloads, version changes and opt-out/in", async () => {
    const { store, fetcher, create, advance } = setup()
    store.setEnabled(true)
    await store.check("0.3.0")
    const reloaded = create()
    expect(reloaded.get().release?.version).toBe("v0.4.0")
    reloaded.setEnabled(false)
    advance(updateInterval - 1)
    await reloaded.check("0.3.1")
    reloaded.setEnabled(true)
    await reloaded.check("0.3.1")
    expect(fetcher).toHaveBeenCalledTimes(1)
    advance(1)
    await reloaded.check("0.3.1")
    expect(fetcher).toHaveBeenCalledTimes(2)
  })

  it("reserves the day before fetching and deduplicates simultaneous checks", async () => {
    const { store, fetcher, create, items } = setup()
    let finish!: (value: Response) => void
    fetcher.mockImplementation(() => new Promise((resolve) => { finish = resolve }))
    store.setEnabled(true)
    const first = store.check("0.3.0")
    const second = store.check("0.3.0")
    await create().check("0.3.0")
    expect(items.get("openport.updates.cache")).toContain('"release":null')
    expect(fetcher).toHaveBeenCalledTimes(1)
    finish(response())
    await Promise.all([first, second])
  })

  it("uses a browser lock to recheck consent and the cache across tabs", async () => {
    const { store, storage, fetcher, now } = setup()
    store.setEnabled(true)
    const queued: (() => Promise<void>)[] = []
    const lock = (check: () => Promise<void>) => new Promise<void>((resolve) => { queued.push(async () => { await check(); resolve() }) })
    const first = createUpdateStore(storage, fetcher, now, lock)
    const second = createUpdateStore(storage, fetcher, now, lock)
    const checks = [first.check("0.3.0"), second.check("0.3.0")]
    for (const check of queued) await check()
    await Promise.all(checks)
    expect(fetcher).toHaveBeenCalledTimes(1)
    expect(second.get().release?.version).toBe("v0.4.0")
  })

  it("does not send a queued check after consent is withdrawn in another tab", async () => {
    const { store, storage, fetcher, now } = setup()
    store.setEnabled(true)
    let start!: () => Promise<void>
    const delayed = createUpdateStore(storage, fetcher, now, async (check) => { start = check })
    await delayed.check("0.3.0")
    store.setEnabled(false)
    await start()
    expect(fetcher).not.toHaveBeenCalled()
  })

  it.each(["network", "http", "json", "draft", "prerelease", "tag", "null"])("silently caches a %s failure for a day", async (failure) => {
    const { store, fetcher, create, advance } = setup()
    fetcher.mockImplementation(async () => {
      if (failure === "network") throw new TypeError("offline")
      if (failure === "http") return new Response("rate limited", { status: 403 })
      if (failure === "json") return new Response("invalid JSON")
      if (failure === "null") return new Response("null")
      return new Response(JSON.stringify({ tag_name: failure === "tag" ? "not-a-version" : "v0.4.0", draft: failure === "draft", prerelease: failure === "prerelease" }))
    })
    store.setEnabled(true)
    await expect(store.check("0.3.0")).resolves.toBeUndefined()
    expect(store.get().release).toBeNull()
    advance(updateInterval - 1)
    await create().check("0.3.0")
    expect(fetcher).toHaveBeenCalledTimes(1)
    advance(1)
    await store.check("0.3.0")
    expect(fetcher).toHaveBeenCalledTimes(2)
  })

  it("ignores untrusted response URLs and persists dismissal for that release", async () => {
    const { store, fetcher, create, advance } = setup()
    fetcher.mockResolvedValue(new Response(JSON.stringify({ tag_name: "v0.4.0", html_url: "javascript:bad()", draft: false, prerelease: false })))
    store.setEnabled(true)
    await store.check("0.3.0")
    expect(store.get().release?.url).toBe("https://github.com/armanruzgar/openport/releases/tag/v0.4.0")
    store.dismiss("v0.4.0")
    expect(create().get().dismissed).toBe("v0.4.0")
    advance(updateInterval)
    fetcher.mockResolvedValue(response("v0.5.0"))
    await store.check("0.3.0")
    expect(store.get().release?.version).toBe("v0.5.0")
    expect(store.get().dismissed).toBe("v0.4.0")
  })

  it("recovers a malformed cache and does not retry when the clock moves backwards", async () => {
    const { store, fetcher, items, advance } = setup()
    items.set("openport.updates.cache", "bad json")
    store.setEnabled(true)
    await store.check("0.3.0")
    advance(-updateInterval)
    await store.check("0.3.0")
    expect(fetcher).toHaveBeenCalledTimes(1)
  })

  it("checks once a day on plain HTTP, where browsers offer no locks", async () => {
    vi.stubGlobal("navigator", {})
    const { storage, fetcher, now, advance } = setup()
    const store = createUpdateStore(storage, fetcher, now)
    store.setEnabled(true)
    await store.check("0.3.0")
    expect(fetcher).toHaveBeenCalledTimes(1)
    expect(store.get().release?.version).toBe("v0.4.0")
    advance(updateInterval - 1)
    await createUpdateStore(storage, fetcher, now).check("0.3.0")
    expect(fetcher).toHaveBeenCalledTimes(1)
    vi.unstubAllGlobals()
  })

  it("skips checks when storage cannot preserve the daily limit", async () => {
    const fetcher = vi.fn<typeof fetch>()
    const store = createUpdateStore(() => { throw new Error("blocked") }, fetcher, Date.now, unlocked)
    store.setEnabled(true)
    await expect(store.check("0.3.0")).resolves.toBeUndefined()
    expect(fetcher).not.toHaveBeenCalled()
    const readOnly = createUpdateStore(() => ({ getItem: () => "true", setItem: () => { throw new Error("quota") } }), fetcher, Date.now, unlocked)
    await readOnly.check("0.3.0")
    expect(fetcher).not.toHaveBeenCalled()
  })
})
