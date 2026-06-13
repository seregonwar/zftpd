/* ══ DOWNLOAD MANAGER VIEW ════════════════════════════════════════════════
 * URL input bar (paste/clear/validate), rich download cards with speed
 * sparklines, queue management, and searchable history with retry.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var ICO = Z.ICO;

  var dlm = {};
  var _downloads = [];       /* active downloads (local mirror) */
  var _history = [];         /* completed/failed (persisted)   */
  var _speedSamples = {};    /* { serverId: [s1,s2,...s10] }  */
  var _pollTimer = null;
  var _destPath = '/data';
  var _MAX_SPARK = 10;

  /* ── PERSISTENCE ── */
  var HIST_KEY = 'zftpd_dl_history';
  function loadHistory() {
    try {
      var raw = localStorage.getItem(HIST_KEY);
      _history = raw ? JSON.parse(raw) : [];
    } catch (e) { _history = []; }
  }
  function saveHistory() {
    try { localStorage.setItem(HIST_KEY, JSON.stringify(_history)); } catch (e) { }
  }
  loadHistory();
  try {
    _destPath = localStorage.getItem('zftpd_dl_dest') || _destPath;
  } catch (e) { }

  /* ═══════════════════════════════════════════════════════════════════════
   * REFRESH — called when view becomes active
   * ═══════════════════════════════════════════════════════════════════════ */
  dlm.refresh = function () {
    var destEl = $('dl-dest-path');
    if (destEl) destEl.textContent = _destPath;
    renderActive();
    renderHistory();
    startPolling();
  };

  /* ═══════════════════════════════════════════════════════════════════════
   * START DOWNLOAD — from input bar or retry
   * ═══════════════════════════════════════════════════════════════════════ */
  dlm.start = function () {
    var input = $('dl-url');
    if (!input) return;
    var url = (input.value || '').trim();
    if (!url) {
      Z.toast('Enter a URL first', 'wn');
      input.focus();
      return;
    }
    _startUrl(url);
    input.value = '';
    /* Hide validation + clear button */
    var valEl = $('dl-validation');
    if (valEl) valEl.style.display = 'none';
    var cb = $('dl-clear-btn');
    if (cb) cb.classList.remove('visible');
  };

  /* ── Retry a failed download from history ── */
  dlm.retry = function (url) {
    _startUrl(url);
  };

  function _startUrl(url) {
    var type = detectUrlType(url);
    var displayName = extractFilename(url) || 'download';

    Z.toast('Starting: ' + displayName, 'ok');

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
      startTime: Date.now(),
      serverId: null,
      queuePos: 0
    };
    _downloads.push(entry);

    /* Set queue position */
    _updateQueuePositions();
    renderActive();

    Z.api.downloadStart(url, _destPath).then(function (d) {
      entry.status = 'downloading';
      if (d && d.id) entry.serverId = d.id;
      if (d && d.name) entry.name = d.name;
      if (d && d.size) entry.size = d.size;
      _speedSamples[entry.serverId || entry.id] = [];
      _updateQueuePositions();
      renderActive();
      if (Z.onDownloadStarted) Z.onDownloadStarted();
    }).catch(function (e) {
      entry.status = 'error';
      entry.error = e.message || 'Start failed';
      renderActive();
      _moveToHistory(entry, 'failed');
      Z.notify('Download failed', displayName + ': ' + (e.message || 'unknown'), 'er');
    });
  }

  /* ── Update queue position for all queued downloads ── */
  function _updateQueuePositions() {
    var queued = [];
    for (var i = 0; i < _downloads.length; i++) {
      var d = _downloads[i];
      if (d.status === 'starting' || d.status === 'queued') {
        queued.push(d);
      }
    }
    for (var j = 0; j < queued.length; j++) {
      queued[j].queuePos = j + 1;
    }
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * DESTINATION PICKER — uses shared Z.modal.folderPicker
   * ═══════════════════════════════════════════════════════════════════════ */
  dlm.selectDest = function () {
    Z.modal.folderPicker('Choose download destination', _destPath).then(function (path) {
      if (path !== null && path !== undefined) {
        _destPath = path;
        try { localStorage.setItem('zftpd_dl_dest', _destPath); } catch (e) { }
        var el = $('dl-dest-path');
        if (el) el.textContent = _destPath;
      }
    });
  };

  /* ═══════════════════════════════════════════════════════════════════════
   * PASTE FROM CLIPBOARD
   * ═══════════════════════════════════════════════════════════════════════ */
  dlm.paste = function () {
    var input = $('dl-url');
    if (!input) return;
    if (navigator.clipboard && navigator.clipboard.readText) {
      navigator.clipboard.readText().then(function (text) {
        if (text) {
          input.value = text.trim();
          input.focus();
          _validateUrl(text.trim());
        }
      }).catch(function () {
        Z.toast('Clipboard access denied', 'wn');
      });
    } else {
      /* Fallback: focus input so user can Ctrl+V */
      input.focus();
      Z.toast('Press Ctrl+V to paste', 'ok');
    }
  };

  /* ── URL validation ── */
  function _validateUrl(url) {
    var valEl = $('dl-validation');
    if (!valEl) return;
    if (!url) {
      valEl.style.display = 'none';
      return;
    }
    var type = detectUrlType(url);
    if (type === 'http') {
      /* Basic URL check */
      if (!/^https?:\/\//i.test(url) && !/^ftp:\/\//i.test(url)) {
        valEl.textContent = 'Add https:// for direct links';
        valEl.className = 'dl-validation dl-val-wn';
        valEl.style.display = 'block';
        return;
      }
    }
    valEl.textContent = 'Detected: ' + typeLabel(type);
    valEl.className = 'dl-validation dl-val-ok';
    valEl.style.display = 'block';
  }

  function typeLabel(type) {
    switch (type) {
      case 'magnet':    return 'Magnet link';
      case 'gdrive':    return 'Google Drive';
      case 'mega':      return 'MEGA';
      case 'mediafire': return 'MediaFire';
      case '1fichier':  return '1fichier';
      default:          return 'Direct URL';
    }
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * RENDER ACTIVE DOWNLOADS
   * ═══════════════════════════════════════════════════════════════════════ */
  function renderActive() {
    var wrap = $('dl-active');
    if (!wrap) return;
    wrap.innerHTML = '';

    var countEl = $('dl-active-count');
    var active = _downloads.filter(function (d) { return d.status !== 'done' && d.status !== 'error'; });
    if (countEl) countEl.textContent = active.length;

    if (!_downloads.length) {
      wrap.innerHTML = '<div class="dl-empty">' +
        '<div class="dl-empty-icon">' + ICO.cloudDown + '</div>' +
        '<div class="dl-empty-title">No active downloads</div>' +
        '<div class="dl-empty-sub">Paste a URL or magnet link above</div>' +
        '</div>';
      return;
    }

    for (var i = 0; i < _downloads.length; i++) {
      var dl = _downloads[i];
      wrap.appendChild(_buildCard(dl));
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

  /* ── Build a single download card ── */
  function _buildCard(dl) {
    var card = D.createElement('div');
    card.className = 'dl-card';
    if (dl.status === 'error') card.classList.add('dl-card-err');
    if (dl.status === 'done') card.classList.add('dl-card-ok');

    var speedStr = dl.speed > 0 ? Z.bps(dl.speed) : '\u2014';
    var sizeStr = fmtSizePair(dl.downloaded, dl.size);
    var etaStr = calcETA(dl);
    var statusText = statusLabel(dl);
    var statusCls = statusClass(dl);

    /* If queued, show queue position */
    var queueBadge = '';
    if ((dl.status === 'starting' || dl.status === 'queued') && dl.queuePos > 0) {
      queueBadge = '<span class="dl-queue-pos">#' + dl.queuePos + ' in queue</span>';
    }

    /* Speed sparkline */
    var sparkHtml = _renderSparkline(dl);

    card.innerHTML =
      /* ── TOP ROW: icon + info + status ── */
      '<div class="dl-card-top">' +
        '<div class="dl-card-icon">' + typeIcon(dl.type) + '</div>' +
        '<div class="dl-card-info">' +
          '<div class="dl-card-name" title="' + escAttr(dl.name) + '">' + escHtml(dl.name) + '</div>' +
          '<div class="dl-card-url">' + escHtml(dl.url).substring(0, 80) + (dl.url.length > 80 ? '\u2026' : '') + '</div>' +
        '</div>' +
        '<div class="dl-card-status ' + statusCls + '">' + statusText + queueBadge + '</div>' +
      '</div>' +
      /* ── STATS ROW ── */
      '<div class="dl-card-stats">' +
        '<div class="dl-stat">' +
          '<div class="dl-stat-row"><span class="dl-stat-value">' + speedStr + '</span>' +
          sparkHtml + '</div>' +
          '<div class="dl-stat-label">Speed</div>' +
        '</div>' +
        '<div class="dl-stat dl-stat-center">' +
          '<div class="dl-stat-value">' + sizeStr + '</div>' +
          '<div class="dl-stat-label">Size</div>' +
        '</div>' +
        '<div class="dl-stat dl-stat-right">' +
          '<div class="dl-stat-value">' + etaStr + '</div>' +
          '<div class="dl-stat-label">ETA</div>' +
        '</div>' +
      '</div>' +
      /* ── PROGRESS ── */
      '<div class="dl-card-progress">' +
        '<div class="dl-card-progress-fill ' + statusCls + '" style="width:' + dl.progress + '%"></div>' +
      '</div>' +
      /* ── FOOTER: percentage + actions ── */
      '<div class="dl-card-footer">' +
        '<span class="dl-card-pct">' + dl.progress + '%</span>' +
        '<div class="dl-card-actions">' +
          (dl.status === 'downloading' ? '<button class="btn btn-sm" data-action="pause" data-id="' + dl.id + '">' + ICO.pause + ' Pause</button>' : '') +
          (dl.status === 'paused' ? '<button class="btn btn-sm" data-action="resume" data-id="' + dl.id + '">' + ICO.play + ' Resume</button>' : '') +
          '<button class="btn btn-sm btn-dg" data-action="cancel" data-id="' + dl.id + '">' + ICO.xcancel + ' Cancel</button>' +
        '</div>' +
      '</div>';

    return card;
  }

  /* ── Speed sparkline (div-based mini bar chart) ── */
  function _renderSparkline(dl) {
    var sid = dl.serverId || dl.id;
    var samples = _speedSamples[sid];
    if (!samples || !samples.length) return '';

    /* Find max for scaling */
    var maxSpeed = 1;
    for (var i = 0; i < samples.length; i++) {
      if (samples[i] > maxSpeed) maxSpeed = samples[i];
    }

    var html = '<span class="dl-sparkline">';
    for (var j = 0; j < samples.length; j++) {
      var h = Math.max(2, Math.round((samples[j] / maxSpeed) * 20));
      var cls = 'dl-spark-bar';
      if (dl.status === 'downloading' && j === samples.length - 1) cls += ' dl-spark-live';
      html += '<span class="' + cls + '" style="height:' + h + 'px"></span>';
    }
    html += '</span>';
    return html;
  }

  /* ── Record speed sample ── */
  function _recordSpeed(dl, speed) {
    var sid = dl.serverId || dl.id;
    if (!_speedSamples[sid]) _speedSamples[sid] = [];
    var arr = _speedSamples[sid];
    arr.push(speed);
    if (arr.length > _MAX_SPARK) arr.shift();
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * RENDER HISTORY
   * ═══════════════════════════════════════════════════════════════════════ */
  function renderHistory() {
    var wrap = $('dl-history');
    if (!wrap) return;
    wrap.innerHTML = '';

    var searchInput = $('dl-history-search');
    var query = searchInput ? (searchInput.value || '').trim().toLowerCase() : '';

    var filtered = _history;
    if (query) {
      filtered = _history.filter(function (h) {
        return (h.name && h.name.toLowerCase().indexOf(query) >= 0) ||
               (h.url && h.url.toLowerCase().indexOf(query) >= 0);
      });
    }

    if (!filtered.length) {
      if (query) {
        wrap.innerHTML = '<div class="dl-empty dl-empty-sm">No results for "' + escHtml(query) + '"</div>';
      } else {
        wrap.innerHTML = '<div class="dl-empty dl-empty-sm">No download history yet</div>';
      }
      return;
    }

    /* Group by date */
    var groups = groupByDate(filtered);
    var groupNames = Object.keys(groups);

    for (var g = 0; g < groupNames.length; g++) {
      var label = groupNames[g];
      var items = groups[label];

      var grp = D.createElement('div');
      grp.className = 'dl-history-group';
      grp.innerHTML = '<div class="dl-history-group-label">' + label + '</div>';

      for (var i = 0; i < items.length; i++) {
        var h = items[i];
        var row = D.createElement('div');
        row.className = 'dl-history-item';
        row.innerHTML =
          '<div class="dl-history-icon">' + typeIcon(h.type || 'http') + '</div>' +
          '<div class="dl-history-info">' +
            '<div class="dl-history-name">' + escHtml(h.name || 'download') + '</div>' +
            '<div class="dl-history-meta">' +
              Z.bytes(h.size || 0) + ' \u00B7 ' +
              formatTime(h.date) +
            '</div>' +
          '</div>' +
          '<div class="dl-history-status ' + (h.status === 'complete' ? 'dl-hs-ok' : 'dl-hs-err') + '">' +
            (h.status === 'complete' ? 'Complete' : h.status === 'failed' ? 'Failed' : h.status === 'cancelled' ? 'Cancelled' : h.status) +
          '</div>' +
          (h.status !== 'complete' && h.url ? '<button class="dl-history-retry" title="Retry" data-retry-url="' + escAttr(h.url) + '">' + ICO.refresh + '</button>' : '');

        grp.appendChild(row);
      }

      wrap.appendChild(grp);
    }

    /* Wire retry buttons */
    var retryBtns = wrap.querySelectorAll('.dl-history-retry');
    for (var r = 0; r < retryBtns.length; r++) {
      (function (btn) {
        btn.onclick = function () {
          var url = btn.getAttribute('data-retry-url');
          if (url) dlm.retry(url);
        };
      })(retryBtns[r]);
    }
  }

  /* ── Group history entries by date ── */
  function groupByDate(items) {
    var now = new Date();
    var today = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
    var yesterday = today - 86400000;
    var weekAgo = today - 7 * 86400000;

    var groups = {};

    for (var i = 0; i < items.length; i++) {
      var item = items[i];
      var itemDay = new Date(item.date).getTime();
      var label;

      if (itemDay >= today) {
        label = 'Today';
      } else if (itemDay >= yesterday) {
        label = 'Yesterday';
      } else if (itemDay >= weekAgo) {
        label = 'This Week';
      } else {
        label = 'Older';
      }

      if (!groups[label]) groups[label] = [];
      groups[label].push(item);
    }
    return groups;
  }

  function formatTime(ts) {
    if (!ts) return '\u2014';
    var d = new Date(ts);
    var h = d.getHours();
    var m = d.getMinutes();
    return (h < 10 ? '0' : '') + h + ':' + (m < 10 ? '0' : '') + m;
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * CLEAR HISTORY
   * ═══════════════════════════════════════════════════════════════════════ */
  dlm.clearHistory = function () {
    Z.modal.confirm('Clear History', 'Remove all download history entries?', true).then(function (ok) {
      if (ok) {
        _history = [];
        saveHistory();
        renderHistory();
        Z.toast('History cleared', 'ok');
      }
    });
  };

  /* ═══════════════════════════════════════════════════════════════════════
   * ACTIONS: pause / resume / cancel
   * ═══════════════════════════════════════════════════════════════════════ */
  function handleAction(action, id) {
    var dl = _downloads.filter(function (d) { return d.id === id; })[0];
    if (!dl) return;

    if (action === 'cancel') {
      if (dl.serverId) {
        Z.api.downloadCancel(dl.serverId).catch(function () { });
      }
      dl.status = 'error';
      dl.error = 'Cancelled';
      delete _speedSamples[dl.serverId || dl.id];
      _moveToHistory(dl, 'cancelled');
      _downloads = _downloads.filter(function (d) { return d.id !== id; });
      _updateQueuePositions();
      renderActive();
      renderHistory();
      Z.toast('Download cancelled', 'wn');
    } else if (action === 'pause') {
      if (dl.serverId) {
        Z.api.downloadPause(dl.serverId).catch(function () { });
      }
      dl.status = 'paused';
      renderActive();
    } else if (action === 'resume') {
      if (dl.serverId) {
        /* Backend toggles pause state — calling again resumes */
        Z.api.downloadPause(dl.serverId).catch(function () { });
      }
      dl.status = 'downloading';
      renderActive();
    }
  }

  /* ── Move a download to history ── */
  function _moveToHistory(dl, status) {
    _history.unshift({
      name: dl.name,
      url: dl.url,
      type: dl.type,
      size: dl.size || dl.downloaded || 0,
      date: Date.now(),
      status: status
    });
    /* Keep max 100 entries */
    if (_history.length > 100) _history.length = 100;
    saveHistory();
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * POLLING — fetch progress from backend
   * ═══════════════════════════════════════════════════════════════════════ */
  function startPolling() {
    if (_pollTimer) clearInterval(_pollTimer);
    _pollTimer = setInterval(_pollTick, 1500);
  }

  function stopPolling() {
    if (_pollTimer) { clearInterval(_pollTimer); _pollTimer = null; }
  }

  function _pollTick() {
    var active = _downloads.filter(function (d) {
      return d.status === 'downloading' || d.status === 'paused' || d.status === 'starting';
    });
    if (!active.length) return;

    Z.api.downloadStatus().then(function (data) {
      if (!data || !Array.isArray(data.downloads)) return;
      var serverItems = data.downloads;

      for (var i = 0; i < serverItems.length; i++) {
        var sd = serverItems[i];
        /* Find local entry by serverId */
        var local = null;
        for (var j = 0; j < _downloads.length; j++) {
          if (_downloads[j].serverId === sd.id) {
            local = _downloads[j];
            break;
          }
        }
        if (!local) continue;

        /* Update local state from server */
        local.progress = sd.progress || 0;
        local.downloaded = sd.downloaded || 0;
        if (sd.total_size) local.size = sd.total_size;
        local.speed = sd.speed || 0;
        if (sd.name && sd.name !== local.name) local.name = sd.name;

        /* Record speed for sparkline */
        if (local.speed > 0) _recordSpeed(local, local.speed);

        /* Handle paused state (string from backend) */
        var isPaused = (sd.paused === true || sd.paused === 'true');
        if (isPaused && local.status !== 'paused') {
          local.status = 'paused';
        } else if (!isPaused && local.status === 'paused') {
          local.status = 'downloading';
        }

        /* Handle done/error (string from backend) */
        var isDone = (sd.done === true || sd.done === 'true');
        if (isDone) {
          if (sd.error) {
            local.status = 'error';
            local.error = sd.error;
            _moveToHistory(local, 'failed');
            Z.toast('Download failed: ' + local.name, 'er');
          } else {
            local.status = 'done';
            _moveToHistory(local, 'complete');
            Z.toast('Downloaded: ' + local.name, 'ok');
          }
          _downloads = _downloads.filter(function (d) { return d.id !== local.id; });
          _updateQueuePositions();
          /* Clean up speed samples */
          delete _speedSamples[local.serverId || local.id];
        }
      }
      renderActive();
      renderHistory();

      /* Stop polling if nothing left */
      if (!_downloads.length) stopPolling();
    }).catch(function () { });
  }

  /* ═══════════════════════════════════════════════════════════════════════
   * HELPERS
   * ═══════════════════════════════════════════════════════════════════════ */

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

  /* ── Status label ── */
  function statusLabel(dl) {
    switch (dl.status) {
      case 'downloading': return 'Downloading';
      case 'starting':
      case 'queued':      return 'Queued';
      case 'paused':      return 'Paused';
      case 'done':        return 'Complete';
      case 'error':       return 'Failed';
      default:            return dl.status;
    }
  }

  /* ── Status CSS class ── */
  function statusClass(dl) {
    switch (dl.status) {
      case 'downloading': return 'dl-st-active';
      case 'starting':
      case 'queued':      return 'dl-st-queued';
      case 'paused':      return 'dl-st-paused';
      case 'done':        return 'dl-st-done';
      case 'error':       return 'dl-st-err';
      default:            return '';
    }
  }

  /* ── Type icon ── */
  function typeIcon(type) {
    switch (type) {
      case 'magnet':    return ICO.link;
      case 'gdrive':    return ICO.cloud;
      case 'mega':      return ICO.cloud;
      case 'mediafire': return ICO.cloud;
      case '1fichier':  return ICO.cloud;
      default:          return ICO.cloudDown;
    }
  }

  /* ── Format downloaded / total size pair ── */
  function fmtSizePair(down, total) {
    if (total > 0) {
      return Z.bytes(down) + ' / ' + Z.bytes(total);
    }
    return Z.bytes(down);
  }

  /* ── Calculate ETA ── */
  function calcETA(dl) {
    if (dl.speed <= 0) return '\u2014';
    if (dl.size <= 0) return '\u2014';
    var remaining = dl.size - dl.downloaded;
    if (remaining <= 0) return 'Done';
    return Z.duration(remaining / dl.speed);
  }

  /* ── Basic HTML escape ── */
  function escHtml(s) {
    if (!s) return '';
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  function escAttr(s) {
    if (!s) return '';
    return String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  /* ── Wire input bar events ── */
  function wireInputBar() {
    var input = $('dl-url');
    if (!input) return;
    var clearBtn = $('dl-clear-btn');
    var valEl = $('dl-validation');

    /* Toggle clear button visibility */
    function updateClearBtn() {
      var hasText = (input.value || '').trim().length > 0;
      if (clearBtn) {
        if (hasText) clearBtn.classList.add('visible');
        else clearBtn.classList.remove('visible');
      }
    }

    /* Enter key → start download */
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') {
        e.preventDefault();
        dlm.start();
      }
    });

    /* Validate + toggle clear on input */
    var debouncedValidate = Z.debounce(function () {
      _validateUrl((input.value || '').trim());
    }, 400);
    input.addEventListener('input', function () {
      updateClearBtn();
      debouncedValidate();
    });

    /* On focus, check initial state */
    input.addEventListener('focus', updateClearBtn);
  }

  /* ── Wire history search ── */
  function wireHistorySearch() {
    var searchInput = $('dl-history-search');
    if (!searchInput) return;
    var debouncedRender = Z.debounce(renderHistory, 300);
    searchInput.addEventListener('input', debouncedRender);
  }

  /* Bootstrap */
  D.addEventListener('DOMContentLoaded', function () {
    wireInputBar();
    wireHistorySearch();
  });

  Z.downloadMgr = dlm;

})(ZFTPD);
