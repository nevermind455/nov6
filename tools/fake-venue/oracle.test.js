'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { twap60 } = require('./oracle');
test('fixture resolution uses 60s window, not final spot tick', () => {
  const prices = new Map();
  for (let second = 0; second < 60; ++second) prices.set(second, 100);
  for (let second = 60; second < 119; ++second) prices.set(second, 200);
  prices.set(119, 1);
  assert.equal(twap60(prices, 60), 100);
  assert.ok(twap60(prices, 120) > twap60(prices, 60));
  assert.ok(prices.get(119) < twap60(prices, 60));
});
test('boundary ticks excluded; absent seconds hold last value', () => {
  const prices = new Map([[-10, 100], [30, 200], [60, 10000]]);
  assert.equal(twap60(prices, 60), 150);
  assert.throws(() => twap60(new Map(), 60));
});
