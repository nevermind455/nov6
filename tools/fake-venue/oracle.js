'use strict';
// Development-only one-second, last-value-held approximation of a 60s TWAP.
// Synthetic prices are not an authenticated Chainlink oracle.
function twap60(prices, end) {
  const boundary = Math.floor(end), start = boundary - 60;
  let last, prior = -Infinity, total = 0;
  for (const [second, price] of prices) {
    if (second <= start && second > prior) { prior = second; last = price; }
  }
  if (!Number.isFinite(last) || last <= 0) throw new Error('TWAP has no initial price');
  for (let second = start; second < boundary; ++second) {
    if (prices.has(second)) last = prices.get(second);
    if (!Number.isFinite(last) || last <= 0) throw new Error('Invalid TWAP sample');
    total += last;
  }
  return total / 60;
}
module.exports = { twap60 };
