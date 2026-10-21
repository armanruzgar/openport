import type { BacktestStart } from "./backtest-types"
import specText from "../../../docs/openapi.yaml?raw"
import backtestTypes from "./backtest-types.ts?raw"
import marketTypes from "./types.ts?raw"
import playbookTypes from "./playbook-types.ts?raw"
import tradingTypes from "./trading-types.ts?raw"
import { describe, expect, it } from "vitest"

// openapi.yaml uses JSON syntax, a YAML subset; no YAML dependency is needed.
interface Schema { $ref?: string; type?: string; const?: string; anyOf?: Schema[]; items?: Schema; properties?: Record<string, Schema>; required?: string[]; additionalProperties?: Schema | boolean }
const spec = JSON.parse(specText) as { components: { schemas: Record<string, Schema> } }
const source = [marketTypes, tradingTypes, playbookTypes].join("\n")
  .replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, "")
const normalize = (value: string) => value.split("|").map((part) => part.trim()).sort().join(" | ")
function wireType(schema: Schema): string {
  if (schema.$ref) return schema.$ref.split("/").at(-1)!
  if (schema.anyOf) return normalize(schema.anyOf.map(wireType).join(" | "))
  if (schema.items) return `${wireType(schema.items)}[]`
  if (schema.type === "object" && typeof schema.additionalProperties === "object") return `Record<string, ${wireType(schema.additionalProperties)}>`
  if (schema.const !== undefined) return JSON.stringify(schema.const)
  return schema.type!
}

describe("checked API core types", () => {
  it("keeps position disposal fields and closure kinds in sync", () => {
    const position = /export interface Position \{([\s\S]*?)\n\}/.exec(source)![1]!
    for (const field of ["no_bid", "do_not_exercise"]) {
      expect(position).toMatch(new RegExp(`${field}\\?: boolean`))
      expect(wireType(spec.components.schemas.Position!.properties![field]!)).toBe("boolean")
    }
    expect(normalize(/settle_by\?: ([^\n]+)/.exec(position)![1]!)).toBe(wireType(spec.components.schemas.Position!.properties!.settle_by!))
    const trade = /export interface Trade \{([\s\S]*?)\n\}/.exec(source)![1]!
    expect(normalize(/closure: ([^\n]+)/.exec(trade)![1]!)).toBe(wireType(spec.components.schemas.Trade!.properties!.closure!))
    for (const field of ["settlement_value", "settlement_source"]) {
      expect(normalize(new RegExp(`${field}\\?: ([^\\n]+)`).exec(trade)![1]!)).toBe(wireType(spec.components.schemas.Trade!.properties![field]!))
    }
    const exits = /export type TradeExit = ([^\n]+)/.exec(source)![1]!
    expect(normalize(`${exits} | null`)).toBe(wireType(spec.components.schemas.Trade!.properties!.closed_by!))
  })
  it("account margin fields match the rules and both request contracts", () => {
    const rules = spec.components.schemas.AccountRules!.properties!
    expect(wireType(rules.account_type!)).toBe(normalize('"margin" | "cash" | "ira"'))
    expect(wireType(rules.margin!)).toBe(normalize('"strategy" | "portfolio"'))
    for (const key of ["house_margin_percent", "pm_vol_shock"]) expect(wireType(rules[key]!)).toBe("integer")
    const accountType = /export type AccountType = ([^\n]+)/.exec(source)![1]!
    expect(normalize(accountType)).toBe(wireType(rules.account_type!))
    const model = /export interface MarginModel \{([^}]+)\}/.exec(source)![1]!
    for (const field of ["margin", "account_type", "house_margin_percent", "pm_vol_shock"]) {
      expect(model).toMatch(new RegExp(`${field}\\?:`))
      expect(source).toMatch(/export type ResetRequest = [^\n]+& MarginModel/)
      expect(source).toMatch(/export type CreateAccountRequest = [^\n]+& MarginModel/)
    }
  })
  it.each(["BuildInfo", "ProgramCosts", "SizeScaling", "SizeScale", "SizeScalingStatus", "AttemptSummary", "ScalingStep", "ScalingStatus", "PayoutRules", "PayoutStatus", "PayoutBestDay", "Candle", "OptionQuote", "ChainRow", "Fill", "OrdersResponse", "EquitySample", "NotificationChannel", "NotificationStatus", "Alert", "AlertsResponse", "AlertResponse", "AlertDeleted",
    "Guardrails", "GuardrailState", "AccountDamage", "JournalSize", "EquityHistory", "VerificationCost", "SettlementSource", "SettlementRecord", "SettlementsResponse", "StockPreview", "MarginLeg", "MarginPart", "MarginScan", "MarginUnderlying", "FillFees", "FeeSchedule", "PriceRange", "PriceOdds", "ProbabilityHorizon", "Probability",
    "ProfileLevel", "ProfileHorizon", "Walk", "WalkStep", "Limits", "TokenStatus", "RunJournal", "RunInput", "VerificationRun", "RunVerification", "ForwardTestWindow", "ForwardTest", "PlaybookReport"])("%s matches OpenAPI fields, types and nullability", (name) => {
    const body = new RegExp(`export interface ${name} \\{([^}]+)\\}`).exec(source)?.[1]
    expect(body).toBeDefined()
    const fields = [...body!.matchAll(/(\w+)(\?)?\s*:\s*([^;\n}]+)/g)]
    const schema = spec.components.schemas[name]!
    expect(fields.map((field) => field[1]).sort()).toEqual(Object.keys(schema.properties!).sort())
    for (const [, field, optional, type] of fields) {
      // TypeScript has no integer type: an OpenAPI integer is a number on the wire.
      expect(normalize(type!), `${name}.${field}`).toBe(normalize(wireType(schema.properties![field!]!).replace(/\binteger\b/g, "number")))
      // Browser types allow fields absent on older servers; all its required
      // fields must still be required by the current HTTP contract.
      if (!optional) expect(schema.required, `${name}.${field}`).toContain(field)
    }
  })
})

it("keeps extended TIFs and sparse order metadata in the wire contract", () => {
  const declared = /export type TimeInForce = ([^\n]+)/.exec(source)![1]!
  expect(normalize(declared)).toBe(wireType(spec.components.schemas.Order!.properties!.time_in_force!))
  for (const key of ["good_till", "limit_ticks"]) {
    expect(source).toContain(`${key}?:`)
    expect(spec.components.schemas.Order!.properties![key]).toBeDefined()
  }
})

it("requires nullable deadlines, walks and groups on pending orders", () => {
  const pending = /export interface PendingOrder \{([^}]+)\}/.exec(source)![1]!
  const schema = spec.components.schemas.PendingOrder!
  for (const field of ["good_till", "walk", "group"]) {
    expect(normalize(new RegExp(`${field}: ([^;\\n]+)`).exec(pending)![1]!)).toBe(wireType(schema.properties![field]!))
    expect(schema.required).toContain(field)
  }
})

it("keeps alert request fields and optional terms in sync", () => {
  const body = /export interface AlertRequest \{([^}]+)\}/.exec(source)![1]!
  const fields = [...body.matchAll(/(\w+)(\?)?\s*:\s*([^;\n}]+)/g)]
  const schema = spec.components.schemas.AlertRequest!
  const scope = /export type AlertScope = ([^\n]+)/.exec(source)![1]!
  expect(fields.map((field) => field[1]).sort()).toEqual(Object.keys(schema.properties!).sort())
  for (const [, field, optional, type] of fields) {
    expect(normalize(type === "AlertScope" ? scope : type!)).toBe(wireType(schema.properties![field!]!))
    expect(schema.required?.includes(field!)).toBe(!optional)
  }
})


it("keeps the preview sizing basis and nullability in sync", () => {
  const declared = /max_units_basis\?: ([^\n]+)/.exec(source)![1]!
  expect(normalize(declared)).toBe(wireType(spec.components.schemas.OrderPreview!.properties!.max_units_basis!))
  expect(spec.components.schemas.OrderPreview!.required).toContain("max_units_basis")
})

it("publishes a stage HTTP order separately from the legacy request", () => {
  const stage = spec.components.schemas.StagedOrder!.properties!
  expect(wireType(stage.order!)).toBe("NewOrder")
  expect(wireType(stage.request!)).toBe("object")
  expect(playbookTypes).toContain("order?: NewOrder")
  expect(playbookTypes).toContain("request?: Record<string, unknown>")
})

it("reports nullable preset ids beside plan names", () => {
  for (const name of ["TradingStatus", "AccountRules", "AttemptSummary"]) {
    expect(wireType(spec.components.schemas[name]!.properties!.plan_id!)).toBe("null | string")
    expect(new RegExp(`export interface ${name} \{([^}]+)\}`).exec(source)?.[1]).toContain("plan_id?: string | null")
  }
})


it("keeps account management and copied-setting fields in sync", () => {
  expect(Object.keys(spec.components.schemas.UpdateAccountRequest!.properties!).sort()).toEqual(["archived", "name"])
  expect(source).toContain("archived?: boolean")
  expect(source).toContain("copy_settings_from?: string")
  expect(wireType(spec.components.schemas.Risk!.properties!.pending_requires_reset!)).toBe("boolean")
  expect(source).toContain("pending_requires_reset?: boolean")
})

it("keeps archived account and history verification fields backward compatible", () => {
  for (const name of ["Account", "ReplayHistory"]) {
    const body = new RegExp(`export interface ${name}(?: extends ReplayState)? \\{([\\s\\S]*?)\\n\\}`).exec(source)![1]!
    for (const field of ["journal", "journal_found", "torn", "bytes_cut", "truncated", "mismatch", "integrity_message", "verification"]) {
      const type = new RegExp(`${field}\\?: ([^\\n]+)`).exec(body)![1]!
      expect(normalize(type)).toBe(wireType(spec.components.schemas[name]!.properties![field]!).replace(/\binteger\b/g, "number"))
    }
  }
})

it("keeps backtest day rows and saved-run controls backward compatible on the wire", () => {
  const schemas = spec.components.schemas
  const day = /export interface BacktestEvaluationDay \{([\s\S]*?)\n\}/.exec(backtestTypes)![1]!
  const fields = [...day.matchAll(/(\w+)(\?)?\s*:\s*([^;\n}]+)/g)]
  expect(fields.map((field) => field[1]).sort()).toEqual(Object.keys(schemas.BacktestEvaluationDay!.properties!).sort())
  for (const name of ["start_balance", "end_balance", "start_equity", "end_equity", "target", "target_progress", "peak"]) {
    expect(wireType(schemas.BacktestEvaluationDay!.properties![name]!)).toBe("string")
    expect(day).toContain(`${name}: string`)
  }
  for (const name of ["pnl", "floor", "floor_distance"]) {
    expect(wireType(schemas.BacktestEvaluationDay!.properties![name]!)).toBe("null | string")
    expect(day).toContain(`${name}: string | null`)
  }
  expect(backtestTypes).toContain("day_rows?: BacktestEvaluationDay[]")
  expect(backtestTypes).toContain("keep?: boolean; bytes?: number")
  expect(schemas.BacktestState!.required).not.toContain("keep")
  expect(schemas.BacktestState!.required).not.toContain("bytes")
  expect(Object.keys(schemas.BacktestComparison!.properties!)).toEqual(expect.arrayContaining(["runs", "daily", "combined", "different_inputs", "different_plans"]))
})

it("keeps replay control feedback and restart provenance optional on old sidecars", () => {
  const state = spec.components.schemas.ReplayState!
  const history = spec.components.schemas.ReplayHistory!
  for (const field of ["skip_pending", "pause_at", "settled_through", "restarted_from"]) {
    expect(source).toMatch(new RegExp(`${field}\\?:`))
    expect(history.properties![field]).toEqual(state.properties![field])
    expect(state.required).not.toContain(field)
    expect(history.required).not.toContain(field)
  }
  for (const field of ["until", "play_until", "abort", "skip"])
    expect(spec.components.schemas.ReplayControl!.properties).toHaveProperty(field)
  for (const field of ["restart", "at"])
    expect(spec.components.schemas.ReplayStart!.properties).toHaveProperty(field)
  expect(spec.components.schemas.ReplayResponse!.properties).toHaveProperty("aborted")
})


it("keeps soft floor warning codes and request-only range validation in sync", () => {
  const schema = JSON.parse(specText).components.schemas
  const body = /export interface RiskWarning \{([^}]+)\}/.exec(source)![1]!
  expect(normalize(/code: ([^\n]+)/.exec(body)![1]!)).toBe(normalize(schema.RiskWarning.properties.code.enum.map((v: string) => JSON.stringify(v)).join(" | ")))
  expect(schema.RiskWarning.properties.code.enum).toContain("SOFT_FLOOR_UNUSED")
  expect(schema.GuardrailsInput.properties.soft_floor_percent).toMatchObject({ type: "integer", minimum: 0, maximum: 99 })
  expect(schema.Guardrails.properties.soft_floor_percent.maximum).toBeUndefined()
  expect(schema.GuardrailsRequest.properties.guardrails.$ref).toBe("#/components/schemas/GuardrailsInput")
})

it("keeps kill-switch reset decisions and actual latch history in the wire contract", () => {
  const schemas = JSON.parse(specText).components.schemas
  for (const name of ["KillState", "KillChange"]) {
    const body = new RegExp(`export interface ${name} \\{([^}]+)\\}`).exec(source)![1]!
    const fields = [...body.matchAll(/(\w+)(\?)?\s*:\s*([^;\n}]+)/g)]
    expect(fields.map((field) => field[1]).sort()).toEqual(Object.keys(schemas[name].properties).sort())
    for (const [, field, , type] of fields) {
      const schema = schemas[name].properties[field!]
      const wire = schema.enum ? normalize(schema.enum.map((v: string) => JSON.stringify(v)).join(" | ")) : wireType(schema)
      expect(normalize(type!)).toBe(wire)
      expect(schemas[name].required).toContain(field)
    }
  }
  expect(schemas.KillState.properties.reset_blocked.description).toContain("not a prediction for POST /api/account/reset")
})

it("accepts custom funded scaling plans in the backtest wire type", () => {
  const request: BacktestStart = { playbook: "sample", scenarios: 1, seed: "42", plan: {
    initial_cash: "50000.00", rules: { phase: "funded", payouts: { qualifying_days: 1 },
      scaling: [{ profit: "0.00", contracts: 2 }, { profit: "1500.00", contracts: 3 }] } } }
  expect(typeof request.plan).toBe("object")
  expect(wireType(spec.components.schemas.AccountRules!.properties!.scaling!)).toBe("ScalingStep[]")
  expect(wireType(spec.components.schemas.AccountRulesInput!.properties!.scaling!)).toBe("ScalingStep[]")
  expect(wireType(spec.components.schemas.Evaluation!.properties!.scaling!)).toBe("ScalingStatus | null")
})

it("includes nullable account size scaling in rules, backtest rules and funded standing", () => {
  for (const name of ["AccountRules", "AccountRulesInput"])
    expect(wireType(spec.components.schemas[name]!.properties!.size_scaling!)).toBe("SizeScaling | null")
  expect(wireType(spec.components.schemas.Evaluation!.properties!.size_scaling!)).toBe("SizeScalingStatus | null")
  const request: BacktestStart = { playbook: "sample", scenarios: 1, seed: "0", plan: { initial_cash: "50000",
    rules: { phase: "funded", payouts: { qualifying_days: 1 }, flat_time: "15:45", no_overnight: true,
      scaling: [{ profit: "0.00", contracts: 2 }], size_scaling: { profit_percent: 10, payouts: 2,
      days: 80, increase_percent: 25, max_balance: "200000" } } } }
  expect(request.plan).toHaveProperty("rules.size_scaling.days", 80)
  for (const name of ["AccountRules", "AccountRulesInput"])
    expect(Object.keys(spec.components.schemas[name]!.properties!)).toEqual(expect.arrayContaining([
      "flat_time", "no_overnight", "scaling", "size_scaling",
    ]))
  expect(Object.keys(spec.components.schemas.Evaluation!.properties!)).toEqual(expect.arrayContaining([
    "flat_time", "flat_now", "scaling", "size_scaling",
  ]))
})

it("keeps verification and program cost settings aligned with both request contracts", () => {
  const rules = spec.components.schemas.AccountRules!.properties!
  expect(wireType(rules.phase!)).toBe(normalize('"evaluation" | "verification" | "funded"'))
  for (const key of ["evaluation_fee", "reset_fee", "activation_fee"]) expect(wireType(rules[key]!)).toBe("Money")
  expect(wireType(rules.max_resets!)).toBe("integer")
  expect(source).toMatch(/export type ResetRequest = [^\n]+& ProgramCostSettings/)
  expect(source).toMatch(/export type CreateAccountRequest = [^\n]+& ProgramCostSettings/)
  expect(source).toContain('"RESET_LIMIT"')
})

it("keeps plan and reset reason codes aligned with OpenAPI", () => {
  const schemas = JSON.parse(specText).components.schemas
  expect(source).toContain("export type PlanRuleReason = PlanReason")
  for (const name of ["PlanReason", "ResetReason"]) {
    const declared = new RegExp(`export type ${name} = ([^\n]+)`).exec(source)![1]!
    expect([...declared.matchAll(/"([A-Z_]+)"/g)].map((match) => match[1]).sort())
      .toEqual([...schemas[name].enum].sort())
  }
})

it("accepts verification backtests with contract scaling and exact program fees", () => {
  const request: BacktestStart = { playbook: "sample", scenarios: 1, seed: "0", plan: { initial_cash: "50000",
    rules: { phase: "verification", profit_target: "2500", time_limit_days: 30,
      scaling: [{ profit: "0.00", contracts: 2 }], size_scaling: null,
      evaluation_fee: "100.000001", reset_fee: "25.000002", activation_fee: "50.000003", max_resets: 2 } } }
  expect(request.plan).toHaveProperty("rules.phase", "verification")
  const rules = JSON.parse(specText).components.schemas.BacktestRequest.properties.plan.oneOf[1].properties.rules.properties
  expect(wireType(rules.phase)).toBe(normalize('"evaluation" | "verification" | "funded"'))
  for (const key of ["evaluation_fee", "reset_fee", "activation_fee"]) expect(wireType(rules[key])).toBe("string")
  expect(wireType(rules.max_resets)).toBe("integer")
})

it("describes joint requests and reports while retaining single schemas", () => {
  const schemas = JSON.parse(specText).components.schemas
  const request: BacktestStart = { playbooks: ["alpha@2", "beta"], plan: "eod-50k", scenarios: 2, seed: "1" }
  expect(request.playbooks).toHaveLength(2)
  expect(schemas.BacktestRequest.required).not.toContain("playbook")
  expect(schemas.BacktestRequest.properties.playbooks).toMatchObject({ minItems: 2, maxItems: 8, uniqueItems: true })
  expect(schemas.BacktestRequest.allOf[1].oneOf).toEqual([
    { required: ["playbook"], not: { required: ["playbooks"] } },
    { required: ["playbooks"], not: { required: ["playbook"] } },
  ])
  expect(schemas.BacktestReport.properties.schema.enum).toEqual([1, 2, 3])
  expect(schemas.BacktestReport.properties.mode.const).toBe("joint")
  expect(schemas.BacktestReport.oneOf[1].required).toEqual(["mode", "playbooks"])
  for (const name of ["BacktestResult", "BacktestSummary"]) {
    expect(schemas[name].properties.per_playbook.additionalProperties.$ref).toBe("#/components/schemas/BacktestPlaybookBreakdown")
  }
  expect(schemas.BacktestState.properties.playbooks.items.required).toEqual(["id", "version", "name"])
  expect(schemas.BacktestComparison.properties.combined.properties.includes_joint.const).toBe(true)
  expect(schemas.BacktestResult.properties.rule_trips.items.properties.playbook.type).toBe("string")
})
