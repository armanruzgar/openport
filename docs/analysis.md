# Analysis portfolios

Import the positions you hold at a broker to see them through OpenPort's risk
views on your own feed. An analysis portfolio is **analysis only**: it is not a
paper account, no order, fill, journal or playbook can reach it, and every page and
response labels it so. It never sends anything to a broker.

## Importing

The Analysis page, or `POST /api/analysis/portfolios` with
`{ "name", "csv", "format" }`, imports a CSV of up to 4 MiB (this route's request
limit; every other route keeps 64 KiB) and 2,000 holdings. `format` is `auto` (detected from the columns), `generic`,
`tastytrade`, `schwab` or `ibkr`. Lots of one symbol are summed, with their
quantity-weighted cost when every lot has one; holdings that net to zero, cash,
totals and unsupported instruments (futures, bonds, funds) are left out and listed
as skipped. A file that cannot be read is refused with the line and the reason.

Layouts are read by their column names, case and punctuation ignored. The broker
layouts use those exports' usual column names and are tested against hand-written
samples, not downloaded statements; brokers change their exports, so check the
imported holdings against your statement.

| Format | Columns read | Option symbols |
| --- | --- | --- |
| generic | `symbol`, `quantity`, optional `cost` (or `price`, `average_price`) per share; or `underlying`, `expiry` (YYYY-MM-DD), `strike`, `type` (call/put), `quantity` | OSI, padded or compact |
| tastytrade | `Symbol`, `Type` (equity option, equity), `Quantity`, optional `Exp Date`, `Strike Price`, `Call/Put`, `Underlying Symbol`, `Trade Price` or `Average Open Price`, and a `Direction` for unsigned quantities | OSI, or the expiry, strike and call/put columns |
| schwab | Title lines, then `Symbol`, `Qty (Quantity)`, optional `Cost Basis` (the lot's total) and `Security Type` | `SPY 12/18/2026 600.00 C`, or `SPY 12/18/26 600.00 C` (two-digit years are 20YY) |
| ibkr | An activity statement's `Open Positions` section (summary rows; lots are not added twice), or a Flex query with `AssetClass`, `Symbol`, `Quantity`, optional `UnderlyingSymbol`, `Expiry`, `Strike`, `Put/Call`, `CostBasisPrice` | `SPY 18DEC26 600 C`, OSI, or the expiry, strike and put/call columns |

Short positions are negative quantities. Costs are per share, or per option share
(the option price, not times 100).

## Valuation and risk views

Holdings are valued from the engine's current analytics exactly as paper positions
are: options at their chain mid and smile IV, shares at the underlying's spot. A
holding whose underlying is not subscribed, or whose contract is not in the chain,
expired or has no usable quote, is listed as unpriced and left out of the views,
which then report `complete: false`.

`GET /api/analysis/portfolios/{id}` returns each holding's mark, value, cost,
unrealized P&L and share-equivalent delta, gamma, vega and theta, with dollar Greeks
by underlying. The risk views run on the valued holdings:

- `risk/profile`: P&L curves across moves of one underlying, or of SPY or SPX with
  each underlying beta-weighted, on chosen dates and with an IV offset, using the
  same model as the paper account's [risk profile](paper-trading.md#risk-profile-and-probabilities).
  Underlyings without a beta yet are left out and named.
- `risk/grid`: P&L across spot and volatility shocks, as the paper account's grid.

P&L is measured from the book's value now, not from its cost. Views are listed in
the portfolio's `views`; stress tests and value at risk will attach there as they
arrive.

## Access

Every analysis-portfolio route needs admin, reads included: the holdings are
someone's outside positions, not market data, so they are not public even on a
server whose market reads are. A loopback server that accepts writes without
credentials gives every request admin, so the terminal works there as before;
anywhere else, send an admin token. Read, trade and sandbox tokens get 403.

## Storage

Portfolios are kept as one JSON file each in `analysis/` beside the candle
directory (`~/.openport/analysis` by default). Replay and the demo keep them in
memory. `DELETE /api/analysis/portfolios/{id}` removes one.
