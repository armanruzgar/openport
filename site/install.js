(() => {
  const command = document.getElementById('install-command');
  const description = document.getElementById('install-description');
  const panel = document.getElementById('install-panel');
  const tabs = [...document.querySelectorAll('[data-install]')];
  const docker = { command: command.textContent.trim(), description: description.innerHTML };
  function select(tab) {
    const brew = tab.dataset.install === 'homebrew';
    command.querySelector('code').textContent = brew
      ? document.getElementById('homebrew-command').content.textContent.trim() : docker.command;
    command.setAttribute('aria-label', brew ? 'Homebrew quick start' : 'Docker quick start');
    if (brew) description.textContent = 'On macOS, Homebrew installs the Apple Silicon archive or builds from source on Intel. The last command opens the terminal with its saved token. Accounts and chart history stay in Homebrew’s var/openport directory. The default feed is Cboe’s free 15-minute delayed data; the demo market uses simulated prices.';
    else description.innerHTML = docker.description;
    panel.setAttribute('aria-labelledby', tab.id);
    for (const item of tabs) {
      item.setAttribute('aria-selected', String(item === tab));
      item.tabIndex = item === tab ? 0 : -1;
    }
    document.getElementById('copy-status').textContent = '';
  }
  for (const tab of tabs) {
    tab.addEventListener('click', () => select(tab));
    tab.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
      event.preventDefault();
      const index = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1
        : (tabs.indexOf(tab) + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
      select(tabs[index]);
      tabs[index].focus();
    });
  }
  const demo = document.getElementById('hosted-demo');
  try {
    const url = new URL(demo.dataset.demoUrl);
    if (url.protocol !== 'https:' || url.username || url.password || url.hash || url.search ||
        /(^|\.)(example\.(com|org|net)|localhost|invalid|test)$/.test(url.hostname)) return;
    demo.href = url.href;
    demo.hidden = false;
  } catch { /* No deployed URL: leave the button hidden and without a link. */ }
})();
