import { createServer } from "node:http"
import { afterEach, expect, it } from "vitest"
import { trades } from "../test/trading-fixtures"
import { requestAiReview } from "./ai-review"
let server
afterEach(async () => { if (server) await new Promise((resolve, reject) => server.close(e => e ? reject(e) : resolve())); server = undefined })
it("sends nothing until explicitly enabled, then posts only to the configured mock endpoint", async () => {
  const requests = []
  server = createServer((req, res) => {
    let body = ""; req.on("data", chunk => { body += String(chunk) }); req.on("end", () => {
      requests.push({ auth: req.headers.authorization, cookie: req.headers.cookie, body: JSON.parse(body) });
      res.setHeader("Content-Type", "application/json"); res.end(JSON.stringify({ review: "Compare heat with your planned risk." }))
    })
  })
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve))
  const address = server.address()
  if (!address || typeof address === "string") throw new Error("Missing mock address")
  const settings = { enabled: false, endpoint: `http://127.0.0.1:${address.port}/review`, key: "user-test-key" }
  await expect(requestAiReview(settings, trades[0], [])).rejects.toThrow("off")
  await expect(requestAiReview({ ...settings, enabled: true, key: "" }, trades[0], [])).rejects.toThrow("off")
  expect(requests).toHaveLength(0)
  await expect(requestAiReview({ ...settings, enabled: true }, trades[0], ["Heat >1R"])).resolves.toContain("planned risk")
  expect(requests).toEqual([{ auth: "Bearer user-test-key", cookie: undefined, body: { trade: trades[0], flags: ["Heat >1R"], paper_trade: true, instruction: expect.any(String) } }])
})
it("rejects insecure remote endpoints before sending", async () => {
  await expect(requestAiReview({ enabled: true, endpoint: "http://example.invalid/review", key: "test" }, trades[0], [])).rejects.toThrow("HTTPS")
})
