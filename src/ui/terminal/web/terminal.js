/* The page has no general native API: only terminal input, size and output ACKs. */
(() => {
  'use strict';
  const maxInputBytes = 64 * 1024;
  const scrollback = 5000;
  const notice = document.getElementById('notice');
  let epoch = '', enabled = false, evicted = false, resetToken = 0;
  const terminal = new Terminal({
    allowProposedApi: true, disableStdin: true, scrollback,
    fontFamily: 'Cascadia Mono, Consolas, monospace', fontSize: 14,
    cursorBlink: true, theme: { background: '#0b1320', foreground: '#dce8f6' },
    linkHandler: { activate() {} }
  });
  const fit = new FitAddon.FitAddon();
  terminal.loadAddon(fit);
  terminal.open(document.getElementById('terminal'));
  // Remote escape sequences cannot read or write the Controller clipboard.
  terminal.parser.registerOscHandler(52, () => true);
  const send = (type, fields = {}) => window.chrome.webview.postMessage({v: 1, epoch, type, ...fields});
  const updateNotice = () => {
    notice.textContent = (enabled ? 'PowerShell' : 'Terminal input paused')
      + (evicted ? ' · Earlier scrollback has been discarded' : '');
  };
  terminal.onData(data => {
    if (!enabled || new TextEncoder().encode(data).length > maxInputBytes) return;
    send('input', {data});
  });
  terminal.onBinary(data => {
    if (!enabled || data.length > maxInputBytes) return;
    send('binary', {data: btoa(data)});
  });
  terminal.onResize(({cols, rows}) => { if (epoch) send('resize', {cols, rows}); });
  terminal.onWriteParsed(() => {
    if (!evicted && terminal.buffer.active.baseY >= scrollback) { evicted = true; updateNotice(); }
  });
  new ResizeObserver(() => { if (document.body.clientHeight > 40) fit.fit(); }).observe(document.body);
  window.chrome.webview.addEventListener('message', ({data}) => {
    if (!data || data.v !== 1) return;
    if (data.type === 'reset' && typeof data.epoch === 'string') {
      const token = ++resetToken;
      enabled = false; terminal.options.disableStdin = true;
      // Drain previously submitted writes before clearing. Native waits for
      // reset_done before submitting any bytes from the next desktop session.
      terminal.write('', () => {
        if (token !== resetToken) return;
        epoch = data.epoch; evicted = false;
        terminal.reset(); fit.fit(); updateNotice();
        send('reset_done'); send('resize', {cols: terminal.cols, rows: terminal.rows});
      });
      return;
    }
    if (data.epoch !== epoch || !epoch) return;
    if (data.type === 'focus') {
      terminal.focus();
    } else if (data.type === 'enabled') {
      enabled = data.enabled === true;
      terminal.options.disableStdin = !enabled; updateNotice();
    } else if (data.type === 'output' && typeof data.data === 'string'
        && Number.isSafeInteger(data.sequence) && data.data.length <= 24 * 1024) {
      const outputEpoch = epoch;
      const bytes = Uint8Array.from(atob(data.data), c => c.charCodeAt(0));
      terminal.write(bytes, () => {
        if (epoch === outputEpoch) send('ack', {sequence: data.sequence});
      });
    }
  });
  fit.fit(); send('ready');
})();
