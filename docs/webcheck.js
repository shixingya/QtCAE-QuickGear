// 网页端口径体检（开发用，发布前跑一次）
//
//   node docs/webcheck.js                 # 从仓库根目录跑
//   node docs/webcheck.js docs/index.html # 也可以显式指定路径
//
// 检查三件光靠肉眼看代码看不出的事：
//   1) 内联脚本语法与顶层执行没问题（改完 JS 没打开浏览器就发版，是事故的常见来源）
//   2) estimateResults 与客户端 MockSolverBackend 是同一套数值口径
//      （同一个参数在网页上给 n=2.92、客户端给 3.33，用户截图一对就知道不可信）
//   3) fallbackFingerprint 的 Math.imul 改造仍产出合法 32 位 hex，
//      且 formatMachineCode 能切出完整 8 组（带负号的段会被正则吃掉）
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2] || 'docs/index.html';
const html = fs.readFileSync(htmlPath, 'utf8');
const blocks = [...html.matchAll(/<script(?![^>]*src)[^>]*>([\s\S]*?)<\/script>/g)];
if (!blocks.length) {
  console.log('FAIL no-inline-script');
  process.exit(1);
}
const src = blocks[0][1].replace(/\nboot\(\);\s*$/, '\n');

const mkEl = () => new Proxy({}, {
  get(t, k) {
    if (k in t) return t[k];
    if (k === 'classList') return { toggle() {}, add() {}, remove() {}, contains() { return false; } };
    if (k === 'style') return {};
    if (k === 'children') return [];
    if (k === 'childElementCount') return 10;
    return () => {};
  },
  set(t, k, v) { t[k] = v; return true; }
});

const doc = {
  getElementById: () => mkEl(),
  querySelectorAll: () => [],
  createElement: () => mkEl(),
  addEventListener() {},
  body: { style: {} },
  hidden: false,
  activeElement: null
};

const ctx = {
  document: doc,
  window: {},
  navigator: {
    userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36',
    language: 'zh-CN',
    hardwareConcurrency: 8,
    deviceMemory: 8
  },
  screen: { width: 1920, height: 1080, colorDepth: 24 },
  localStorage: { getItem: () => null, setItem() {} },
  console,
  setTimeout: () => 0,
  clearTimeout() {},
  requestAnimationFrame: () => 0,
  cancelAnimationFrame() {},
  ResizeObserver: undefined,
  Date, Math, Number, String, JSON, Promise, Error, Float32Array, Map, Set,
  Object, Array, isNaN, parseInt, parseFloat
};
ctx.globalThis = ctx;
vm.createContext(ctx);

try {
  new vm.Script(src, { filename: htmlPath });
} catch (e) {
  console.log('FAIL syntax :: ' + e.message);
  process.exit(1);
}
console.log('PASS syntax :: 内联脚本可解析');

try {
  vm.runInContext(src, ctx);
} catch (e) {
  console.log('FAIL bootstrap :: ' + e.message);
  process.exit(1);
}
console.log('PASS bootstrap :: 顶层执行无异常');

// 客户端那一侧：MockSolverBackend::solve，量程 320，许用 400（默认材料 20CrMnTi）
const client = (z, m, T) => {
  const raw = 12 * T / Math.pow(m, 3) * Math.sqrt(24 / z);
  const peak = Math.min(raw, 640);            // 截断线 = 量程的两倍
  return {
    sigma: peak,
    disp: peak * 0.09,
    elems: Math.trunc(18000 * Math.pow(z / 24, 1.15) * Math.pow(2 / m, 1.6)),
    sf: 400 / peak
  };
};

const grid = [[24, 2, 80], [24, 2, 200], [40, 1, 300], [20, 4, 20], [60, 3, 150]];
let bad = 0;
for (const [z, m, T] of grid) {
  vm.runInContext(`state.z=${z};state.m=${m};state.T=${T};`, ctx);
  const web = vm.runInContext('estimateResults()', ctx);
  const ref = client(z, m, T);
  const same = Math.abs(web.sigma - ref.sigma) < 1e-9
    && Math.abs(web.sf - ref.sf) < 1e-9
    && Math.abs(web.disp - ref.disp) < 1e-9
    && web.elems === ref.elems;
  if (!same) ++bad;
  console.log(`${same ? 'PASS' : 'FAIL'} parity z=${z} m=${m} T=${T} :: `
    + `web σ=${web.sigma.toFixed(2)} n=${web.sf.toFixed(2)} δ=${web.disp.toFixed(2)} elems=${web.elems} | `
    + `客户端 σ=${ref.sigma.toFixed(2)} n=${ref.sf.toFixed(2)} δ=${ref.disp.toFixed(2)} elems=${ref.elems}`);
}

const fp = vm.runInContext('fallbackFingerprint()', ctx);
const mc = vm.runInContext('formatMachineCode(fallbackFingerprint())', ctx);
const fpOk = /^[0-9a-f]{32}$/.test(fp) && /^[0-9A-F]{4}(-[0-9A-F]{4}){7}$/.test(mc);
if (!fpOk) ++bad;
console.log(`${fpOk ? 'PASS' : 'FAIL'} fingerprint :: hex=${fp} 机器码=${mc}`);

// 额度耗尽的自动引导必须带 exhausted 语义，否则弹窗顶着"本机设备码"的标题
const modalOk = /setTimeout\(\(\) => openModal\('exhausted'\), 420\)/.test(html);
if (!modalOk) ++bad;
console.log(`${modalOk ? 'PASS' : 'FAIL'} modal-copy :: 用完后的自动弹窗带 exhausted 语义`);

console.log(bad === 0 ? 'RESULT: ALL PASS' : `RESULT: ${bad} FAILED`);
process.exit(bad === 0 ? 0 : 1);
