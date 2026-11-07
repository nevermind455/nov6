// Fake Polymarket + Coinbase venue for end-to-end paper testing of sniper-cpp.
// Development only: the bot never loads this. Every host the bot talks to is
// served locally behind an HTTP CONNECT proxy; see README.md here.
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
  sigma: 1e-4, // per sqrt(second)
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
function marketServer(port) {
  const srv = https.createServer(tlsOpts);
  const wss = new WebSocketServer({ server: srv, path: '/ws/market' });
  wss.on('connection', (ws) => {
    const c = { ws, assets: new Set() };
    marketClients.add(c); count('market_ws_connect');
    ws.on('message', (data) => {
      const s = data.toString();
      if (s === 'PING') { ws.send('PONG'); count('market_ping'); return; }
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
  setTimeout(tickLoop, 40 + Math.random() * 120);
}
setInterval(() => {
  const raw = JSON.stringify({ type: 'heartbeat', last_trade_id: tradeId, product_id: 'BTC-USD', sequence: seq, time: iso(Date.now()) });
  for (const ws of cbClients) ws.send(raw);
}, 1000);
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
