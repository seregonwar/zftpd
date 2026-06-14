/* ══ DASHBOARD VIEW — PS5 Hub Style ═══════════════════════════════════════
 * Homepage with game cards, quick actions, stats widgets, recent files.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var ICO = Z.ICO;

  var dashboard = {};
  var _games = [];
  var _uniqueGames = [];
  var _recentFiles = [];
  dashboard.heroSet = false;

  /* ── Refresh dashboard data ── */
  dashboard.refresh = function () {
    dashboard.heroSet = false;
    var hc = $('dash-hero-container');
    if (hc) { hc.innerHTML = ''; hc.style.display = 'none'; }
    loadGames();
    loadRecentFiles();
    loadStats();
  };

  /* ── Load games — installed titles only ─────── */
  function loadGames() {
    _games = [];
    _uniqueGames = [];
    dashboard.heroSet = false;

    var row = $('dash-games');
    if (row) {
      row.innerHTML = '';
      for (var s = 0; s < 4; s++) {
        row.innerHTML += '<div class="dash-game-card shimmer" style="border-color:transparent"><div class="dash-game-cover"></div><div class="dash-game-info"><div class="dash-game-title" style="height:14px;background:var(--bd2);border-radius:4px;width:80%"></div><div class="dash-game-id" style="height:10px;background:var(--bd);border-radius:4px;width:40%;margin-top:6px"></div></div></div>';
      }
    }

    Z.api.gamesInstalled().then(function (res) {
      _games = (res && res.entries && Array.isArray(res.entries)) ? res.entries : [];
      computeInstalledGames();
    }).catch(function () {
      _games = [];
      _uniqueGames = [];
      renderGames('Installed list unavailable');
    });
  }

  function jsq(s) {
    return String(s == null ? '' : s).replace(/\\/g, '\\\\').replace(/'/g, "\\'");
  }

  function computeInstalledGames() {
    _uniqueGames = [];
    for (var i = 0; i < _games.length; i++) {
      var g = _games[i];
      var id = g.id || '';
      var path = g.path || '';
      var name = g.name || id || 'Installed title';
      _uniqueGames.push({
        fingerprint: id || path || name,
        id: id,
        name: name,
        path: path,
        source: g.source || '',
        hasIcon: !!g.has_icon
      });
    }
    renderGames();
  }

  /* ── Render game cards row ── */
  function renderGames(emptyText) {
    var row = $('dash-games');
    if (!row) return;
    row.innerHTML = '';

    if (!_uniqueGames.length) {
      row.innerHTML = '<div class="dash-game-card" style="width:280px;display:flex;align-items:center;justify-content:center;padding:20px;color:var(--tx3);font-size:12px;">' +
        ICO.gamepad + ' <span style="margin-left:8px">' + Z.h(emptyText || 'No installed games found') + '</span></div>';
      return;
    }

    for (var i = 0; i < _uniqueGames.length && i < 20; i++) {
      var ug = _uniqueGames[i];
      var card = D.createElement('div');
      card.className = 'dash-game-card';

      card.innerHTML = '<div class="dash-game-cover placeholder">' + ICO.gamepad + '</div>' +
        '<div class="dash-game-info">' +
          '<div class="dash-game-title" title="' + Z.h(ug.name) + '">' + Z.h(ug.name) + '</div>' +
          '<div class="dash-game-id">' + Z.h(ug.id || ug.path || 'Installed') + '</div>' +
        '</div>';

      (function (fp) {
        card.onclick = function () {
           dashboard.playGame(fp);
        };
      })(ug.fingerprint);

      row.appendChild(card);
    }
  }
  dashboard.playGame = function(fp) {
    var ug = null;
    for (var i = 0; i < _uniqueGames.length; i++) {
        if (_uniqueGames[i].fingerprint === fp) { ug = _uniqueGames[i]; break; }
    }
    if (!ug) return;

    var d = document.getElementById('dash-action-modal');
    if (d) d.parentNode.removeChild(d);

    var html = 
      '<div id="dash-action-modal" class="dash-loc-dropdown">' +
        '<div class="dash-loc-box">' +
          '<div class="dash-loc-box-title">' +
            '<div class="dash-loc-heading">' +
              '<div class="dash-loc-cover placeholder">' + ICO.gamepad + '</div>' +
              '<div class="dash-loc-title-text"><b title="' + Z.h(ug.name) + '">' + Z.h(ug.name) + '</b><span>' + Z.h(ug.id || 'Installed title') + '</span></div>' +
            '</div>' +
            '<button class="close-btn" onclick="var d=document.getElementById(\'dash-action-modal\');if(d)d.parentNode.removeChild(d);">' + ICO.x + '</button>' +
          '</div>' +
          '<div class="dash-loc-box-sub">' + Z.h(ug.path || 'Installed app') + '</div>' +
          '<div class="dash-loc-actions">';

    if (ug.id || ug.path) {
      html += 
        '<button class="dash-loc-primary" onclick="ZFTPD.dashboard.launchGame(\'' + jsq(fp) + '\')">' +
          ICO.gamepad + '<span>Launch Game</span>' +
        '</button>';
    }

    if (ug.path) {
      html +=
        '<button class="dash-loc-secondary" onclick="ZFTPD.dashboard.navTo(\'' + Z.E(ug.path) + '\')">' +
          ICO.folder + '<span>Browse App Folder</span>' +
        '</button>';
    }
    
    html += '</div></div></div>';
    var div = document.createElement('div');
    div.innerHTML = html;
    document.body.appendChild(div.firstChild);
  };

  dashboard.launchGame = function(fp) {
    var ug = null;
    for (var i = 0; i < _uniqueGames.length; i++) {
      if (_uniqueGames[i].fingerprint === fp) { ug = _uniqueGames[i]; break; }
    }
    if (!ug) return;
    Z.api.gameLaunch(ug.id, ug.path).then(function(j){
      Z.toast((j && j.message) || 'Launch signal sent', (j && j.status) === 'ok' || (j && j.ok === true) ? 'ok' : 'wn');
      var m = document.getElementById('dash-action-modal');
      if (m) m.parentNode.removeChild(m);
    }).catch(function(){
      Z.toast('Launch failed', 'er');
    });
  };

  dashboard.navTo = function(pathUrl) {
    var path = decodeURIComponent(pathUrl);
    var d = document.getElementById('dash-action-modal');
    if (d) d.parentNode.removeChild(d);
    
    Z.switchView('explorer');
    setTimeout(function() { if(Z.explorer) Z.explorer.nav(Z.parent(path) || '/'); }, 100);
  };

  /* ── Load recent files ── */
  function loadRecentFiles() {
    Z.api.list(Z.state.path || '/').then(function (d) {
      var entries = (d && Array.isArray(d.entries)) ? d.entries : [];
      _recentFiles = entries.filter(function (e) {
          return e.type !== 'directory' && e.name.indexOf('._') !== 0;
        })
        .sort(function (a, b) { return (b.mtime || 0) - (a.mtime || 0); })
        .slice(0, 8);
      renderRecent();
    }).catch(function () { });
  }

  function renderRecent() {
    var list = $('dash-recent-list');
    if (!list) return;
    list.innerHTML = '';

    if (!_recentFiles.length) {
      list.innerHTML = '<div style="padding:16px;color:var(--tx3);font-size:12px;text-align:center;">No files found</div>';
      return;
    }

    for (var i = 0; i < _recentFiles.length; i++) {
      var f = _recentFiles[i];
      var cat = Z.fileCategory(f.name, false);
      var path = Z.join(Z.state.path || '/', f.name);

      var item = D.createElement('div');
      item.className = 'dash-recent-item';
      item.innerHTML =
        '<div class="dash-recent-ico fi-' + cat + '">' + ICO.file + '</div>' +
        '<div class="dash-recent-name" title="' + f.name + '">' + f.name + '</div>' +
        '<div class="dash-recent-size">' + Z.bytes(f.size || 0) + '</div>';

      (function (p) {
        item.onclick = function () {
          Z.download(Z.api.downloadUrl(p));
        };
      })(path);

      list.appendChild(item);
    }
  }

  /* ── Load & render stats ── */
  function loadStats() {
    Z.api.stats('/').then(function (d) {
      dashboard.updateStats(d);
    }).catch(function () { });
  }

  dashboard.updateStats = function (d) {
    if (!d || typeof d !== 'object') return;

    /* Disk Ring */
    var hasDisk = (typeof d.disk_used === 'number') && (typeof d.disk_total === 'number') && d.disk_total > 0;
    if (hasDisk) {
      var pct = Math.min(100, Math.floor(d.disk_used / d.disk_total * 100));
      var txt = $('dash-disk-txt');
      if (txt) txt.textContent = pct + '%';
      var sub = $('dash-disk-sub');
      if (sub) sub.textContent = Z.bytes(d.disk_used) + ' / ' + Z.bytes(d.disk_total);
      
      var ring = $('dash-disk-ring');
      if (ring) {
        var offset = 220 - (pct / 100) * 220;
        ring.style.strokeDashoffset = offset;
        ring.className.baseVal = 'dash-stat-ring-fg' + (pct > 85 ? ' cr' : pct > 70 ? ' wn' : '');
      }
    }

    /* Temp Ring */
    if (typeof d.cpu_temp === 'number') {
      var tTxt = $('dash-temp-txt');
      if (tTxt) tTxt.textContent = d.cpu_temp + '\u00b0C';
      var tSub = $('dash-temp-sub');
      if (tSub) tSub.textContent = d.cpu_temp > 65 ? 'Running hot' : 'Normal';
      
      var tRing = $('dash-temp-ring');
      if (tRing) {
        var tpct = Math.min(100, d.cpu_temp);
        var toffset = 220 - (tpct / 100) * 220;
        tRing.style.strokeDashoffset = Math.max(0, toffset);
        tRing.className.baseVal = 'dash-stat-ring-fg' + (d.cpu_temp > 65 ? ' wn' : '');
      }
    } else {
      var ntTxt = $('dash-temp-txt');
      if (ntTxt) ntTxt.textContent = 'N/A';
      var ntSub = $('dash-temp-sub');
      if (ntSub) ntSub.textContent = 'Unavailable on this firmware';
      var ntRing = $('dash-temp-ring');
      if (ntRing) {
        ntRing.style.strokeDashoffset = 220;
        ntRing.className.baseVal = 'dash-stat-ring-fg';
      }
    }
  };

  /* ── Quick action handlers ── */
  dashboard.goExplorer = function () { Z.switchView('explorer'); };
  dashboard.goFileManager = function () { Z.switchView('filemanager'); };
  dashboard.goDownloads = function () { Z.switchView('downloads'); };
  dashboard.doUpload = function () {
    Z.switchView('explorer');
    setTimeout(function () {
      var fi = $('file-input');
      if (fi) fi.click();
    }, 200);
  };

  /* ── See All Games List Modal ── */
  dashboard.showGamesList = function () {
    if (Z.modal) {
      var html = '<div class="dash-library-grid">';
      for (var i = 0; i < _uniqueGames.length; i++) {
        var ug = _uniqueGames[i];

        html += 
          '<div class="dash-game-card" onclick="ZFTPD.dashboard.playGame(\''+jsq(ug.fingerprint)+'\'); ZFTPD.modal.close()">' +
            '<div class="dash-game-cover placeholder">' + ICO.gamepad + '</div>' +
            '<div class="dash-game-info">' +
              '<div class="dash-game-title" title="'+Z.h(ug.name)+'">' + Z.h(ug.name) + '</div>' +
              '<div class="dash-game-id">' + Z.h(ug.id || ug.path || 'Installed') + '</div>' +
            '</div>' +
          '</div>';
      }
      html += '</div>';

      Z.modal.showHTML('Full Game Library', html);
      
      var d = document.getElementById('zftpd-modal-content');
      if (d) {
        d.style.overflowY = 'auto';
        d.style.maxHeight = 'min(68vh, 720px)';
        d.style.padding = '20px';
        if (d.parentNode) d.parentNode.style.width = 'min(860px,92vw)';
      }
    }
  };

  Z.dashboard = dashboard;

})(ZFTPD);
