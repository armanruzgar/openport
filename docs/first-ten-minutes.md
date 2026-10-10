# Your first ten minutes

This walk-through starts a simulated trading day, takes a prop-style evaluation on it,
passes the evaluation with one trade and reads that trade in the journal. It needs no
market data, no key and no open market: the demo market generates its prices on your
machine. Everything is simulated, and a pass on generated prices is practice.

## 1. Start OpenPort

With Docker:

```bash
docker run --rm --name openport -p 127.0.0.1:8080:8080 -v openport:/var/lib/openport ghcr.io/38st/openport
```

With Homebrew on an Apple Silicon Mac, follow the [quick start](../README.md#quick-start)
and find the link in `$(brew --prefix)/var/log/openport.log`.

Open the `http://localhost:8080/#token=…` link it prints **in a new browser tab**. The
link saves the write token for that tab. Without it the terminal is watch-only and
shows **Enter write token**.

On a first visit, **Welcome to openport** opens. Choose **Practice library**, or close
it and pick **Replay** under System in the sidebar.

## 2. Set up a practice day

On the Replay page, fill in **Start controls**:

- **Plan**: `End-of-day 25K`. The account starts with $25,000. It passes when equity
  reaches $28,000 (12%) and fails if equity touches the floor, $1,500 below the start.
  The floor moves up only at each day's close, and the plan has no minimum days, so
  one day can pass it.
- **Start at (New York)**: `14:25`.
- **Scenario seed**: `Scenario’s seed`, so your prices match this guide.
- Tick **Pause at start**.

Then, under **Demo market**, click **Start** on **Afternoon waterfall**. After a few
seconds of "Preparing start state…", the banner reads `Wed, Sep 16, 14:25:00 ET` and
`paused`.

The replay has its own paper account, separate from your main one. The sidebar marks
it `REPLAY` and shows the plan's badge (`ACTIVE`), a **Target** meter and a
**Floor buffer** meter. The scenario's goal is to stay above the floor through a
selloff that starts at 14:30. You know it is coming. The point here is to learn the
screens, not to predict the market.

## 3. Place a trade

1. Open **Trade** (or press `2`) and click **SPX** in the header. The **Chain** tab
   opens on the same-day Sep 16 expiry. SPX is near 5985.
2. Calls are on the left, puts on the right and strikes in the middle. Click the put
   **Ask** on the 5975 row, `2.60`. The **Order ticket** opens on an SPX long put:
   Buy, Limit, at the ask.
3. Read the **Simulated order preview**, then click **Size to floor**. It sets the
   quantity to what fits half your room above the floor: 2 contracts. A long put
   can lose at most its premium, here $521.30 with fees. That is less than the $1,500
   buffer, so this trade alone cannot fail the attempt.
4. Click **Buy 2 Long Put @ $2.60**. The ticket shows `2/2 filled`.

The **auto-close** countdown above the chain is a plan rule: it closes positions
five minutes before expiry.

## 4. Let the day play

On **Replay**, under **Now replaying**, choose `60×` (a market minute each second)
and click **Resume**. Then open **Dashboard** (`1`).

As SPX falls, the Target meter fills. At 14:34 ET, equity reaches the target. The
badge turns `PASSED` and **Evaluation passed** appears. The simulator then closes every
position at the bid or ask. That close can leave equity a little below $28,000. The
pass still stands, because it was decided when equity touched the target. After a
pass, only closing orders are allowed. **Start a new attempt** begins another.

If you chose a different trade and failed, start **Afternoon waterfall** again from
Replay with the same settings.

## 5. Read the journal

Open **Journal** (`5`). The tiles sum up the attempt: net P&L of about +$3,000, a 100%
win rate and a hold of under ten minutes. The **P&L calendar** marks Sep 16.

Under **Trade history**, click the `SPX Sep 16 5975P` row (`liquidated: target`):

- **Fills** show each fill's price, the quote and displayed size at that moment, and
  who acted. The closing fill is the simulator's profit-target liquidation, not your
  order.
- **Entry context** and **Exit context** record the underlying, IV, delta, equity and
  room above the floor when you opened and when the trade closed.
- **Trade excursions** show the worst (MAE) and best (MFE) marked P&L during the trade,
  and the give-back from the best mark to the close.
- **P&L by Greek** splits the result into delta, gamma, vega, theta and costs.

**Download trades CSV** and **Download fills CSV** export the same records.

## Next

- Click **Back to live** in the banner to return to your main account. The finished
  run stays in the sidebar's account list as a replay.
- Choose **Fresh seed** to play another path of the same day, or try another of the
  twenty [scenarios](scenarios.md).
- Try a harder plan. Static plans need four trading days, and Locking plans cap the
  best day at half the profit. [Plans and rules](paper-trading.md#account-rules-and-evaluations).
- Connect your own data: [providers and configuration](configuration.md).
- [Features](features.md) lists everything the terminal and simulator do.
