(() => {
  const $ = id => document.getElementById(id);
  const root = document.documentElement;
  const tape = $('tape'), tapeStat = $('tape-stat'), tapeHead = $('tape-head'), tapeRows = $('tape-rows'), note = $('tape-note');
  const out = $('out'), form = $('prompt'), input = $('cmd'), hintTape = $('hint-tape'), copyStatus = $('copy-status');
  const themeLabel = $('theme-label'), hero = document.querySelector('.hero');
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const INSTALL = $('install-command').textContent.trim();

  for (const el of document.querySelectorAll('[data-js]')) el.hidden = false;

  /* Theme */
  const systemLight = matchMedia('(prefers-color-scheme: light)');
  const theme = () => root.dataset.theme || (systemLight.matches ? 'light' : 'dark');
  const syncTheme = () => { themeLabel.textContent = theme() === 'light' ? 'Dark mode' : 'Light mode'; };
  function toggleTheme() {
    root.dataset.theme = theme() === 'light' ? 'dark' : 'light';
    try { localStorage.setItem('op-theme', root.dataset.theme); } catch {}
    syncTheme();
  }
  syncTheme();
  systemLight.addEventListener('change', syncTheme);

  /* Tape: simulated prints at a fixed snapshot of real quotes (tape-data.js).
     Each contract starts from Cboe's delayed closing bid and ask and moves with
     its delta and gamma as spot drifts. A buy lifts the ask; a sell hits the bid. */
  let pinned = false, hover = false, stats = () => {};
  const paused = () => hover || pinned;
  const CH = window.CHAINS;
  if (CH) {
    const ROWS = 18;
    const SYMBOLS = [
      { root: 'SPXW', name: 'SPX', weight: 0.45, beta: 1, lot: [1, 20], block: [50, 300] },
      { root: 'SPY', name: 'SPY', weight: 0.35, beta: 1, lot: [1, 60], block: [150, 2000] },
      { root: 'QQQ', name: 'QQQ', weight: 0.2, beta: 1.25, lot: [1, 50], block: [100, 1500] },
    ];
    const MONTHS = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];
    for (const s of SYMBOLS) {
      const c = CH[s.root];
      s.spot0 = c.spot;
      s.prev = c.prev;
      s.x = 0;
      s.quotes = c.quotes.map(([exp, K, cp, bid, ask, iv, delta, gamma, vol]) => ({
        s, exp: exp.slice(4) + MONTHS[+exp.slice(2, 4) - 1], K, put: cp === 'P',
        mid: (bid + ask) / 2, half: (ask - bid) / 2, iv, delta, gamma, w: Math.pow(vol + 20, 0.7),
      }));
      s.total = s.quotes.reduce((a, q) => a + q.w, 0);
    }
    const asof = new Date(CH.SPY.last.slice(0, 10) + 'T12:00:00Z');
    const day = asof.toLocaleDateString('en-GB', { weekday: 'short', timeZone: 'UTC' });
    const date = asof.toLocaleDateString('en-GB', { day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' });
    note.textContent = `# simulated prints at Cboe’s delayed closing quotes, ${day} ${date}. Hover to pause.`;

    // The clock runs through the last minutes of that session; options on all three trade until 16:15 ET.
    const OPEN = (15 * 3600 + 52 * 60) * 1000, END = (16 * 3600 + 14 * 60 + 59) * 1000;
    let r = 0, sim = OPEN, last = performance.now(), prints = 18442, queue = [];
    const gauss = () => Math.sqrt(-2 * Math.log(1 - Math.random())) * Math.cos(2 * Math.PI * Math.random());
    const pad = (s, n) => String(s).padStart(n);
    const spot = s => s.spot0 * (1 + r * s.beta + s.x);
    const tickOf = (s, p) => (s.root === 'SPXW' ? (p < 3 ? 0.05 : 0.1) : 0.01);
    function quote(q) {
      const dS = spot(q.s) - q.s.spot0;
      const mid = Math.max(0.01, q.mid + q.delta * dS + 0.5 * q.gamma * dS * dS);
      const t = tickOf(q.s, mid);
      const bid = Math.max(t, Math.floor((mid - q.half) / t + 1e-9) * t);
      const ask = Math.max(bid + t, Math.ceil((mid + q.half) / t - 1e-9) * t);
      return { bid, ask, t };
    }
    function pick(list, total, weight) {
      let u = Math.random() * total;
      for (const item of list) if ((u -= weight(item)) < 0) return item;
      return list[0];
    }
    function size(s) {
      const u = Math.random(), v = Math.random(), [a, b] = s.lot, [c, d] = s.block;
      return u < 0.6 ? a + (v * (b - a) * 0.3 | 0) : u < 0.95 ? a + (v * (b - a) | 0) : c + (v * (d - c) | 0);
    }
    const print = (q, side, qty, price, cond) => ({ q, side, qty, price, cond });
    // One order is a list of groups; a group prints at one timestamp.
    function order() {
      r += gauss() * 0.00003 - r * 0.02;
      for (const s of SYMBOLS) s.x += (s.root === 'QQQ' ? gauss() * 0.00002 : 0) - s.x * 0.01;
      const s = pick(SYMBOLS, 1, x => x.weight), q = pick(s.quotes, s.total, x => x.w), qt = quote(q);
      const buy = Math.random() < 0.5, side = buy ? 'BUY' : 'SELL', px = buy ? qt.ask : qt.bid, u = Math.random();
      if (s.root === 'SPXW' && u < 0.15) {
        const width = 5 * (1 + (Math.random() * 3 | 0));
        const far = s.quotes.find(o => o.exp === q.exp && o.put === q.put && o.K === q.K + (q.put ? -width : width));
        if (far) {
          const n = 1 + (Math.random() * 30 | 0);
          return [[print(q, 'BUY', n, qt.ask, 'MLET'), print(far, 'SELL', n, quote(far).bid, 'MLET')]];
        }
      }
      if (s.root !== 'SPXW' && u < 0.08) {
        const venues = 2 + (Math.random() * 3 | 0), group = [];
        for (let i = 0; i < venues; i++) group.push(print(q, side, 20 + (Math.random() * 180 | 0), px, 'ISOI'));
        return [group];
      }
      if (u < 0.18) {
        const levels = 2 + (Math.random() * 2 | 0), steps = [];
        for (let i = 0; i < levels; i++) steps.push([print(q, side, size(s), Math.max(qt.t, px + (buy ? i : -i) * qt.t), 'AUTO')]);
        return steps;
      }
      return [[print(q, side, size(s), px, 'AUTO')]];
    }

    const two = n => String(n).padStart(2, '0');
    const clock = ms => [`${two(ms / 3600e3 | 0)}:${two(ms / 60e3 % 60 | 0)}:${two(ms / 1e3 % 60 | 0)}`, '.' + String(ms % 1000 | 0).padStart(3, '0')];
    const money = p => (p < 1e3 ? '$' + Math.round(p) : p < 1e5 ? '$' + (p / 1e3).toFixed(1) + 'K' : p < 1e6 ? '$' + Math.round(p / 1e3) + 'K' : '$' + (p / 1e6).toFixed(2) + 'M');
    function line(cols, cls) {
      const row = document.createElement('div');
      row.className = cls;
      for (const [k, text] of cols) {
        const span = document.createElement('span');
        span.className = k;
        span.textContent = text;
        row.append(span);
      }
      return row;
    }
    function rowFor(p, fresh) {
      const [hms, ms] = clock(p.t), prem = p.qty * p.price * 100, q = p.q;
      const cls = 'row ' + p.side.toLowerCase() + (prem >= 250000 ? ' big' : '') + (fresh ? ' new' : '');
      return line([
        ['t', hms], ['ms', ms],
        ['k', `  ${q.s.root.padEnd(4)} ${q.exp} ${pad(q.K + (q.put ? 'P' : 'C'), 5)}`],
        ['s', '  ' + pad(p.qty, 6)],
        ['p', ' @ ' + pad(p.price.toFixed(2), 6)],
        ['d', '  ' + p.side.padEnd(4)],
        ['v', '  ' + pad((q.iv * 100).toFixed(1) + '%', 6)],
        ['m', '  ' + pad(money(prem), 7)],
        ['c', '  ' + p.cond],
      ], cls);
    }
    tapeHead.append(...line([
      ['t', 'TIME    '], ['ms', '    '], ['k', '  CONTRACT'.padEnd(18)], ['s', '  ' + pad('SIZE', 6)], ['p', '   ' + pad('PRICE', 6)],
      ['d', '  SIDE'], ['v', '  ' + pad('IV', 6)], ['m', '  ' + pad('PREM', 7)], ['c', '  COND'],
    ], 'row').childNodes);

    stats = () => {
      const ticker = s => {
        const p = spot(s), c = (p / s.prev - 1) * 100;
        return `<span><b>${s.name} ${p.toFixed(2)}</b> <span class="${c >= 0 ? 'up' : 'down'}">${c >= 0 ? '+' : ''}${c.toFixed(2)}%</span></span>`;
      };
      tapeStat.innerHTML = SYMBOLS.map(ticker).join('') + (paused() ? '<span>PAUSED</span>' : `<span>PRINTS <b>${prints.toLocaleString('en-US')}</b></span>`);
    };
    let trimTimer = 0;
    function push(group, fresh) {
      const t = Math.floor(sim);
      for (const p of group) {
        p.t = t;
        tapeRows.prepend(rowFor(p, fresh));
        prints += 1;
      }
      if (fresh && !reduce) {
        const h = tapeRows.firstElementChild.getBoundingClientRect().height;
        tapeRows.style.transition = 'none';
        tapeRows.style.transform = `translateY(${-group.length * h}px)`;
        tapeRows.getBoundingClientRect();
        tapeRows.style.transition = 'transform 180ms cubic-bezier(.2, .7, .3, 1)';
        tapeRows.style.transform = '';
      }
      while (tapeRows.children.length > ROWS + 8) tapeRows.lastElementChild.remove();
      clearTimeout(trimTimer);
      trimTimer = setTimeout(() => { while (tapeRows.children.length > ROWS) tapeRows.lastElementChild.remove(); }, 220);
      stats();
    }
    function loop() {
      const now = performance.now();
      let wait = 250;
      if (!paused() && !document.hidden && !tape.hidden) {
        sim += now - last;
        if (sim > END) sim = OPEN;
        if (!queue.length) queue = order();
        push(queue.shift(), true);
        wait = queue.length ? 50 + Math.random() * 40 : Math.min(1600, Math.max(60, -Math.log(1 - Math.random()) * 380));
      }
      last = now;
      setTimeout(loop, wait);
    }
    while (tapeRows.children.length < ROWS) {
      for (const g of order()) { sim += 40 + Math.random() * 600; push(g, false); }
    }
    tape.hidden = note.hidden = false;
    tape.addEventListener('mouseenter', () => { hover = true; stats(); });
    tape.addEventListener('mouseleave', () => { hover = false; stats(); });
    if (!reduce) loop();
  }

  /* Terminal */
  const COMMANDS = {
    help(res) {
      const rows = [
        ['install', 'Docker quick start, no key needed'],
        ['demo', 'Play the 15 s trading demo'],
        ['tape', 'Show the simulated options tape'],
        ['pause', 'Pause or resume the tape'],
        ['bench', 'IV solve and analytics timings'],
        ['docs', 'Architecture, methods and providers'],
        ['github', 'Source code, MIT licence'],
        ['clear', 'Clear the screen   ^L'],
        ['theme', 'Light or dark      ^M'],
      ];
      res.innerHTML = '<dl class="cmds">' + rows.map(([c, d]) => `<div><dt><button type="button" data-cmd="${c}">${c}</button></dt><dd>${d}</dd></div>`).join('') + '</dl>';
    },
    install(res) {
      res.innerHTML = '# Needs Docker. Cboe’s free 15-minute delayed feed, no key.\n'
        + `<code>${INSTALL}</code><button class="copy" type="button" data-copy>copy</button>\n`
        + '# Then open the http://localhost:8080/#token=… link it prints.';
    },
    demo(res) {
      res.textContent = 'Playing the demo below. Prices, volume and fills are simulated.';
      $('demo').scrollIntoView();
      const video = [...document.querySelectorAll('#demo video')].find(v => v.offsetParent);
      if (video) video.play().catch(() => {});
    },
    tape(res) {
      if (!CH) { res.textContent = 'The tape data did not load.'; return; }
      tape.hidden = note.hidden = false;
      hintTape.hidden = true;
      pinned = false;
      stats();
      res.textContent = 'Simulated prints at Cboe’s delayed closing quotes for SPXW, SPY and QQQ. Sides and sizes are random.';
    },
    pause(res) {
      pinned = !pinned;
      stats();
      res.textContent = pinned ? 'Tape paused. Run pause again to resume.' : 'Tape resumed.';
    },
    bench(res) {
      res.innerHTML = 'iv solve   0.4 µs   5.4 Newton iterations per option\n'
        + 'spx pass   40 ms    30,182 options, 63 expiries\n'
        + '# medians, Apple M2 Max, 2026-09-24. <a href="https://github.com/38st/openport/blob/main/docs/methods.md">methods ↗</a>';
    },
    docs(res) {
      const base = 'https://github.com/38st/openport/blob/main/docs/';
      res.innerHTML = [['architecture', 'architecture.md'], ['methods', 'methods.md'], ['paper trading', 'paper-trading.md'], ['providers', 'providers.md'], ['install', 'install.md']]
        .map(([t, f]) => `<a href="${base}${f}">${t} ↗</a>`).join('\n');
    },
    github(res) { res.innerHTML = '<a href="https://github.com/38st/openport">github.com/38st/openport ↗</a>  MIT licence'; },
    theme(res) { toggleTheme(); res.textContent = 'theme: ' + theme(); },
    sudo(res) { res.textContent = 'Not needed. Everything runs as you, on your machine.'; },
    buy(res) { res.textContent = 'Orders go through the terminal’s ticket, and fills are simulated. Try install.'; },
  };
  COMMANDS.sell = COMMANDS.trade = COMMANDS.buy;
  COMMANDS.ls = COMMANDS['?'] = COMMANDS.help;
  COMMANDS.chain = COMMANDS.tape;

  function clear() {
    out.textContent = '';
    tape.hidden = note.hidden = true;
    hintTape.hidden = false;
  }

  const history = [];
  let cursor = 0;
  function run(raw) {
    const cmd = raw.trim();
    if (!cmd) return;
    history.push(cmd);
    cursor = history.length;
    const name = cmd.toLowerCase().split(/\s+/)[0];
    if (name === 'clear') { clear(); return; }
    const entry = document.createElement('div');
    entry.className = 'entry';
    const echo = document.createElement('div');
    echo.className = 'echo';
    echo.textContent = '~ $ ' + cmd;
    const res = document.createElement('div');
    res.className = 'res';
    if (Object.hasOwn(COMMANDS, name)) COMMANDS[name](res);
    else res.textContent = `openport: command not found: ${name}. Try help.`;
    entry.append(echo, res);
    out.append(entry);
    if (name !== 'demo') form.scrollIntoView({ block: 'nearest' });
  }

  form.addEventListener('submit', e => { e.preventDefault(); run(input.value); input.value = ''; });
  input.addEventListener('keydown', e => {
    if (e.key === 'ArrowUp' && cursor > 0) { input.value = history[--cursor]; e.preventDefault(); }
    if (e.key === 'ArrowDown') { cursor = Math.min(history.length, cursor + 1); input.value = history[cursor] || ''; e.preventDefault(); }
  });

  async function copyInstall(button) {
    const label = button.querySelector('span') || button;
    label.dataset.label ??= label.textContent;
    clearTimeout(button.reset);
    try {
      await navigator.clipboard.writeText(INSTALL);
      label.textContent = 'Copied';
      copyStatus.textContent = 'Command copied to clipboard.';
    } catch {
      // Keep manual copying possible when clipboard access is unavailable or denied.
      const command = $('install-command');
      command.focus();
      const range = document.createRange();
      range.selectNodeContents(command);
      const selection = getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      copyStatus.textContent = 'Couldn’t copy automatically. The command is selected; use your device’s Copy action.';
    }
    button.reset = setTimeout(() => { label.textContent = label.dataset.label; copyStatus.textContent = ''; }, 2500);
  }
  document.addEventListener('click', e => {
    const copy = e.target.closest('[data-copy]');
    if (copy) { copyInstall(copy); return; }
    const button = e.target.closest('[data-cmd]');
    if (!button) return;
    run(button.dataset.cmd);
    if (button.dataset.cmd !== 'demo') input.focus({ preventScroll: true });
  });

  let heroVisible = true;
  new IntersectionObserver(([entry]) => { heroVisible = entry.isIntersecting; }).observe(hero);
  document.addEventListener('keydown', e => {
    const key = e.key || '', ctrl = e.ctrlKey && !e.metaKey && !e.altKey;
    if (ctrl && key.toLowerCase() === 'l') { e.preventDefault(); run('clear'); return; }
    if (ctrl && key.toLowerCase() === 'm') { e.preventDefault(); run('theme'); return; }
    const typing = e.target.closest?.('input, textarea, select, [contenteditable]');
    if (key === '?' && (!typing || (e.target === input && !input.value))) { e.preventDefault(); run('help'); input.focus({ preventScroll: true }); return; }
    if (!typing && heroVisible && key.length === 1 && key !== ' ' && !e.ctrlKey && !e.metaKey && !e.altKey) input.focus({ preventScroll: true });
  });

  /* Demo videos */
  const videos = [...document.querySelectorAll('#demo video')];
  const mobileLayout = matchMedia('(max-width: 700px)');
  // A change in layout must never leave the hidden recording playing.
  mobileLayout.addEventListener('change', () => videos.forEach(video => video.pause()));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) videos.forEach(video => video.pause());
  });
})();
