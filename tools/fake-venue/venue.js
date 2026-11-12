// Fake Polymarket + Coinbase venue for end-to-end testing of sniper-cpp.
// Development only: the bot never loads this. Every host the bot talks to is
// served locally behind an HTTP CONNECT proxy; see README.md here. With
// VENUE_LIVE=1 it also plays the account side (orders, cancels, the private
// channel, trades, balance), so `--live` can be exercised with no real venue.
'use strict';
const fs = require('fs');
const net = require('net');
const https = require('https');
const http2 = require('http2');
const { WebSocketServer } = require('ws');
const { keccak_256 } = require('@noble/hashes/sha3');
const { twap60 } = require('./oracle');

const env = (k, d) => (process.env[k] === undefined ? d : process.env[k]);
const CFG = {
  proxyPort: +env('VENUE_PROXY_PORT', 18080),
  portBase: +env('VENUE_PORT_BASE', 19000),
  restTs: env('VENUE_REST_TS', 'lastchange'), // lastchange | now
  mmLagMean: +env('VENUE_MM_LAG_MEAN', 1.0), // seconds between MM requotes
  jumpRate: +env('VENUE_JUMP_RATE', 0.08), // jumps per second
  shuffle: env('VENUE_SHUFFLE', '1') === '1', // shuffle change order in one message
  snapshotEvery: +env('VENUE_BOOK_SNAPSHOT_S', 0), // periodic WS book events (0 = never)
  resolveDelay: +env('VENUE_RESOLVE_DELAY_S', 20),
  gammaStallAfter: +env('VENUE_GAMMA_STALL_AFTER_S', 0), // 0 = never stall
  gammaStallMs: +env('VENUE_GAMMA_STALL_MS', 12000),
  proxyDropEvery: +env('VENUE_PROXY_DROP_EVERY_S', 0), // cut every tunnel (0 = never)
  live: env('VENUE_LIVE', '0') === '1', // accept orders (never without it)
  fak: env('VENUE_FAK', 'miss'), // miss | book: what a FAK does
  orderDelayMs: +env('VENUE_ORDER_DELAY_MS', 300), // the taker hold before a FAK's reply
  restFillRate: +env('VENUE_REST_FILL_RATE', 0.05), // sellers hitting a resting bid, per second
  restPartial: +env('VENUE_REST_PARTIAL', 0.3), // share of those fills that are partial
  cancelFail: +env('VENUE_CANCEL_FAIL', 0), // share of cancels answered with HTTP 500
  quietUser: env('VENUE_QUIET_USER', '0') === '1', // send no order events on the private channel
  balance: +env('VENUE_BALANCE', 100), // dollars
  ptb: env('VENUE_PTB', '1') === '1', // publish the price to beat in Gamma
  marketKillEvery: +env('VENUE_MARKET_KILL_EVERY_S', 0), // close ONE market websocket this often (0 = never)
  noPong: env('VENUE_NO_PONG', '0') === '1', // never answer the text PING on the market channel
  marketFlap: env('VENUE_MARKET_FLAP', '0') === '1', // close every market websocket 100 ms after it connects
  sigma: +env('VENUE_SIGMA', 1e-4), // BTC volatility per sqrt(second)
  log: env('VENUE_LOG', __dirname + '/venue.log'),
};
const logStream = fs.createWriteStream(CFG.log, { flags: 'a' });
const log = (...a) => logStream.write(new Date().toISOString() + ' ' + a.join(' ') + '\n');
const counters = {};
const count = (k) => (counters[k] = (counters[k] || 0) + 1);
setInterval(() => log('COUNTERS', JSON.stringify(counters)), 15000);

const tlsOpts = {
  key: fs.readFileSync(__dirname + '/certs/server.key'),
  cert: fs.readFileSync(__dirname + '/certs/server.crt'),
};
const hex = (b) => Buffer.from(b).toString('hex');
const kec = (data) => keccak_256(typeof data === 'string' ? Buffer.from(data) : data);
const USDCE = Buffer.from('2791Bca1f2de4661ED88A30C99A7a9449Aa84174', 'hex');
const bigFromBytes = (b) => BigInt('0x' + hex(b)).toString(10);

// ---------------------------------------------------------------- BTC process
let btc = 100000, seq = 1000000, tradeId = 5000;
const priceAt = new Map(); // unix second -> last price in that second
const initialSecond = Math.floor(Date.now() / 1000);
for (let second = initialSecond - 400; second < initialSecond; ++second)
  priceAt.set(second, btc); // Synthetic flat prehistory for the first round.
function normal() {
  let u = 0, v = 0;
  while (u === 0) u = Math.random();
  while (v === 0) v = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}
let lastStep = Date.now() / 1000;
function stepBtc() {
  const now = Date.now() / 1000, dt = Math.max(1e-3, now - lastStep);
  lastStep = now;
  let r = CFG.sigma * Math.sqrt(dt) * normal();
  if (Math.random() < CFG.jumpRate * dt) {
    const j = (2 + Math.random() * 3) * 1e-4 * (Math.random() < 0.5 ? -1 : 1);
    r += j;
    count('btc_jump');
  }
  btc *= Math.exp(r);
  priceAt.set(Math.floor(now), btc);
  if (priceAt.size > 4000) priceAt.delete(priceAt.keys().next().value);
}
function priceAtSecond(s) {
  for (let k = s; k > s - 30; --k) if (priceAt.has(k)) return priceAt.get(k);
  return btc;
}

// ---------------------------------------------------------------- rounds
const rounds = new Map(); // start -> round
function roundFor(start) {
  if (rounds.has(start)) return rounds.get(start);
  const cid = '0x' + hex(kec('cond' + start));
  const c0 = kec('coll0' + start), c1 = kec('coll1' + start);
  const up = bigFromBytes(kec(Buffer.concat([USDCE, Buffer.from(c0)])));
  const down = bigFromBytes(kec(Buffer.concat([USDCE, Buffer.from(c1)])));
  const r = { start, end: start + 300, cid, c0: hex(c0), c1: hex(c1), up, down,
    strike: null, mmFair: 0.5, nextQuote: 0, books: {}, lastChange: Date.now() };
  r.books[up] = { bids: new Map(), asks: new Map() };
  r.books[down] = { bids: new Map(), asks: new Map() };
  rounds.set(start, r);
  for (const k of [...rounds.keys()]) if (k < start - 3600) {
    const expired = rounds.get(k);
    tokenIndex.delete(expired.up); tokenIndex.delete(expired.down);
    rounds.delete(k);
  }
  return r;
}
const tokenIndex = new Map();
function byToken(t) {
  if (tokenIndex.has(t)) return tokenIndex.get(t);
  for (const r of rounds.values()) if (r.up === t || r.down === t) { tokenIndex.set(t, r); return r; }
  return null;
}
const nowStart = () => Math.floor(Date.now() / 1000 / 300) * 300;
function ensureRounds() { const s = nowStart(); for (const t of [s - 300, s, s + 300]) roundFor(t); }
ensureRounds();
const Phi = (z) => 0.5 * (1 + erf(z / Math.SQRT2));
function erf(x) { // Abramowitz-Stegun 7.1.26
  const s = Math.sign(x); x = Math.abs(x);
  const t = 1 / (1 + 0.3275911 * x);
  const y = 1 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * Math.exp(-x * x);
  return s * y;
}
function theo(r, nowS) {
  if (r.strike === null) return 0.5;
  const tau = Math.max(1, r.end - nowS);
  return Phi(Math.log(btc / r.strike) / (CFG.sigma * Math.sqrt(tau)));
}
const fmtP = (p) => (Math.round(p * 100) / 100).toFixed(2).replace(/^0\./, '0.');
function quoteLevels(m) {
  // UP book around fair m: 5 levels each side, 1 tick apart.
  let bid = Math.floor((m - 0.01) * 100) / 100, ask = Math.ceil((m + 0.01) * 100) / 100;
  bid = Math.min(Math.max(bid, 0.01), 0.97); ask = Math.max(Math.min(ask, 0.99), bid + 0.01);
  const bids = new Map(), asks = new Map();
  for (let i = 0; i < 5; ++i) {
    const b = Math.round((bid - i * 0.01) * 100) / 100, a = Math.round((ask + i * 0.01) * 100) / 100;
    if (b >= 0.01) bids.set(fmtP(b), String(20 + 37 * i + Math.floor(Math.random() * 40)));
    if (a <= 0.99) asks.set(fmtP(a), String(20 + 41 * i + Math.floor(Math.random() * 40)));
  }
  return { bids, asks };
}
function mirror(b) {
  const bids = new Map(), asks = new Map();
  for (const [p, s] of b.asks) bids.set(fmtP(1 - +p), s);
  for (const [p, s] of b.bids) asks.set(fmtP(1 - +p), s);
  return { bids, asks };
}
function diff(asset, oldB, newB, out) {
  for (const [side, key] of [['BUY', 'bids'], ['SELL', 'asks']]) {
    for (const [p, s] of newB[key]) if (oldB[key].get(p) !== s) out.push({ asset_id: asset, price: p, size: s, side });
    for (const [p] of oldB[key]) if (!newB[key].has(p)) out.push({ asset_id: asset, price: p, size: '0', side });
  }
}
const bestOf = (b) => ({
  bid: [...b.bids.keys()].map(Number).reduce((a, x) => Math.max(a, x), 0),
  ask: [...b.asks.keys()].map(Number).reduce((a, x) => Math.min(a, x), 1),
});
function bookJson(r, token, ts) {
  const b = r.books[token];
  const bids = [...b.bids].map(([price, size]) => ({ price, size })).sort((x, y) => +x.price - +y.price);
  const asks = [...b.asks].map(([price, size]) => ({ price, size })).sort((x, y) => +y.price - +x.price);
  return { market: r.cid, asset_id: token, timestamp: String(ts), hash: hex(kec(JSON.stringify([bids, asks]))).slice(0, 40), bids, asks };
}
function requote(r, nowMs, force) {
  const nowS = nowMs / 1000;
  if (r.strike === null && nowS >= r.start) r.strike = twap60(priceAt, r.start);
  if (!force && nowS < r.nextQuote) return;
  r.nextQuote = nowS + (-Math.log(1 - Math.random()) * CFG.mmLagMean);
  r.mmFair = Math.min(0.985, Math.max(0.015, theo(r, nowS)));
  const upB = quoteLevels(r.mmFair), dnB = mirror(upB);
  const changes = [];
  diff(r.up, r.books[r.up], upB, changes);
  diff(r.down, r.books[r.down], dnB, changes);
  r.books[r.up] = upB; r.books[r.down] = dnB;
  if (!changes.length) return;
  r.lastChange = nowMs;
  if (CFG.shuffle) for (let i = changes.length - 1; i > 0; --i) { const j = Math.floor(Math.random() * (i + 1)); [changes[i], changes[j]] = [changes[j], changes[i]]; }
  else changes.sort((a, b) => (a.size === '0' ? 0 : 1) - (b.size === '0' ? 0 : 1)); // removals first: never transiently crossed
  const bu = bestOf(upB), bd = bestOf(dnB);
  for (const c of changes) {
    const b = c.asset_id === r.up ? bu : bd;
    c.hash = hex(kec(JSON.stringify(c))).slice(0, 40); c.best_bid = String(b.bid); c.best_ask = String(b.ask);
  }
  broadcastMarket(r, { market: r.cid, price_changes: changes, timestamp: String(nowMs), event_type: 'price_change' });
  count('price_change');
}

// ---------------------------------------------------------------- market WS
const marketClients = new Set();
function broadcastMarket(r, msg) {
  const raw = JSON.stringify(msg);
  for (const c of marketClients) if (c.assets.has(r.up) || c.assets.has(r.down)) c.ws.send(raw);
}
function sendBooks(c, assets) {
  const out = [];
  for (const a of assets) {
    const r = byToken(a);
    if (!r) { count('ws_unknown_asset'); continue; }
    out.push({ event_type: 'book', ...bookJson(r, a, r.lastChange) });
  }
  if (out.length) c.ws.send(JSON.stringify(out));
}
// ---------------------------------------------------------------- private (user) WS
const userClients = new Set();
function sendUser(market, msg) {
  const raw = JSON.stringify(msg);
  for (const c of userClients) if (c.markets.has(market)) { c.ws.send(raw); count('user_event ' + msg.event_type + (msg.type ? ' ' + msg.type : '')); }
}
function marketServer(port) {
  const srv = https.createServer(tlsOpts);
  const wss = new WebSocketServer({ noServer: true });
  const userWss = new WebSocketServer({ noServer: true });
  srv.on('upgrade', (req, socket, head) => {
    const path = new URL(req.url, 'https://x').pathname;
    const target = path === '/ws/market' ? wss : path === '/ws/user' && CFG.live ? userWss : null;
    if (!target) { socket.destroy(); return; }
    target.handleUpgrade(req, socket, head, (ws) => target.emit('connection', ws, req));
  });
  userWss.on('connection', (ws) => {
    const c = { ws, markets: new Set() };
    userClients.add(c); count('user_ws_connect');
    ws.on('message', (data) => {
      const s = data.toString();
      if (s === 'PING') { ws.send('PONG'); return; }
      let j; try { j = JSON.parse(s); } catch { count('user_bad'); return; }
      const ids = (j.markets || []).map(String);
      if (j.operation === 'unsubscribe') for (const m of ids) c.markets.delete(m);
      else for (const m of ids) c.markets.add(m);
      log('USER_SUB', j.operation || 'initial', ids.length);
    });
    ws.on('close', () => userClients.delete(c));
    ws.on('error', () => userClients.delete(c));
  });
  wss.on('connection', (ws) => {
    const c = { ws, assets: new Set(), since: Date.now() };
    marketClients.add(c); count('market_ws_connect'); log('MARKET_WS_CONNECT', marketClients.size);
    if (CFG.marketFlap) setTimeout(() => ws.close(), 100);
    ws.on('message', (data) => {
      const s = data.toString();
      if (s === 'PING') { count('market_ping'); if (!CFG.noPong) ws.send('PONG'); return; }
      let j; try { j = JSON.parse(s); } catch { ws.send('INVALID OPERATION'); count('market_bad'); return; }
      const ids = (j.assets_ids || []).map(String);
      log('MARKET_SUB', s.slice(0, 300));
      if (j.operation === 'unsubscribe') { for (const a of ids) c.assets.delete(a); count('market_unsub'); return; }
      const add = ids.filter((a) => !c.assets.has(a));
      for (const a of ids) c.assets.add(a);
      count(j.operation === 'subscribe' ? 'market_dyn_sub' : 'market_initial_sub');
      sendBooks(c, j.operation === 'subscribe' ? ids : add.length ? add : ids);
    });
    ws.on('close', () => marketClients.delete(c));
    ws.on('error', () => marketClients.delete(c));
  });
  srv.listen(port, '127.0.0.1');
}

// ---------------------------------------------------------------- account (VENUE_LIVE=1)
// One account. A FAK misses (or takes the book); a post-only GTC rests until
// it is cancelled, a seller hits it, or the market maker's ask comes down to
// it. Order ids are the order's EIP-712 hash, as on the real venue.
const EXCHANGE = '0xE111180000d2663C0091e4f400237545B87B996B';
const NEG_EXCHANGE = '0xe2222d279d744050d28e00520010520000310F59';
const w32 = (n) => Buffer.from(BigInt(n).toString(16).padStart(64, '0'), 'hex');
const raw32 = (h) => Buffer.from(String(h).replace(/^0x/, '').padStart(64, '0'), 'hex');
const ORDER_TYPE = Buffer.from(kec('Order(uint256 salt,address maker,address signer,uint256 tokenId,uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,uint256 timestamp,bytes32 metadata,bytes32 builder)'));
const ORDER_DOMAIN = Buffer.from(kec(Buffer.concat([
  Buffer.from(kec('EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)')),
  Buffer.from(kec('Polymarket CTF Exchange')), Buffer.from(kec('2')), w32(137), raw32(EXCHANGE)])));
function orderHash(o) {
  const struct = kec(Buffer.concat([ORDER_TYPE, w32(o.salt), raw32(o.maker), raw32(o.signer), w32(o.tokenId),
    w32(o.makerAmount), w32(o.takerAmount), w32(o.side === 'BUY' ? 0 : 1), w32(o.signatureType),
    w32(o.timestamp), raw32(o.metadata), raw32(o.builder)]));
  return '0x' + hex(kec(Buffer.concat([Buffer.from([0x19, 0x01]), ORDER_DOMAIN, Buffer.from(struct)])));
}
const account = { balance: Math.round(CFG.balance * 1e6), orders: new Map(), trades: [], nextTrade: 1 };
const sh = (n) => String(Math.round(n * 1e6) / 1e6);
function orderJson(o) {
  return { id: o.id, status: o.status, owner: o.owner, maker_address: o.maker, market: o.market, asset_id: o.asset_id,
    side: 'BUY', original_size: sh(o.size), size_matched: sh(o.matched), price: fmtP(o.price), outcome: o.outcome,
    order_type: o.type, created_at: Math.floor(o.created / 1000), expiration: '0', associate_trades: o.trades };
}
function orderEvent(o, type) {
  if (CFG.quietUser) return;
  sendUser(o.market, { event_type: 'order', type, id: o.id, market: o.market, asset_id: o.asset_id, side: 'BUY',
    price: fmtP(o.price), original_size: sh(o.size), size_matched: sh(o.matched), outcome: o.outcome,
    owner: o.owner, order_owner: o.owner, status: o.status, timestamp: String(Date.now()) });
}
function tradeEvent(t) { sendUser(t.market, { event_type: 'trade', type: 'TRADE', ...t, last_update: String(Math.floor(Date.now() / 1000)), timestamp: String(Date.now()) }); }
// The trade's life on the real venue: MATCHED, then MINED, then CONFIRMED.
function settleTrade(t, cost) {
  tradeEvent(t);
  setTimeout(() => { t.status = 'MINED'; tradeEvent(t); }, 700);
  setTimeout(() => { t.status = 'CONFIRMED'; account.balance -= cost; tradeEvent(t); }, 1500);
}
function fillResting(o, qty, why) {
  qty = Math.min(qty, o.size - o.matched);
  if (qty <= 0 || o.status !== 'LIVE') return;
  o.matched = Math.round((o.matched + qty) * 1e6) / 1e6;
  if (o.matched >= o.size - 1e-9) o.status = 'MATCHED';
  const t = { id: 'trade-' + account.nextTrade++, taker_order_id: '0x' + hex(kec('taker' + account.nextTrade)),
    market: o.market, asset_id: o.asset_id, side: 'SELL', size: sh(qty), price: fmtP(o.price), status: 'MATCHED',
    outcome: o.outcome, owner: 'someone-else', trade_owner: o.owner, trader_side: 'MAKER',
    match_time: String(Math.floor(Date.now() / 1000)),
    maker_orders: [{ order_id: o.id, asset_id: o.asset_id, matched_amount: sh(qty), price: fmtP(o.price),
      outcome: o.outcome, owner: o.owner, maker_address: o.maker, side: 'BUY' }] };
  o.trades.push(t.id); account.trades.push(t);
  count('rest_fill ' + why); log('REST_FILL', o.id.slice(0, 12), sh(qty), '@', fmtP(o.price), why);
  orderEvent(o, 'UPDATE');
  settleTrade(t, Math.round(qty * o.price * 1e6));
}
let lastRestStep = Date.now();
function stepResting() {
  const now = Date.now(), dt = (now - lastRestStep) / 1000;
  lastRestStep = now;
  for (const o of account.orders.values()) {
    if (o.status !== 'LIVE') continue;
    const r = byToken(o.asset_id);
    if (!r || now / 1000 >= r.end) { o.status = 'CANCELED_MARKET_RESOLVED'; orderEvent(o, 'CANCELLATION'); count('rest_expired'); continue; }
    // the market maker's ask came down to our bid: it takes us (the adverse fill)
    if (bestOf(r.books[o.asset_id]).ask <= o.price + 1e-9) { fillResting(o, o.size - o.matched, 'ask_came_down'); continue; }
    if (Math.random() < CFG.restFillRate * dt)
      fillResting(o, Math.random() < CFG.restPartial ? Math.max(1, Math.floor((o.size - o.matched) / 2)) : o.size - o.matched, 'seller');
  }
}
function readBody(req, done) { let body = ''; req.on('data', (d) => (body += d)); req.on('end', () => done(body)); }
function accountHandler(req, res, url) {
  const path = url.pathname;
  if (path === '/auth/ban-status/closed-only') return jsonReply(res, 200, { closed_only: false }), true;
  if (path === '/balance-allowance')
    return jsonReply(res, 200, { balance: String(account.balance), allowances: { [EXCHANGE]: '1000000000000000', [NEG_EXCHANGE]: '1000000000000000' } }), true;
  if (path === '/data/trades') return jsonReply(res, 200, { data: account.trades, next_cursor: 'LTE=', count: account.trades.length }), true;
  if (path === '/data/orders') {
    const market = url.searchParams.get('market');
    const open = [...account.orders.values()].filter((o) => o.status === 'LIVE' && (!market || o.market === market)).map(orderJson);
    return jsonReply(res, 200, { data: open, next_cursor: 'LTE=', count: open.length }), true;
  }
  let m = path.match(/^\/data\/order\/(0x[0-9a-f]{64})$/);
  if (m) { const o = account.orders.get(m[1]); return jsonReply(res, 200, o ? orderJson(o) : 'null'), true; }
  if (path === '/order' && req.method === 'DELETE') {
    readBody(req, (body) => {
      let id; try { id = JSON.parse(body).orderID; } catch { return jsonReply(res, 400, { error: 'bad json' }); }
      if (Math.random() < CFG.cancelFail) { count('cancel_failed_on_purpose'); return jsonReply(res, 500, { error: 'fake venue: cancel failed' }); }
      const o = account.orders.get(id);
      if (!o || o.status !== 'LIVE') { count('cancel_not_open'); return jsonReply(res, 200, { canceled: [], not_canceled: { [id]: "order can't be found - already canceled or matched" } }); }
      o.status = 'CANCELED'; count('cancel_ok'); log('CANCEL', id.slice(0, 12), 'matched', sh(o.matched), 'rested_ms', Date.now() - o.created);
      jsonReply(res, 200, { canceled: [id], not_canceled: {} });
      orderEvent(o, 'CANCELLATION');
    });
    return true;
  }
  if (path === '/order' && req.method === 'POST') {
    readBody(req, (body) => {
      let j; try { j = JSON.parse(body); } catch { return jsonReply(res, 400, { error: 'bad json' }); }
      const od = j.order, r = byToken(String(od.tokenId));
      if (!r) return jsonReply(res, 400, { error: 'market not found' });
      const id = orderHash(od), size = Number(od.takerAmount) / 1e6, price = Math.round(Number(od.makerAmount) / Number(od.takerAmount) * 1e6) / 1e6;
      const best = bestOf(r.books[String(od.tokenId)]);
      count('order ' + j.orderType + (j.postOnly ? ' postOnly' : ''));
      log('ORDER', j.orderType, j.postOnly ? 'postOnly' : '', id.slice(0, 12), sh(size), '@', price, 'best ask', best.ask);
      if (j.orderType === 'GTC') {
        if (!j.postOnly) return jsonReply(res, 400, { error: 'fake venue: only post-only resting orders' });
        if (size < 5) { count('rest_too_small'); return jsonReply(res, 400, { error: 'order ' + id + ' is invalid. Size (' + size + ') lower than the minimum: 5' }); }
        if (best.ask <= price + 1e-9) { count('rest_crossed'); return jsonReply(res, 400, { error: 'invalid post-only order: order crosses book' }); }
        if ([...account.orders.values()].some((o) => o.status === 'LIVE')) count('SECOND_RESTING_ORDER');
        const o = { id, owner: j.owner, maker: od.maker, market: r.cid, asset_id: String(od.tokenId), outcome: String(od.tokenId) === r.up ? 'Up' : 'Down',
          price, size, matched: 0, status: 'LIVE', type: 'GTC', created: Date.now(), trades: [] };
        account.orders.set(id, o); count('rest_live');
        jsonReply(res, 200, { errorMsg: '', orderID: id, takingAmount: '', makingAmount: '', status: 'live', transactionsHashes: null, success: true });
        orderEvent(o, 'PLACEMENT');
        return;
      }
      // a taker order is held, then matched against the book as it is then
      setTimeout(() => {
        if ([...account.orders.values()].some((o) => o.status === 'LIVE')) count('FAK_WHILE_RESTING');
        const now = bestOf(r.books[String(od.tokenId)]);
        if (CFG.fak !== 'book' || now.ask > price + 1e-9) {
          count('fak_miss');
          return jsonReply(res, 400, { error: 'no orders found to match with FAK order. FAK orders are partially filled or killed if no match is found.' });
        }
        const cost = Math.round(size * now.ask * 1e6);
        const t = { id: 'trade-' + account.nextTrade++, taker_order_id: id, market: r.cid, asset_id: String(od.tokenId), side: 'BUY',
          size: sh(size), price: fmtP(now.ask), status: 'MATCHED', owner: j.owner, trade_owner: j.owner, trader_side: 'TAKER',
          match_time: String(Math.floor(Date.now() / 1000)),
          maker_orders: [{ order_id: '0x' + hex(kec('mm' + account.nextTrade)), asset_id: String(od.tokenId), matched_amount: sh(size), price: fmtP(now.ask), outcome: 'x', owner: 'mm' }] };
        account.trades.push(t); count('fak_fill');
        jsonReply(res, 200, { errorMsg: '', orderID: id, takingAmount: sh(size), makingAmount: sh(cost / 1e6), status: 'matched', success: true });
        settleTrade(t, cost + Math.round(size * 0.07 * now.ask * (1 - now.ask) * 1e6));
      }, CFG.orderDelayMs);
    });
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- Coinbase WS
const cbClients = new Set();
const iso = (ms) => new Date(ms).toISOString().replace(/\.(\d{3})Z$/, (m, d) => '.' + d + String(Math.floor(Math.random() * 1000)).padStart(3, '0') + 'Z');
function coinbaseServer(port) {
  const srv = https.createServer(tlsOpts);
  const wss = new WebSocketServer({ server: srv });
  wss.on('connection', (ws) => {
    count('cb_connect');
    ws.on('message', (data) => {
      const j = JSON.parse(data.toString());
      if (j.type === 'subscribe') {
        ws.send(JSON.stringify({ type: 'subscriptions', channels: [
          { name: 'ticker', product_ids: ['BTC-USD'], account_ids: null },
          { name: 'heartbeat', product_ids: ['BTC-USD'], account_ids: null }] }));
        cbClients.add(ws);
      }
    });
    ws.on('close', () => cbClients.delete(ws));
    ws.on('error', () => cbClients.delete(ws));
  });
  srv.listen(port, '127.0.0.1');
}
function tickLoop() {
  stepBtc();
  const ms = Date.now();
  ++seq; ++tradeId;
  const bid = (Math.floor(btc * 100) / 100), ask = Math.round((bid + 0.01) * 100) / 100;
  const raw = JSON.stringify({ type: 'ticker', sequence: seq, product_id: 'BTC-USD', price: btc.toFixed(2),
    open_24h: '99000.00', volume_24h: '12345.6', low_24h: '98000', high_24h: '101000', volume_30d: '1',
    best_bid: bid.toFixed(2), best_bid_size: '0.5', best_ask: ask.toFixed(2), best_ask_size: '0.4',
    side: Math.random() < 0.5 ? 'buy' : 'sell', time: iso(ms), trade_id: tradeId, last_size: '0.001' });
  for (const ws of cbClients) ws.send(raw);
  count('cb_ticker');
  ensureRounds();
  for (const r of rounds.values()) if (ms / 1000 < r.end + 5 && ms / 1000 > r.start - 600) requote(r, ms, false);
  if (CFG.live) stepResting();
  setTimeout(tickLoop, 40 + Math.random() * 120);
}
setInterval(() => {
  const raw = JSON.stringify({ type: 'heartbeat', last_trade_id: tradeId, product_id: 'BTC-USD', sequence: seq, time: iso(Date.now()) });
  for (const ws of cbClients) ws.send(raw);
}, 1000);
// Single-socket loss: the oldest market connection is cut, cleanly and
// abruptly in turn. A client with a standby should not lose the feed.
let marketKills = 0;
if (CFG.marketKillEvery > 0)
  setInterval(() => {
    const oldest = [...marketClients].sort((a, b) => a.since - b.since)[0];
    if (!oldest) return;
    count('market_ws_killed'); log('MARKET_WS_KILL', marketClients.size);
    if (marketKills++ % 2) oldest.ws.terminate(); else oldest.ws.close();
  }, CFG.marketKillEvery * 1000);
if (CFG.snapshotEvery > 0)
  setInterval(() => {
    for (const c of marketClients) for (const a of c.assets) { const r = byToken(a); if (r) c.ws.send(JSON.stringify({ event_type: 'book', ...bookJson(r, a, r.lastChange) })); }
  }, CFG.snapshotEvery * 1000);

// ---------------------------------------------------------------- REST (gamma, clob h1/h2, rpc)
const isoS = (s) => new Date(s * 1000).toISOString().replace('.000Z', 'Z');
function resolved(r) { return Date.now() / 1000 >= r.end + CFG.resolveDelay; }
function upWon(r) { return twap60(priceAt, r.end) >= (r.strike ?? twap60(priceAt, r.start)); }
function gammaEvent(start) {
  const r = roundFor(start), closed = resolved(r);
  const won = closed ? upWon(r) : null;
  return {
    id: String(start), slug: 'btc-updown-5m-' + start, active: true, closed, endDate: isoS(r.end),
    ...(CFG.ptb && r.strike !== null ? { eventMetadata: { priceToBeat: r.strike } } : {}),
    markets: [{
      id: String(6700000 + (start / 300) % 100000), conditionId: r.cid, slug: 'btc-updown-5m-' + start,
      active: true, closed, acceptingOrders: !closed, enableOrderBook: true, version: 'v1',
      eventStartTime: isoS(start), endDate: isoS(r.end),
      outcomes: '["Up", "Down"]', clobTokenIds: JSON.stringify([r.up, r.down]),
      outcomePrices: closed ? JSON.stringify(won ? ['1', '0'] : ['0', '1']) : '["0.5", "0.5"]',
      umaResolutionStatus: closed ? 'resolved' : '',
      cryptoMarketConfig: { id: 'btc-5m-twap-60', asset: 'btc', duration: '5m', twapEnabled: true, twapLookbackSeconds: 60 },
    }],
  };
}
function jsonReply(res, code, obj) {
  const body = typeof obj === 'string' ? obj : JSON.stringify(obj);
  res.writeHead(code, { 'content-type': 'application/json' });
  res.end(body);
}
function gammaServer(port) {
  https.createServer(tlsOpts, (req, res) => {
    count('gamma ' + req.url.replace(/\d{6,}/g, 'N'));
    const m = req.url.match(/^\/events\/slug\/btc-updown-5m-(\d+)$/);
    if (!m) return jsonReply(res, 404, { error: 'not found' });
    const start = +m[1];
    if (start % 300 || Math.abs(start - nowStart()) > 86400) return jsonReply(res, 404, { error: 'not found' });
    if (CFG.gammaStallAfter > 0 && process.uptime() > CFG.gammaStallAfter) {
      count('gamma_stalled');
      return setTimeout(() => jsonReply(res, 200, gammaEvent(start)), CFG.gammaStallMs);
    }
    jsonReply(res, 200, gammaEvent(start));
  }).listen(port, '127.0.0.1');
}
function clobHandler(req, res) {
  const url = new URL(req.url, 'https://clob.polymarket.com');
  count('clob ' + (req.httpVersion === '2.0' ? 'h2 ' : 'h1 ') + req.method + ' ' + url.pathname.replace(/0x[0-9a-f]{64}/, 'CID'));
  if (url.pathname === '/time') return jsonReply(res, 200, String(Math.floor(Date.now() / 1000)));
  let m = url.pathname.match(/^\/clob-markets\/(0x[0-9a-f]{64})$/);
  if (m) {
    const r = [...rounds.values()].find((x) => x.cid === m[1]);
    if (!r) return jsonReply(res, 404, { error: 'market not found' });
    return jsonReply(res, 200, { r: { mi: 50, ma: 4.5, moas: 30 }, t: [{ t: r.up, o: 'Up' }, { t: r.down, o: 'Down' }], c: r.cid,
      mos: 5, mts: 0.01, mbf: 1000, tbf: 1000, ao: true, itode: true, fd: { r: 0.07, e: 1, to: true }, v: 'v1' });
  }
  if (url.pathname === '/book') {
    const t = url.searchParams.get('token_id'), r = byToken(t);
    if (!r) return jsonReply(res, 404, { error: 'No orderbook exists for the requested token id' });
    const ts = CFG.restTs === 'now' ? Date.now() : r.lastChange;
    return jsonReply(res, 200, { ...bookJson(r, t, ts), min_order_size: '5', tick_size: '0.01', neg_risk: false, last_trade_price: '0.500' });
  }
  m = url.pathname.match(/^\/markets\/(0x[0-9a-f]{64})$/);
  if (m) {
    const r = [...rounds.values()].find((x) => x.cid === m[1]);
    if (!r) return jsonReply(res, 404, { error: 'market not found' });
    const closed = resolved(r), won = closed && upWon(r);
    return jsonReply(res, 200, { condition_id: r.cid, closed, tokens: [
      { token_id: r.up, outcome: 'Up', winner: closed && won }, { token_id: r.down, outcome: 'Down', winner: closed && !won }] });
  }
  if (CFG.live && accountHandler(req, res, url)) return;
  if (url.pathname === '/order') { count('ORDER_POST_SEEN'); return jsonReply(res, 400, { errorMsg: 'fake venue: paper must not post' }); }
  jsonReply(res, 404, { error: 'not found' });
}
function clobServer(port) {
  const srv = http2.createSecureServer({ ...tlsOpts, allowHTTP1: true }, clobHandler);
  srv.listen(port, '127.0.0.1');
}
const SEL = {
  den: hex(kec('payoutDenominator(bytes32)').slice(0, 4)),
  num: hex(kec('payoutNumerators(bytes32,uint256)').slice(0, 4)),
  coll: hex(kec('getCollectionId(bytes32,bytes32,uint256)').slice(0, 4)),
};
const word = (n) => '0x' + BigInt(n).toString(16).padStart(64, '0');
function rpcServer(port) {
  https.createServer(tlsOpts, (req, res) => {
    let body = '';
    req.on('data', (d) => (body += d));
    req.on('end', () => {
      count('rpc');
      let batch; try { batch = JSON.parse(body); } catch { return jsonReply(res, 400, { error: 'bad json' }); }
      const out = batch.map((call) => {
        const data = call.params[0].data.replace(/^0x/, ''), sel = data.slice(0, 8);
        const w = (i) => data.slice(8 + 64 * i, 8 + 64 * (i + 1));
        let cid, r;
        if (sel === SEL.den || sel === SEL.num) cid = '0x' + w(0); else cid = '0x' + w(1);
        r = [...rounds.values()].find((x) => x.cid === cid);
        if (!r) return { jsonrpc: '2.0', id: call.id, result: word(0) };
        const done = resolved(r), won = done && upWon(r);
        if (sel === SEL.den) return { jsonrpc: '2.0', id: call.id, result: word(done ? 1 : 0) };
        if (sel === SEL.num) { const i = Number(BigInt('0x' + w(1))); return { jsonrpc: '2.0', id: call.id, result: word(done ? ((i === 0) === won ? 1 : 0) : 0) }; }
        if (sel === SEL.coll) { const ix = Number(BigInt('0x' + w(2))); return { jsonrpc: '2.0', id: call.id, result: '0x' + (ix === 1 ? r.c0 : r.c1) }; }
        return { jsonrpc: '2.0', id: call.id, error: { code: -32601, message: 'unknown selector' } };
      });
      jsonReply(res, 200, out);
    });
  }).listen(port, '127.0.0.1');
}

// ---------------------------------------------------------------- CONNECT proxy
const B = CFG.portBase;
const ROUTES = {
  'clob.polymarket.com': B + 1, 'gamma-api.polymarket.com': B + 2,
  'ws-subscriptions-clob.polymarket.com': B + 3, 'ws-feed.exchange.coinbase.com': B + 4,
  'polygon-bor-rpc.publicnode.com': B + 5, 'ws-live-data.polymarket.com': B + 6,
};
// Reconnection stress: cut every tunnelled connection (market and Coinbase
// websockets, CLOB HTTP/2, REST) at a fixed interval.
const tunnels = new Set();
if (CFG.proxyDropEvery > 0)
  setInterval(() => {
    count('proxy_drop ' + tunnels.size);
    for (const s of tunnels) s.destroy();
    tunnels.clear();
  }, CFG.proxyDropEvery * 1000);
net.createServer((client) => {
  tunnels.add(client);
  client.on('close', () => tunnels.delete(client));
  let buf = Buffer.alloc(0);
  const onData = (d) => {
    buf = Buffer.concat([buf, d]);
    const end = buf.indexOf('\r\n\r\n');
    if (end < 0) return;
    client.removeListener('data', onData);
    const line = buf.slice(0, end).toString().split('\r\n')[0];
    const m = line.match(/^CONNECT ([^:\s]+):(\d+) HTTP\/1\.[01]$/);
    const port = m && ROUTES[m[1]];
    if (!port) { count('proxy_refused ' + line); client.end('HTTP/1.1 403 Forbidden\r\n\r\n'); return; }
    count('proxy ' + m[1]);
    const up = net.connect(port, '127.0.0.1', () => {
      client.write('HTTP/1.1 200 Connection established\r\n\r\n');
      const rest = buf.slice(end + 4);
      if (rest.length) up.write(rest);
      client.pipe(up); up.pipe(client);
    });
    client.on('close', () => up.destroy());
    up.on('close', () => client.destroy());
    up.on('error', () => client.destroy());
    client.on('error', () => up.destroy());
  };
  client.on('data', onData);
}).listen(CFG.proxyPort, '127.0.0.1');

clobServer(B + 1); gammaServer(B + 2); marketServer(B + 3); coinbaseServer(B + 4); rpcServer(B + 5);
tickLoop();
log('VENUE_UP', JSON.stringify(CFG));
console.log('venue up', JSON.stringify(CFG));
