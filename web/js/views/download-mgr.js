/* ══ DOWNLOAD MANAGER VIEW ════════════════════════════════════════════════
 * URL input, active downloads, history.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var ICO = Z.ICO;

  var dlm = {};
  var _downloads = [];
  var _history = [];
  var _pollTimer = null;
  var _destPath = '/';

  dlm.refresh = function () {
    renderActive();
    renderHistory();
    startPolling();
  };

  /* ── Start a download ── */
  dlm.start = function () {
    var input = $('dl-url');
    if (!input) return;
    var url = (input.value || '').trim();
    if (!url) {
      Z.toast('Enter a URL first', 'wn');
      input.focus();
      return;
    }

    /* Detect URL type */
    var type = detectUrlType(url);
    var displayName = extractFilename(url) || 'download';

    Z.toast('Starting download: ' + displayName, 'ok');
    input.value = '';

    var entry = {
      id: Date.now(),
      url: url,
      name: displayName,
      type: type,
      dst: _destPath,
      status: 'starting',
      progress: 0,
      speed: 0,
      size: 0,
      downloaded: 0,
      startTime: Date.now()
    };
    _downloads.push(entry);
    renderActive();

    Z.api.downloadStart(url, _destPath).then(function (d) {
      entry.status = 'downloading';
      if (d && d.id) entry.serverId = d.id;
      if (d && d.name) entry.name = d.name;
      if (d && d.size) entry.size = d.size;
      renderActive();
      /* Start topbar download progress polling */
      if (Z.onDownloadStarted) Z.onDownloadStarted();
    }).catch(function (e) {
      entry.status = 'error';
      entry.error = e.message;
      renderActive();
      Z.notify('Download failed', displayName + ': ' + e.message, 'er');
    });
  };

  /* ── URL type detection ── */
  function detectUrlType(url) {
    if (/magnet:/i.test(url)) return 'magnet';
    if (/drive\.google\.com/i.test(url)) return 'gdrive';
    if (/mega\.(nz|co\.nz)/i.test(url)) return 'mega';
    if (/mediafire\.com/i.test(url)) return 'mediafire';
    if (/1fichier\.com/i.test(url)) return '1fichier';
    return 'http';
  }

  /* ── Extract filename from URL ── */
  function extractFilename(url) {
    try {
      if (/magnet:/i.test(url)) {
        var dnMatch = url.match(/dn=([^&]+)/);
        return dnMatch ? decodeURIComponent(dnMatch[1]) : 'magnet-download';
      }
      var path = url.split('?')[0].split('#')[0];
      var parts = path.split('/');
      var last = parts[parts.length - 1];
      return last ? decodeURIComponent(last) : 'download';
    } catch (e) {
      return 'download';
    }
  }

  /* ── Type icon ── */
  function typeIcon(type) {
    switch (type) {
      case 'magnet':    return ICO.link;
      case 'gdrive':    return ICO.cloud;
      case 'mega':      return ICO.cloud;
      case 'mediafire': return ICO.cloud;
      default:          return ICO.cloudDown;
    }
  }

  /* ── Render active downloads ── */
  function renderActive() {
    var wrap = $('dl-active');
    if (!wrap) return;
    wrap.innerHTML = '';

    var countEl = $('dl-active-count');
    var active = _downloads.filter(function (d) { return d.status !== 'done' && d.status !== 'error'; });
    if (countEl) countEl.textContent = active.length;

    if (!_downloads.length) {
      wrap.innerHTML = '<div class="dl-empty"><div class="dl-empty-icon">' + ICO.cloudDown + '</div><div>No active downloads</div><div style="font-size:11px;color:var(--tx3);">Paste a URL above to start downloading</div></div>';
      return;
    }

    for (var i = 0; i < _downloads.length; i++) {
      var dl = _downloads[i];
      var card = D.createElement('div');
      card.className = 'dl-card';

      var elapsed = (Date.now() - dl.startTime) / 1000;
      var speedStr = dl.speed > 0 ? Z.bps(dl.speed) : '\u2014';
      var etaStr = dl.speed > 0 && dl.size > 0 ? Z.duration((dl.size - dl.downloaded) / dl.speed) : '\u2014';
      var progressClass = dl.status === 'error' ? ' error' : dl.status === 'done' ? ' done' : '';
      var statusLabel = dl.status === 'downloading' ? 'Downloading' : dl.status === 'starting' ? 'Starting\u2026' : dl.status === 'paused' ? 'Paused' : dl.status === 'done' ? 'Complete' : dl.status === 'error' ? 'Failed' : dl.status;

      card.innerHTML =
        '<div class="dl-card-top">' +
          '<div class="dl-card-icon">' + typeIcon(dl.type) + '</div>' +
          '<div class="dl-card-info">' +
            '<div class="dl-card-name">' + dl.name + '</div>' +
            '<div class="dl-card-url">' + dl.url.substring(0, 80) + (dl.url.length > 80 ? '\u2026' : '') + '</div>' +
          '</div>' +
          '<div class="dl-card-stats">' +
            '<div class="dl-stat"><div class="dl-stat-value">' + speedStr + '</div><div class="dl-stat-label">Speed</div></div>' +
            '<div class="dl-stat"><div class="dl-stat-value">' + etaStr + '</div><div class="dl-stat-label">ETA</div></div>' +
            '<div class="dl-stat"><div class="dl-stat-value">' + dl.progress + '%</div><div class="dl-stat-label">' + statusLabel + '</div></div>' +
          '</div>' +
        '</div>' +
        '<div class="dl-progress"><div class="dl-progress-fill' + progressClass + '" style="width:' + dl.progress + '%"></div></div>' +
        '<div class="dl-card-actions">' +
          (dl.status === 'downloading' ? '<button class="btn" data-action="pause" data-id="' + dl.id + '">' + ICO.pause + ' Pause</button>' : '') +
          (dl.status === 'paused' ? '<button class="btn" data-action="resume" data-id="' + dl.id + '">' + ICO.play + ' Resume</button>' : '') +
          '<button class="btn danger" data-action="cancel" data-id="' + dl.id + '">' + ICO.xcancel + ' Cancel</button>' +
        '</div>';

      wrap.appendChild(card);
    }

    /* Wire action buttons */
    var btns = wrap.querySelectorAll('button[data-action]');
    for (var b = 0; b < btns.length; b++) {
      (function (btn) {
        btn.onclick = function () {
          var action = btn.getAttribute('data-action');
          var id = parseInt(btn.getAttribute('data-id'), 10);
          handleAction(action, id);
        };
      })(btns[b]);
    }
  }

  /* ── Handle button actions ── */
  function handleAction(action, id) {
    var dl = _downloads.filter(function (d) { return d.id === id; })[0];
    if (!dl) return;

    if (action === 'cancel') {
      if (dl.serverId) Z.api.downloadCancel(dl.serverId).catch(function () { });
      dl.status = 'error';
      dl.error = 'Cancelled';
      /* Move to history */
      _history.unshift({ name: dl.name, size: dl.downloaded, date: Date.now(), status: 'cancelled' });
      _downloads = _downloads.filter(function (d) { return d.id !== id; });
      renderActive();
      renderHistory();
      Z.toast('Download cancelled', 'wn');
    } else if (action === 'pause') {
      if (dl.serverId) Z.api.downloadPause(dl.serverId).catch(function () { });
      dl.status = 'paused';
      renderActive();
    } else if (action === 'resume') {
      if (dl.serverId) Z.api.downloadPause(dl.serverId).catch(function () { });
      dl.status = 'downloading';
      renderActive();
    }
  }

  /* ── Render history ── */
  function renderHistory() {
    var wrap = $('dl-history');
    if (!wrap) return;
    wrap.innerHTML = '';

    if (!_history.length) {
      wrap.innerHTML = '<div style="padding:16px;text-align:center;color:var(--tx3);font-size:11px;">No download history</div>';
      return;
    }

    for (var i = 0; i < _history.length && i < 20; i++) {
      var h = _history[i];
      var item = D.createElement('div');
      item.className = 'dl-history-item';
      item.innerHTML =
        '<div class="dl-history-name">' + h.name + '</div>' +
        '<div class="dl-history-size">' + Z.bytes(h.size || 0) + '</div>' +
        '<div class="dl-history-date">' + Z.relativeTime(Math.floor(h.date / 1000)) + '</div>' +
        '<div class="dl-history-status ' + (h.status === 'complete' ? 'ok' : 'er') + '">' + h.status + '</div>';
      wrap.appendChild(item);
    }
  }

  /* ── Poll for progress ── */
  function startPolling() {
    if (_pollTimer) clearInterval(_pollTimer);
    _pollTimer = setInterval(function () {
      var active = _downloads.filter(function (d) { return d.status === 'downloading'; });
      if (!active.length) return;

      Z.api.downloadStatus().then(function (data) {
        if (!data || !Array.isArray(data.downloads)) return;
        for (var i = 0; i < data.downloads.length; i++) {
          var sd = data.downloads[i];
          var local = _downloads.filter(function (d) { return d.serverId === sd.id; })[0];
          if (!local) continue;
          local.progress = sd.progress || 0;
          local.speed = sd.speed || 0;
          local.downloaded = sd.downloaded || 0;
          local.size = sd.total_size || local.size;
          if (sd.done) {
            local.status = sd.error ? 'error' : 'done';
            if (sd.error) local.error = sd.error;
            /* Move to history */
            _history.unshift({ name: local.name, size: local.size, date: Date.now(), status: local.status === 'done' ? 'complete' : 'failed' });
            _downloads = _downloads.filter(function (d) { return d.id !== local.id; });
            if (local.status === 'done') Z.toast('Downloaded: ' + local.name, 'ok');
          }
        }
        renderActive();
        renderHistory();
      }).catch(function () { });
    }, 1500);
  }

  /* ── Destination selector — modal folder browser ──────────────────────
   *
   *  Opens an overlay that browses the console filesystem via /api/list.
   *  Only directories are shown; clicking one navigates into it.
   *  "Select this folder" confirms the choice.
   *
   *  ┌──────────────────────────────────────────────┐
   *  │  Choose download destination        [✕]      │
   *  │  ─────────────────────────────────────────── │
   *  │  / > data > user                             │
   *  │  [↑ Back]                                    │
   *  │  ┌──────────────────────────────┐            │
   *  │  │ 📁 folder_a                  │            │
   *  │  │ 📁 folder_b                  │            │
   *  │  │ 📁 folder_c                  │            │
   *  │  └──────────────────────────────┘            │
   *  │         [ Select this folder ]               │
   *  └──────────────────────────────────────────────┘
   */
  dlm.selectDest = function () {
    var browsePath = _destPath;
    var overlay = D.createElement('div');
    overlay.className = 'dl-dest-overlay';
    overlay.style.cssText = 'position:fixed;inset:0;z-index:1000;background:rgba(0,0,0,.65);' +
        'display:flex;align-items:center;justify-content:center;animation:fadeIn .15s ease;';

    var card = D.createElement('div');
    card.style.cssText = 'background:var(--sf);border:1px solid var(--bd2);border-radius:16px;' +
        'width:min(460px,90vw);max-height:70vh;display:flex;flex-direction:column;' +
        'box-shadow:0 32px 80px rgba(0,0,0,.6);overflow:hidden;';

    /* Header */
    var hdr = D.createElement('div');
    hdr.style.cssText = 'display:flex;align-items:center;padding:16px 20px;' +
        'border-bottom:1px solid var(--bd);gap:10px;';
    hdr.innerHTML = '<div style="flex:1;font-weight:700;font-size:14px;color:var(--tx);">' +
        'Choose download destination</div>' +
        '<button id="dl-dest-close" style="background:none;border:none;color:var(--tx3);' +
        'font-size:20px;cursor:pointer;padding:0 4px;line-height:1;">&times;</button>';

    /* Breadcrumb bar */
    var bcBar = D.createElement('div');
    bcBar.style.cssText = 'padding:10px 20px 4px;font-size:11px;color:var(--tx3);' +
        'font-family:monospace;white-space:nowrap;overflow-x:auto;';

    /* Body — folder list */
    var body = D.createElement('div');
    body.style.cssText = 'flex:1;overflow-y:auto;padding:8px 12px;min-height:120px;max-height:50vh;';

    /* Footer — back, new folder, select */
    var ftr = D.createElement('div');
    ftr.style.cssText = 'padding:12px 20px;border-top:1px solid var(--bd);display:flex;gap:8px;';
    ftr.innerHTML = '<button id="dl-dest-up" class="btn" style="padding:6px 10px;font-size:11px;">' +
        '&uarr; Back</button>' +
        '<button id="dl-dest-mkdir" class="btn" style="padding:6px 10px;font-size:11px;">' +
        '+ New Folder</button>' +
        '<div style="flex:1;"></div>' +
        '<button id="dl-dest-ok" class="btn" style="padding:8px 18px;font-size:12px;font-weight:700;' +
        'background:var(--ac);color:#fff;border:none;border-radius:8px;cursor:pointer;">' +
        'Select this folder</button>';

    card.appendChild(hdr);
    card.appendChild(bcBar);
    card.appendChild(body);
    card.appendChild(ftr);
    overlay.appendChild(card);
    D.body.appendChild(overlay);

    /* Close overlay */
    function close() { if (overlay.parentNode) overlay.parentNode.removeChild(overlay); }
    overlay.addEventListener('click', function (e) { if (e.target === overlay) close(); });
    hdr.querySelector('#dl-dest-close').onclick = close;

    /* Navigate up */
    ftr.querySelector('#dl-dest-up').onclick = function () {
      var parent = Z.parent(browsePath);
      if (parent !== null) { browsePath = parent; loadDir(browsePath); }
    };

    /* Create new folder in current directory */
    ftr.querySelector('#dl-dest-mkdir').onclick = function () {
      Z.modal.prompt('New Folder', '').then(function (name) {
        if (!name) return;
        Z.api.mkdir(browsePath, name).then(function () {
          Z.toast('Folder created', 'ok');
          loadDir(browsePath);
        }).catch(function (e) {
          Z.toast('Failed: ' + e.message, 'er');
        });
      });
    };

    /* Confirm selection */
    ftr.querySelector('#dl-dest-ok').onclick = function () {
      _destPath = browsePath;
      var el = $('dl-dest-path');
      if (el) el.textContent = _destPath;
      close();
    };

    /* Render breadcrumb */
    function renderBreadcrumb(path) {
      var parts = path.split('/').filter(function (s) { return s.length > 0; });
      var html = '<span style="color:var(--ac);cursor:pointer;" data-path="/">/</span>';
      var cum = '';
      for (var i = 0; i < parts.length; i++) {
        cum += '/' + parts[i];
        html += ' <span style="color:var(--tx3);">›</span> ' +
            '<span style="color:var(--ac);cursor:pointer;" data-path="' + cum + '">' +
            parts[i] + '</span>';
      }
      bcBar.innerHTML = html;
      /* Wire breadcrumb clicks */
      var spans = bcBar.querySelectorAll('span[data-path]');
      for (var s = 0; s < spans.length; s++) {
        (function (sp) {
          sp.onclick = function () {
            browsePath = sp.getAttribute('data-path');
            loadDir(browsePath);
          };
        })(spans[s]);
      }
    }

    /* Load directory listing */
    function loadDir(path) {
      renderBreadcrumb(path);
      body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--tx3);font-size:12px;">Loading…</div>';
      Z.api.list(path).then(function (data) {
        body.innerHTML = '';
        if (!data || !data.entries || !data.entries.length) {
          body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--tx3);font-size:12px;">Empty directory</div>';
          return;
        }
        /* Show only directories, sorted alphabetically */
        var dirs = data.entries.filter(function (e) { return e.type === 'directory'; });
        dirs.sort(function (a, b) { return a.name.localeCompare(b.name); });

        if (!dirs.length) {
          body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--tx3);font-size:12px;">No subdirectories</div>';
          return;
        }

        for (var i = 0; i < dirs.length; i++) {
          (function (entry) {
            var row = D.createElement('div');
            row.style.cssText = 'display:flex;align-items:center;gap:10px;padding:8px 12px;' +
                'border-radius:8px;cursor:pointer;color:var(--tx2);transition:all .1s;font-size:12px;';
            row.innerHTML = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
                'stroke-width="2" style="flex-shrink:0;color:var(--ac);"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 ' +
                '1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>' +
                '<span style="flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;">' +
                entry.name + '</span>' +
                '<span style="color:var(--tx3);font-size:10px;">›</span>';
            row.onmouseenter = function () { row.style.background = 'var(--sf2)'; row.style.color = 'var(--tx)'; };
            row.onmouseleave = function () { row.style.background = 'none'; row.style.color = 'var(--tx2)'; };
            row.onclick = function () {
              browsePath = Z.join(path, entry.name);
              loadDir(browsePath);
            };
            body.appendChild(row);
          })(dirs[i]);
        }
      }).catch(function (err) {
        body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--er);font-size:12px;">' +
            'Error loading directory: ' + err.message + '</div>';
      });
    }

    loadDir(browsePath);
  };

  Z.downloadMgr = dlm;

})(ZFTPD);
