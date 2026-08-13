# Sessions and expiry

A backtest that runs one day is not a backtest. This describes what happens when
a strategy spans many sessions, and what happens to options that reach expiry.

## What crosses a session boundary

The strategy coroutine and the portfolio outlive a session; only the data is
swapped. A position opened on Monday is still open on Tuesday, and the strategy
is still sitting at whatever it was awaiting.

**Survives:** open positions, attached risk rules, realised P&L, costs, and the
carried mark on every leg.

**Does not survive, by design:**

| | Why |
|---|---|
| Bar series and indicators | They describe one session. Querying one from a later day **throws** rather than answering. |
| Working orders | Exchange orders are day orders. Anything unfilled at the bell is cancelled. |
| Options past their expiry | Settled — see below. |

## Three traps, and how each is closed

These are the ways a multi-session engine quietly corrupts results.

### A held position reading as flat overnight

On the next session's first observations an instrument has not printed yet, so
asking for its quote returns nothing. An engine that values a leg at zero in that
case reports an overnight position as flat — and worse, every percentage stop on
it stops firing.

Every open leg therefore carries a **last mark**, refreshed on each observation
and used whenever no current quote exists. A test opens a position, jumps to the
first observation of the next session, and asserts the P&L is exactly what
yesterday's closing ask implies rather than zero.

### A time of day resolving onto a day that does not trade

`ctx.at("09:20")` means *the next occurrence*. If it has already passed today it
resolves onto the next **trading** session, not the next calendar day, so a
strategy written as a daily loop simply repeats:

```cpp
while (true) {
    co_await ctx.at("09:30");
    auto pos = ctx.sell(ctx.chain().straddle(), 1);
    pos->exit_at("15:00");
    co_await ctx.at("15:20");
}
```

A test runs Thursday through Tuesday and asserts Friday is followed by Monday.

Two related sharp edges are worth knowing. `pos->exit_at("15:00")` means **today**
at 15:00 — for a position carried across sessions that is almost never what is
meant, so `exit_at_on(date, "15:00")` exists. And `ctx.next_expiry()` includes an
expiry falling today, which is the thing a roll is getting *out of*, so
`next_expiry(1)` is what a roll wants.

### A bar series answering for the wrong day

A `BarSeries` fetched on Monday would happily keep returning Monday's bars on
Tuesday — silently, and looking entirely reasonable. Series are bound to the
session they were built from and **throw** when queried past it. Re-acquire them
inside the daily loop.

## Expiry

The default is to **cash settle**, because that is what happens in reality when a
strategy does nothing. Open legs close at intrinsic against the settlement level:

- The settlement level is the underlying's closing print. Failing an index feed,
  it is recovered from the chain by put-call parity — and because the expiring
  series has no time left to invert at the bell, a later series is consulted.
  With no level available at all, the run **fails** rather than settling at a
  guessed number.
- STT on exercise is charged on **intrinsic value** and falls on the buyer. A
  writer being assigned pays none of it. Settlement is not a brokered order, so
  no brokerage or exchange charge applies.

Strike selection works on an options-only feed too: with no spot instrument, the
money is located by the same parity forward.

### Doing something other than settling

There is no menu of expiry actions, because the useful ones are not enumerable.
"Roll" alone could mean same strike, same moneyness, same delta, or a different
structure entirely, and the answer belongs to the strategy. So the engine settles
by default and gets out of the way:

```cpp
// Square off before expiry rather than take settlement.
pos->exit_at_on(expiry, "15:00");

// Or roll: close what is expiring, open further out. Ordinary strategy code.
if (held_expiry && ctx.date() >= add_days(*held_expiry, -1)) {
    held->close();
    held = ctx.sell(ctx.chain(*ctx.next_expiry(2)).straddle(), 1);
}
```

`ExpiryHandling::Forbid` makes holding into expiry an error, for strategies that
must never take settlement risk and would rather fail loudly than find out from
the P&L.

## Two behaviours worth knowing

**A stop that fires but cannot fill by the bell is re-armed.** Its exit order is
cancelled with every other day order, and leaving the rule spent would carry the
position overnight believing it was protected. It fires again on the next
session.

**A position with a cancelled leg is still managed.** If one leg of a spread
never fills, the position is established on the legs that did, so percentage
stops keep working on the surviving holding rather than being silently disabled.

## Known limitations

- If the data has no session on an expiry date, the position settles at the
  **next available session** and therefore at that session's level, not the true
  expiry level. Driven by what is held rather than by the calendar, so nothing is
  left open forever — but the price is a day late.
- Overnight gaps are priced from the next session's first quotes. There is no
  model of what could have been done between sessions, because nothing could.
- Weekly and monthly expiries are treated identically; there is no notion of
  expiry-day special handling beyond settlement.
