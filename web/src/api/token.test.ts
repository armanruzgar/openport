// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest"
import { adoptLinkedToken, createTokenStore, writeToken } from "../lib/write-token"
import { api, bearer, readHeaders } from "./client"

afterEach(() => { writeToken.set(""); vi.unstubAllGlobals() })

describe("write tokens beyond ASCII", () => {
  it("send the token's UTF-8 bytes, which is what openportd compares", () => {
    expect(bearer("plain-token")).toBe("Bearer plain-token")
    // ö and é are two UTF-8 bytes each, sent as two Latin-1 characters.
    expect(bearer("tökén-secret")).toBe("Bearer tÃ¶kÃ©n-secret")
    // Cyrillic is above U+00FF, which fetch refused outright; its bytes are not.
    const cyrillic = bearer("ключ-secret")
    expect([...cyrillic].every((character) => character.charCodeAt(0) <= 0xff)).toBe(true)
    expect(() => new Headers({ Authorization: cyrillic })).not.toThrow()
    expect(() => new Headers({ Authorization: "Bearer ключ-secret" })).toThrow()
  })
  it("go on reads and writes", async () => {
    writeToken.set("ключ-secret")
    expect(readHeaders().Authorization).toBe(bearer("ключ-secret"))
    const send = vi.fn(async (_path: string, _init?: RequestInit) => new Response(JSON.stringify({ id: "ira", deleted: true })))
    vi.stubGlobal("fetch", send)
    await api.deletePortfolio("ira", "token")
    expect(new Headers(send.mock.calls[0]![1]!.headers).get("Authorization")).toBe(bearer("ключ-secret"))
  })
  it("are not taken from a link when they hold a control character", () => {
    const store = createTokenStore(() => window.sessionStorage)
    const history = { replaceState: vi.fn() }
    expect(adoptLinkedToken({ hash: "#token=bad%0Atoken", pathname: "/", search: "" }, history, store)).toBe(false)
    expect(store.get()).toBe("")
    expect(history.replaceState).toHaveBeenCalled()
    expect(adoptLinkedToken({ hash: "#token=t%C3%B6k%C3%A9n", pathname: "/", search: "" }, history, store)).toBe(true)
    expect(store.get()).toBe("tökén")
    store.set("")
  })
})
