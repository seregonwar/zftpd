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
        if (d.done && !d.error && !Z.state[nKey]) {
          Z.state[nKey] = true;
          Z.notify('Download complete', d.filename || d.url, 'ok');
        }
        if (d.error && !Z.state[nKey]) {
          Z.state[nKey] = true;
          Z.notify('Download failed', (d.filename || d.url) + ': ' + d.error_msg, 'er');
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
    }).catch(function () { });
  }

  /* ── Bootstrap ── */
  D.addEventListener('DOMContentLoaded', function () {

    /* Restore saved preferences */
    try {
      var sv = localStorage.getItem('zftpd_view');
      if (sv) Z.state.view = sv;
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
      if (e.key === 'Escape') {
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

    /* Stats polling */
    Z.state.statsTimer = setInterval(refreshStats, 15000);

    /* Initialize the active view */
    Z.switchView(Z.state.view);

    /* Preload root directory */
    Z.api.list('/').catch(function () { });
  });

})(ZFTPD);
