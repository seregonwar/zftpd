/* ══ APP — Router & State Management ══════════════════════════════════════
 * Central application bootstrap. Manages view switching, global state,
 * and wires up navigation tabs.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;

  /* ── Global state ── */
  Z.state = {
    view: 'dashboard',       /* active view: dashboard | explorer | filemanager | downloads */
    path: '/',               /* current directory path (explorer) */
    entries: [],             /* current directory entries */
    transferActive: false,   /* true while upload/copy is running */
    statsTimer: null,        /* interval ID for stats polling */
    fmLeftPath: '/',         /* file manager left panel path */
    fmRightPath: '/'         /* file manager right panel path */
  };

  /* ── Themes ── */
  Z.THEMES = [
    { id: 'ps5',    name: 'PS5',     desc: 'Default blue',  sw: '#2b8cff' },
    { id: 'cloud',  name: 'Cloud',   desc: 'Clean white',   sw: '#3b82f6' },
    { id: 'matrix', name: 'Matrix',  desc: 'Green terminal', sw: '#00e639' },
    { id: 'sunset', name: 'Sunset',  desc: 'Warm orange',   sw: '#ff6535' },
    { id: 'arctic', name: 'Arctic',  desc: 'Cool cyan',     sw: '#00d4ff' },
    { id: 'neon',   name: 'Neon',    desc: 'Purple glow',   sw: '#c800ff' },
    { id: 'amber',  name: 'Amber',   desc: 'Retro gold',    sw: '#ffb700' }
  ];

  /* ── View switching ── */
  Z.switchView = function (viewId) {
    var valid = { dashboard: 1, explorer: 1, filemanager: 1, downloads: 1, games: 1, settings: 1 };
    if (!valid[viewId]) return;

    Z.state.view = viewId;

    /* Update nav tabs */
    var tabs = D.querySelectorAll('.nav-tab');
    for (var i = 0; i < tabs.length; i++) {
      var t = tabs[i];
      if (t.getAttribute('data-view') === viewId) {
        t.classList.add('active');
      } else {
        t.classList.remove('active');
      }
    }

    /* Update view panels */
    var views = D.querySelectorAll('.view');
    for (var j = 0; j < views.length; j++) {
      var v = views[j];
      if (v.id === 'view-' + viewId) {
        v.classList.add('view-active');
      } else {
        v.classList.remove('view-active');
      }
    }

    /* Trigger view-specific init */
    if (viewId === 'explorer' && Z.explorer && Z.explorer.init) {
      Z.explorer.init();
      Z.explorer.nav(Z.state.path);
    }
    if (viewId === 'dashboard' && Z.dashboard && Z.dashboard.refresh) {
      Z.dashboard.refresh();
    }
    if (viewId === 'filemanager' && Z.fileManager && Z.fileManager.init) {
      Z.fileManager.init();
    }
    if (viewId === 'downloads' && Z.downloadMgr && Z.downloadMgr.refresh) {
      Z.downloadMgr.refresh();
    }
    if (viewId === 'games' && Z.gamesView && Z.gamesView.refresh) {
      Z.gamesView.refresh();
    }
    if (viewId === 'settings' && Z.settingsView && Z.settingsView.refresh) {
      Z.settingsView.refresh();
    }

    if (Z.updateTopbarContext) Z.updateTopbarContext();

    try { localStorage.setItem('zftpd_view', viewId); } catch (e) { }
  };

  /* ═══════════════════════════════════════════════════════════════════════
   * TRANSFER LOCK MODAL
   * Shows a blocking modal during uploads/copies preventing navigation.
   *
   *   ┌──────────────────────────────────┐
   *   │  ⬆ UPLOADING…    18 MB/s  17s  │
   *   │  filename.pkg            44%    │
   *   │  → /data                        │
   *   │  ████████░░░░░░░░░░░░░░░        │
   *   │      [‖ Pause]  [✕ Cancel]      │
   *   └──────────────────────────────────┘
   * ═══════════════════════════════════════════════════════════════════════ */

  var _xferOverlay = null;
  var _xferEls = {};

  function _ensureXferOverlay() {
    if (_xferOverlay) return;
    var ov = D.createElement('div');
    ov.className = 'xfer-lock-overlay';
    ov.innerHTML =
      '<div class="xfer-lock-card">' +
        '<div class="xfer-lock-header">' +
          '<span id="xfer-label">UPLOADING\u2026</span>' +
          '<span><span id="xfer-speed" class="xfer-lock-speed"></span> ' +
          '<span id="xfer-time" class="xfer-lock-time"></span> ' +
          '<span id="xfer-pct" class="xfer-lock-pct"></span></span>' +
        '</div>' +
        '<div class="xfer-lock-body">' +
          '<div id="xfer-filename" class="xfer-lock-filename"></div>' +
          '<div id="xfer-dest" class="xfer-lock-dest"></div>' +
          '<div class="xfer-lock-bar"><div id="xfer-bar" class="xfer-lock-bar-fill"></div></div>' +
        '</div>' +
        '<div class="xfer-lock-footer">' +
          '<button id="xfer-pause" class="btn">&#x2016; Pause</button>' +
          '<button id="xfer-cancel" class="btn danger">&times; Cancel</button>' +
        '</div>' +
      '</div>';
    D.body.appendChild(ov);
    _xferOverlay = ov;
    _xferEls = {
      label: ov.querySelector('#xfer-label'),
      speed: ov.querySelector('#xfer-speed'),
      time: ov.querySelector('#xfer-time'),
      pct: ov.querySelector('#xfer-pct'),
      filename: ov.querySelector('#xfer-filename'),
      dest: ov.querySelector('#xfer-dest'),
      bar: ov.querySelector('#xfer-bar'),
      pauseBtn: ov.querySelector('#xfer-pause'),
      cancelBtn: ov.querySelector('#xfer-cancel')
    };
  }

  /**
   * Show the transfer lock modal
   * @param {object} opts - { label, filename, dest, onPause, onCancel }
   */
  Z.showTransferLock = function (opts) {
    _ensureXferOverlay();
    opts = opts || {};
    Z.state.transferActive = true;
    _xferEls.label.textContent = (opts.label || 'UPLOADING') + '\u2026';
    _xferEls.filename.textContent = opts.filename || '';
    _xferEls.dest.textContent = opts.dest || '/';
    _xferEls.speed.textContent = '';
    _xferEls.time.textContent = '';
    _xferEls.pct.textContent = '';
    _xferEls.bar.style.width = '0%';
    _xferEls.pauseBtn.onclick = opts.onPause || function () {};
    _xferEls.cancelBtn.onclick = opts.onCancel || function () {};
    /* Show/hide pause button */
    _xferEls.pauseBtn.style.display = opts.onPause ? '' : 'none';
    _xferOverlay.classList.add('on');
  };

  /**
   * Update the transfer lock modal progress
   * @param {object} info - { pct, speed, elapsed }
   */
  Z.updateTransferLock = function (info) {
    if (!_xferOverlay) return;
    info = info || {};
    if (typeof info.pct === 'number') {
      _xferEls.pct.textContent = info.pct + '%';
      _xferEls.bar.style.width = info.pct + '%';
    }
    if (info.speed) _xferEls.speed.textContent = info.speed;
    if (info.elapsed) _xferEls.time.textContent = info.elapsed;
  };

  /** Hide the transfer lock modal */
  Z.hideTransferLock = function () {
    Z.state.transferActive = false;
    if (_xferOverlay) _xferOverlay.classList.remove('on');
  };

  /* Legacy compat */
  Z.setTransferActive = function (active) {
    Z.state.transferActive = !!active;
    if (!active) Z.hideTransferLock();
  };

  Z.ensureTransferIdle = function () {
    if (Z.state.transferActive) {
      Z.toast('Transfer in progress\u2026', 'wn');
      return false;
    }
    return true;
  };

  /* ═══════════════════════════════════════════════════════════════════════
   * NOTIFICATION CENTER
   *
   *  Z.notify(title, desc, type)   — type: 'ok' | 'er' | 'wn' | ''
   *  Notifications appear in the bell dropdown in the topbar.
   * ═══════════════════════════════════════════════════════════════════════ */

  var _notifications = [];
  var _notifMaxItems = 50;

  Z.notify = function (title, desc, type) {
    _notifications.unshift({
      title: title || '',
      desc: desc || '',
      type: type || '',
      time: new Date()
    });
    if (_notifications.length > _notifMaxItems) {
      _notifications.length = _notifMaxItems;
    }
    _renderNotifications();
    /* Also show toast for immediate feedback */
    Z.toast(title, type);
  };

  function _renderNotifications() {
    var badge = $('tb-notif-badge');
    var list = $('tb-notif-list');
    if (!list) return;

    /* Update badge count */
    if (badge) {
      if (_notifications.length > 0) {
        badge.textContent = _notifications.length > 99 ? '99+' : _notifications.length;
        badge.classList.remove('hidden');
      } else {
        badge.classList.add('hidden');
      }
    }

    list.innerHTML = '';
    
    if (!_notifications.length) {
      list.innerHTML = '<div class="tb-notif-empty">No notifications</div>';
      return;
    }

    var clr = D.createElement('div');
    clr.style.cssText = 'padding:8px 16px;text-align:right;font-size:12px;color:var(--ac);cursor:pointer;border-bottom:1px solid var(--bd);line-height:1;margin-bottom:4px;font-weight:600;';
    clr.innerHTML = 'Clear All';
    clr.onclick = function(e) {
      e.stopPropagation();
      _notifications = [];
      _renderNotifications();
    };
    list.appendChild(clr);

    var icoMap = {
      ok: '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/></svg>',
      er: '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>',
      wn: '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>'
    };
    var defaultIco = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>';

    for (var i = 0; i < _notifications.length && i < 30; i++) {
      var n = _notifications[i];
      var ago = _timeAgo(n.time);
      var item = D.createElement('div');
      item.className = 'tb-notif-item';
      item.innerHTML =
        '<div class="ni-ico ' + (n.type || '') + '">' + (icoMap[n.type] || defaultIco) + '</div>' +
        '<div class="ni-body"><div class="ni-title">' + n.title + '</div>' +
        (n.desc ? '<div class="ni-desc">' + n.desc + '</div>' : '') + '</div>' +
        '<div class="ni-time">' + ago + '</div>';
        
      var cb = D.createElement('span');
      cb.innerHTML = '&times;';
      cb.style.cssText = 'position:absolute;top:10px;right:10px;cursor:pointer;color:var(--tx3);font-size:16px;line-height:1;width:24px;height:24px;display:flex;align-items:center;justify-content:center;border-radius:12px;';
      cb.onmouseover = function(){this.style.background='var(--tg)';this.style.color='var(--tx)';};
      cb.onmouseout = function(){this.style.background='none';this.style.color='var(--tx3)';};
      (function(idx) {
        cb.onclick = function(e){
          e.stopPropagation();
          _notifications.splice(idx, 1);
          _renderNotifications();
        };
      })(i);
      item.appendChild(cb);
      list.appendChild(item);
    }
  }

  function _timeAgo(date) {
    var sec = Math.floor((Date.now() - date.getTime()) / 1000);
    if (sec < 60) return 'now';
    if (sec < 3600) return Math.floor(sec / 60) + 'm';
    if (sec < 86400) return Math.floor(sec / 3600) + 'h';
    return Math.floor(sec / 86400) + 'd';
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * DOWNLOAD PROGRESS PILL (topbar)
   *
   * Polls /api/download/status every 2s when downloads are active.
   * Updates the pill count + aggregate progress bar.
   * ═══════════════════════════════════════════════════════════════════════ */

  var _dlPollTimer = null;

  function _startDlPolling() {
    if (_dlPollTimer) return;
    _dlPollTimer = setInterval(_pollDownloads, 2000);
    _pollDownloads();
  }

  function _stopDlPolling() {
    if (_dlPollTimer) { clearInterval(_dlPollTimer); _dlPollTimer = null; }
  }

  function _pollDownloads() {
    Z.api.downloadStatus().then(function (data) {
      var items = (data && Array.isArray(data.downloads)) ? data.downloads : [];
      var active = items.filter(function (d) { return !d.done && !d.error; });
      var pill = $('tb-dl-pill');
      var countEl = $('tb-dl-count');
      var barFill = $('tb-dl-bar-fill');

      if (active.length > 0) {
        if (pill) pill.classList.remove('hidden');
        if (countEl) countEl.textContent = active.length;

        /* Calculate aggregate progress */
        var totalDown = 0, totalSize = 0;
        for (var i = 0; i < active.length; i++) {
          totalDown += (active[i].downloaded || 0);
          totalSize += (active[i].total_size || 0);
        }
        var pct = totalSize > 0 ? Math.floor(totalDown / totalSize * 100) : 0;
        if (barFill) barFill.style.width = pct + '%';
      } else {
        if (pill) pill.classList.add('hidden');
        _stopDlPolling();
      }

      /* Notify on completed/errored downloads */
      for (var j = 0; j < items.length; j++) {
        var d = items[j];
        var nKey = 'dl_notified_' + (d.id || j);
        var isDone = (d.done === true || d.done === 'true');
        var hasErr = (!!d.error);
        var dName = d.name || d.url || '';
        if (isDone && !hasErr && !Z.state[nKey]) {
          Z.state[nKey] = true;
          Z.notify('Download complete', dName, 'ok');
        }
        if (hasErr && !Z.state[nKey]) {
          Z.state[nKey] = true;
          Z.notify('Download failed', dName + ': ' + d.error, 'er');
        }
      }
    }).catch(function () { });
  }

  /* Hook: call this from download-mgr.js when a download starts */
  Z.onDownloadStarted = function () { _startDlPolling(); };
  /* Click pill → go to downloads view */
  D.addEventListener('DOMContentLoaded', function () {
    var pill = $('tb-dl-pill');
    if (pill) pill.onclick = function () { Z.switchView('downloads'); };
  });

  /* ── Theme management ── */
  Z.setTheme = function (id) {
    var ids = Z.THEMES.map(function (t) { return t.id; });
    var theme = ids.indexOf(id) >= 0 ? id : 'ps5';
    D.documentElement.setAttribute('data-theme', theme);

    /* Update mobile select */
    var sel = $('theme-select');
    if (sel) sel.value = theme;

    /* Update desktop button swatch */
    var t = Z.THEMES.filter(function (x) { return x.id === theme; })[0] || Z.THEMES[0];
    var sw = $('t-sw');
    if (sw) sw.style.background = t.sw;
    var tn = $('t-nm');
    if (tn) tn.textContent = t.name;

    try { localStorage.setItem('zftpd_theme', theme); } catch (e) { }
  };

  /* ── Stats polling ── */
  function refreshStats() {
    Z.api.stats(Z.state.path).then(function (d) {
      if (Z.dashboard && Z.dashboard.updateStats) Z.dashboard.updateStats(d);
      if (Z.updateTopbarStats) Z.updateTopbarStats(d);
    }).catch(function () { });
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * TOPBAR COMMAND CENTER
   * Global command palette, quick tools, context labels, and health chips.
   * ═══════════════════════════════════════════════════════════════════════ */

  var _cmdOpen = false;
  var _cmdActiveIndex = 0;
  var _cmdVisible = [];

  Z.updateTopbarContext = function () {
    var viewNames = {
      dashboard: 'Dashboard',
      explorer: 'Explorer',
      filemanager: 'File Manager',
      downloads: 'Downloads',
      games: 'Games',
      settings: 'Settings'
    };
    var view = Z.state.view || 'dashboard';
    var label = viewNames[view] || view;
    var context = '/';

    if (view === 'explorer') context = Z.state.path || '/';
    else if (view === 'filemanager') context = (Z.state.fmLeftPath || '/') + '  \u2192  ' + (Z.state.fmRightPath || '/');
    else if (view === 'downloads') context = 'Transfer queue';
    else if (view === 'games') context = 'Library and package tools';
    else if (view === 'settings') context = 'Preferences';
    else context = 'System overview';

    var viewEl = $('tb-view-label');
    var ctxEl = $('tb-context-label');
    if (viewEl) viewEl.textContent = label;
    if (ctxEl) {
      ctxEl.textContent = context;
      ctxEl.title = context;
    }
  };

  Z.updateTopbarStats = function (d) {
    if (!d || typeof d !== 'object') return;

    var storage = $('tb-storage');
    if (storage) {
      var used = typeof d.disk_used === 'number' ? d.disk_used : null;
      var total = typeof d.disk_total === 'number' ? d.disk_total : null;
      if (used === null && total && typeof d.disk_free === 'number') used = total - d.disk_free;
      if (used !== null && total && total > 0) {
        var pct = Math.max(0, Math.min(100, Math.round(used / total * 100)));
        var b = storage.querySelector('b');
        var bar = storage.querySelector('em');
        if (b) b.textContent = pct + '%';
        if (bar) bar.style.width = pct + '%';
        storage.className = 'tb-health-card' + (pct >= 90 ? ' danger' : pct >= 75 ? ' warn' : '');
        storage.title = 'Storage: ' + Z.bytes(used) + ' / ' + Z.bytes(total);
      }
    }

    var temp = $('tb-temp');
    if (temp) {
      var tb = temp.querySelector('b');
      if (typeof d.cpu_temp === 'number') {
        var t = Math.round(d.cpu_temp);
        if (tb) tb.textContent = t + '\u00b0C';
        temp.className = 'tb-health-card compact' + (t >= 78 ? ' danger' : t >= 65 ? ' warn' : '');
        temp.title = t >= 78 ? 'APU temperature: hot' : t >= 65 ? 'APU temperature: warm' : 'APU temperature: normal';
      } else {
        if (tb) tb.textContent = 'N/A';
        temp.className = 'tb-health-card compact';
        temp.title = 'APU temperature unavailable on this firmware';
      }
    }
  };

  function quickUpload() {
    Z.switchView('explorer');
    setTimeout(function () {
      var fi = $('file-input');
      if (fi) fi.click();
    }, 120);
  }

  function quickNewFolder() {
    Z.switchView('explorer');
    setTimeout(function () {
      if (!Z.modal || !Z.api) return;
      Z.modal.prompt('New Folder', '').then(function (name) {
        if (!name) return;
        Z.api.mkdir(Z.state.path || '/', name).then(function () {
          Z.notify('Folder created', name, 'ok');
          if (Z.explorer && Z.explorer.nav) Z.explorer.nav(Z.state.path || '/');
        }).catch(function (e) {
          Z.notify('Create folder failed', e.message, 'er');
        });
      });
    }, 120);
  }

  function quickRefresh() {
    var view = Z.state.view;
    if (view === 'explorer' && Z.explorer && Z.explorer.nav) Z.explorer.nav(Z.state.path || '/');
    else if (view === 'dashboard' && Z.dashboard && Z.dashboard.refresh) Z.dashboard.refresh();
    else if (view === 'filemanager' && Z.fileManager && Z.fileManager.init) Z.fileManager.init();
    else if (view === 'downloads' && Z.downloadMgr && Z.downloadMgr.refresh) Z.downloadMgr.refresh();
    else if (view === 'games' && Z.gamesView && Z.gamesView.refresh) Z.gamesView.refresh();
    else if (view === 'settings' && Z.settingsView && Z.settingsView.refresh) Z.settingsView.refresh();
    refreshStats();
    Z.toast('Refreshed', 'ok');
  }

  function focusExplorerSearch() {
    Z.switchView('explorer');
    setTimeout(function () {
      var sr = $('search');
      if (sr) {
        sr.focus();
        sr.select();
      }
    }, 120);
  }

  function openDownloadInput() {
    Z.switchView('downloads');
    setTimeout(function () {
      var input = $('dl-url');
      if (input) input.focus();
    }, 120);
  }

  function openGameInstallInput() {
    if (!Z.featureEnabled || !Z.featureEnabled('pkgInstall')) {
      Z.notify('PKG install disabled', 'This feature will return in a future release.', 'wn');
      return;
    }
    Z.switchView('games');
    setTimeout(function () {
      if (Z.gamesView && Z.gamesView.pickPackage) {
        Z.gamesView.pickPackage();
        return;
      }
      var btn = $('games-pkg-browse-btn');
      if (btn) btn.focus();
    }, 120);
  }

  function copyCurrentPath() {
    var p = Z.state.view === 'explorer' ? (Z.state.path || '/') : '/';
    Z.copyText(p).then(function () {
      Z.notify('Path copied', p, 'ok');
    }).catch(function () {
      Z.modal.prompt('Copy path', p);
    });
  }

  function toggleExplorerInspector() {
    Z.switchView('explorer');
    setTimeout(function () {
      var btn = $('btn-inspector');
      if (btn) btn.click();
    }, 120);
  }

  function networkReset() {
    if (!Z.modal || !Z.api) return;
    Z.modal.confirm('Network Reset', 'Reset FTP network stack?', true).then(function (ok) {
      if (!ok) return;
      Z.api.networkReset().then(function () {
        Z.notify('Network reset', 'FTP stack reset requested.', 'ok');
      }).catch(function (e) {
        Z.notify('Network reset failed', e.message, 'er');
      });
    });
  }

  function commandItems() {
    var items = [
      { group: 'Navigation', title: 'Dashboard', meta: 'System overview and recent activity', scope: 'View', icon: Z.ICO.home, match: 'dashboard home overview stats', run: function () { Z.switchView('dashboard'); } },
      { group: 'Navigation', title: 'Explorer', meta: 'Browse, preview, upload, and manage files', scope: 'View', icon: Z.ICO.folder, match: 'explorer files browse preview', run: function () { Z.switchView('explorer'); } },
      { group: 'Navigation', title: 'Dual Pane Manager', meta: 'Copy between source and destination panes', scope: 'View', icon: Z.ICO.columns, match: 'file manager dual pane copy', run: function () { Z.switchView('filemanager'); } },
      { group: 'Navigation', title: 'Downloads', meta: 'Remote URL and queue manager', scope: 'View', icon: Z.ICO.cloudDown, match: 'downloads queue url remote', run: function () { Z.switchView('downloads'); } },
      { group: 'Navigation', title: 'Games', meta: 'Launch installed apps and repair package visibility', scope: 'View', icon: Z.ICO.gamepad, match: 'games launch installed apps visibility', run: function () { Z.switchView('games'); } },
      { group: 'File Tools', title: 'Upload Files', meta: 'Upload into the current Explorer folder', scope: 'Tool', icon: Z.ICO.upload, match: 'upload files transfer', run: quickUpload },
      { group: 'File Tools', title: 'New Folder', meta: 'Create a folder in Explorer', scope: 'Tool', icon: Z.ICO.newFolder, match: 'mkdir create folder directory', run: quickNewFolder },
      { group: 'File Tools', title: 'Search Current Folder', meta: 'Focus Explorer search', scope: 'Tool', icon: Z.ICO.search, match: 'search filter current folder', run: focusExplorerSearch },
      { group: 'File Tools', title: 'Copy Current Path', meta: Z.state.path || '/', scope: 'Tool', icon: Z.ICO.clipboard, match: 'copy current path clipboard', run: copyCurrentPath },
      { group: 'File Tools', title: 'Toggle Inspector', meta: 'Show or hide the media inspector', scope: 'Tool', icon: Z.ICO.panelRight, match: 'inspector preview panel details', run: toggleExplorerInspector },
      { group: 'Transfers', title: 'New Remote Download', meta: 'Focus URL downloader', scope: 'Queue', icon: Z.ICO.cloudDown, match: 'download url remote queue', run: openDownloadInput },
      { group: 'System', title: 'Refresh Current View', meta: 'Reload data for the active workspace', scope: 'System', icon: Z.ICO.refresh, match: 'refresh reload update', run: quickRefresh },
      { group: 'System', title: 'Settings', meta: 'Theme, display, and explorer preferences', scope: 'System', icon: Z.ICO.settings, match: 'settings preferences theme', run: function () { Z.switchView('settings'); } },
      { group: 'System', title: 'Network Reset', meta: 'Reset FTP network stack', scope: 'System', icon: Z.ICO.wifiOff, match: 'network reset ftp stack', run: networkReset }
    ];
    if (Z.featureEnabled && Z.featureEnabled('pkgInstall')) {
      items.splice(items.length - 3, 0, {
        group: 'Games',
        title: 'Install Package',
        meta: 'Choose a PKG from File Explorer',
        scope: 'Game',
        icon: Z.ICO.archive,
        match: 'install pkg package game',
        run: openGameInstallInput
      });
    }
    return items;
  }

  function openCommandPalette() {
    var palette = $('cmd-palette');
    var btn = $('cmd-btn');
    var input = $('cmd-input');
    if (!palette) return;
    _cmdOpen = true;
    _cmdActiveIndex = 0;
    palette.classList.add('show');
    if (btn) btn.classList.add('active');
    if (input) {
      input.value = '';
      setTimeout(function () { input.focus(); }, 20);
    }
    renderCommandList('');
  }

  function closeCommandPalette() {
    var palette = $('cmd-palette');
    var btn = $('cmd-btn');
    if (palette) palette.classList.remove('show');
    if (btn) btn.classList.remove('active');
    _cmdOpen = false;
  }

  function renderCommandList(query) {
    var list = $('cmd-list');
    if (!list) return;
    query = (query || '').toLowerCase();
    var items = commandItems();
    _cmdVisible = [];
    var groups = {};
    var order = [];

    for (var i = 0; i < items.length; i++) {
      var it = items[i];
      var hay = (it.group + ' ' + it.title + ' ' + it.meta + ' ' + it.scope + ' ' + it.match).toLowerCase();
      if (query && hay.indexOf(query) < 0) continue;
      it._index = _cmdVisible.length;
      _cmdVisible.push(it);
      if (!groups[it.group]) {
        groups[it.group] = [];
        order.push(it.group);
      }
      groups[it.group].push(it);
    }

    if (_cmdActiveIndex >= _cmdVisible.length) _cmdActiveIndex = Math.max(0, _cmdVisible.length - 1);

    if (!_cmdVisible.length) {
      list.innerHTML = '<div class="cmd-empty">No commands found</div>';
      return;
    }

    var html = '';
    for (var g = 0; g < order.length; g++) {
      var groupName = order[g];
      html += '<div class="cmd-group"><div class="cmd-group-title">' + Z.h(groupName) + '</div>';
      var rows = groups[groupName];
      for (var r = 0; r < rows.length; r++) {
        var row = rows[r];
        html += '<button class="cmd-item' + (row._index === _cmdActiveIndex ? ' active' : '') + '" data-cmd-index="' + row._index + '">' +
          '<span class="cmd-item-ico">' + row.icon + '</span>' +
          '<span class="cmd-item-main"><span class="cmd-item-title">' + Z.h(row.title) + '</span>' +
          '<span class="cmd-item-meta">' + Z.h(row.meta) + '</span></span>' +
          '<span class="cmd-item-scope">' + Z.h(row.scope) + '</span>' +
          '</button>';
      }
      html += '</div>';
    }
    list.innerHTML = html;

    var buttons = list.querySelectorAll('.cmd-item');
    for (var b = 0; b < buttons.length; b++) {
      (function (el) {
        el.onclick = function () {
          var idx = parseInt(el.getAttribute('data-cmd-index'), 10);
          runCommand(idx);
        };
      })(buttons[b]);
    }
  }

  function runCommand(index) {
    var cmd = _cmdVisible[index];
    if (!cmd || !cmd.run) return;
    closeCommandPalette();
    cmd.run();
  }

  function wireTopbarTools() {
    var cmdBtn = $('cmd-btn');
    if (cmdBtn) cmdBtn.onclick = function (e) {
      e.stopPropagation();
      if (_cmdOpen) closeCommandPalette();
      else openCommandPalette();
    };

    var up = $('tb-tool-upload');
    if (up) up.onclick = quickUpload;
    var folder = $('tb-tool-folder');
    if (folder) folder.onclick = quickNewFolder;
    var refresh = $('tb-tool-refresh');
    if (refresh) refresh.onclick = quickRefresh;

    var close = $('cmd-close');
    if (close) close.onclick = closeCommandPalette;
    var palette = $('cmd-palette');
    if (palette) palette.addEventListener('click', function (e) {
      if (e.target === palette) closeCommandPalette();
    });
    var input = $('cmd-input');
    if (input) {
      input.oninput = function () {
        _cmdActiveIndex = 0;
        renderCommandList(input.value);
      };
      input.onkeydown = function (e) {
        if (e.key === 'ArrowDown') {
          e.preventDefault();
          _cmdActiveIndex = Math.min(_cmdVisible.length - 1, _cmdActiveIndex + 1);
          renderCommandList(input.value);
        } else if (e.key === 'ArrowUp') {
          e.preventDefault();
          _cmdActiveIndex = Math.max(0, _cmdActiveIndex - 1);
          renderCommandList(input.value);
        } else if (e.key === 'Enter') {
          e.preventDefault();
          runCommand(_cmdActiveIndex);
        } else if (e.key === 'Escape') {
          e.preventDefault();
          closeCommandPalette();
        }
      };
    }
  }

  /* ── Bootstrap ── */
  D.addEventListener('DOMContentLoaded', function () {

    /* Restore saved preferences */
    try {
      var sv = localStorage.getItem('zftpd_view');
      if (sv) Z.state.view = sv;
      var sp = localStorage.getItem('zftpd_path');
      if (sp) Z.state.path = sp;
      var st = localStorage.getItem('zftpd_theme');
      if (st) Z.setTheme(st);
    } catch (e) { }

    /* Brand logo */
    var bl = D.querySelector('.brand-logo');
    if (bl) bl.src = 'assets/zftpd-logo.png';

    /* Nav tab clicks */
    var tabs = D.querySelectorAll('.nav-tab');
    for (var i = 0; i < tabs.length; i++) {
      (function (tab) {
        tab.onclick = function () {
          var view = tab.getAttribute('data-view');
          if (view) Z.switchView(view);
        };
      })(tabs[i]);
    }

    /* Theme selector (mobile) */
    var themeSel = $('theme-select');
    if (themeSel) {
      themeSel.innerHTML = '';
      Z.THEMES.forEach(function (t) {
        var opt = D.createElement('option');
        opt.value = t.id;
        opt.textContent = t.name;
        themeSel.appendChild(opt);
      });
      var cur = D.documentElement.getAttribute('data-theme') || 'ps5';
      themeSel.value = cur;
      themeSel.onchange = function () { Z.setTheme(this.value); };
    }

    /* Theme button (desktop) */
    var themeBtn = $('theme-btn');
    var themeDd = $('theme-dd');
    if (themeBtn && themeDd) {
      themeBtn.onclick = function (e) {
        e.stopPropagation();
        themeDd.classList.toggle('show');
        themeBtn.classList.toggle('open', themeDd.classList.contains('show'));
        /* Build dropdown items */
        themeDd.innerHTML = '<div class="td-hd">Select Theme</div>';
        var curTheme = D.documentElement.getAttribute('data-theme') || 'ps5';
        Z.THEMES.forEach(function (t) {
          var el = D.createElement('div');
          el.className = 'td-item' + (t.id === curTheme ? ' active' : '');
          el.innerHTML = '<div class="td-sw" style="background:' + t.sw + '"></div>' +
            '<div class="td-info"><div class="td-nm">' + t.name + '</div><div class="td-ds">' + t.desc + '</div></div>' +
            '<span class="td-ck">' + Z.ICO.check + '</span>';
          el.onclick = function () {
            Z.setTheme(t.id);
            themeDd.classList.remove('show');
            themeBtn.classList.remove('open');
          };
          themeDd.appendChild(el);
        });
      };
    }

    /* Notification bell toggle */
    var notifBtn = $('tb-notif-btn');
    var notifDd = $('tb-notif-dd');
    if (notifBtn && notifDd) {
      notifBtn.onclick = function (e) {
        e.stopPropagation();
        notifDd.classList.toggle('show');
        /* Close theme dropdown */
        if (themeDd) themeDd.classList.remove('show');
        if (themeBtn) themeBtn.classList.remove('open');
        /* Re-render to update relative times */
        if (notifDd.classList.contains('show')) _renderNotifications();
      };
    }

    /* Close all dropdowns on outside click */
    D.addEventListener('click', function () {
      if (themeDd) themeDd.classList.remove('show');
      if (themeBtn) themeBtn.classList.remove('open');
      if (notifDd) notifDd.classList.remove('show');
    });

    /* Escape key */
    D.addEventListener('keydown', function (e) {
      if ((e.ctrlKey || e.metaKey) && String(e.key || '').toLowerCase() === 'k') {
        e.preventDefault();
        if (_cmdOpen) closeCommandPalette();
        else openCommandPalette();
        return;
      }
      if (e.key === 'Escape') {
        if (_cmdOpen) {
          closeCommandPalette();
          return;
        }
        if (themeDd) themeDd.classList.remove('show');
        if (themeBtn) themeBtn.classList.remove('open');
        if (notifDd) notifDd.classList.remove('show');
      }
    });

    /* Drag-and-drop upload */
    var _dd = 0;
    D.addEventListener('dragenter', function (e) {
      e.preventDefault();
      _dd++;
      var drop = $('drop-overlay');
      if (drop) drop.classList.add('on');
    });
    D.addEventListener('dragover', function (e) { e.preventDefault(); });
    D.addEventListener('dragleave', function (e) {
      e.preventDefault();
      _dd = Math.max(0, _dd - 1);
      if (!_dd) {
        var drop = $('drop-overlay');
        if (drop) drop.classList.remove('on');
      }
    });
    D.addEventListener('drop', function (e) {
      e.preventDefault();
      _dd = 0;
      var drop = $('drop-overlay');
      if (drop) drop.classList.remove('on');
      if (Z.explorer && Z.explorer.upload) {
        Z.explorer.upload(e.dataTransfer.files);
      }
    });

    var dc = $('drop-close');
    if (dc) dc.onclick = function () {
      var drop = $('drop-overlay');
      if (drop) drop.classList.remove('on');
    };

    wireTopbarTools();
    Z.updateTopbarContext();

    /* Stats polling */
    Z.state.statsTimer = setInterval(refreshStats, 15000);
    refreshStats();

    /* Initialize the active view */
    Z.switchView(Z.state.view);

    /* Preload root directory */
    Z.api.list('/').catch(function () { });
  });

})(ZFTPD);
