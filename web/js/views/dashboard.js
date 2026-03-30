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

  /* ── Load games — recursive scan ────────────── */
  var _gameExts = { pkg: 1, fpkg: 1, ffpkg: 1, exfat: 1 };
  var _searchRoots = ['/mnt/usb0', '/mnt/usb1', '/', '/data', '/user'];

  function loadGames() {
    _games = [];
    var pending = 0;
    var seen = {};
    var metaPromises = [];
    dashboard.heroSet = false;

    var row = $('dash-games');
    if (row) {
      row.innerHTML = '';
      for (var s = 0; s < 6; s++) {
        row.innerHTML += '<div class="dash-game-card shimmer" style="border-color:transparent"><div class="dash-game-cover"></div><div class="dash-game-info"><div class="dash-game-title" style="height:14px;background:var(--bd2);border-radius:4px;width:80%"></div><div class="dash-game-id" style="height:10px;background:var(--bd);border-radius:4px;width:40%;margin-top:6px"></div></div></div>';
      }
    }

    function scanDir(dirPath, depth) {
      if (depth > 2) return;
      pending++;
      Z.api.list(dirPath).then(function (d) {
        var entries = (d && Array.isArray(d.entries)) ? d.entries : [];
        for (var i = 0; i < entries.length; i++) {
          var e = entries[i];
          var fullPath = Z.join(dirPath, e.name);
          if (e.type === 'directory' && depth < 2) {
            scanDir(fullPath, depth + 1);
          } else {
            var ext = Z.extname(e.name);
            if (e.name.indexOf('._') === 0) continue;

            if (_gameExts[ext] && !seen[fullPath]) {
              seen[fullPath] = 1;
              var g = { name: e.name, size: e.size, path: fullPath, meta: null };
              _games.push(g);
              
              /* Fetch meta to get content_id for reliable deduplication */
              var metaPromise = Z.api.gameMeta(fullPath).then((function(gameObj) {
                return function(meta) { if(meta) gameObj.meta = meta; };
              })(g)).catch(function(){});
              
              metaPromises.push(metaPromise);
            }
          }
        }
        pending--;
        if (pending === 0) finishLoad();
      }).catch(function () {
        pending--;
        if (pending === 0) finishLoad();
      });
    }
    
    function finishLoad() {
      if (metaPromises.length > 0) {
        Promise.all(metaPromises).then(computeUniqueGames).catch(computeUniqueGames);
      } else {
        computeUniqueGames();
      }
    }

    for (var r = 0; r < _searchRoots.length; r++) {
      scanDir(_searchRoots[r], 0);
    }
  }

  function getFingerprintFromName(name) {
    var match = name.match(/(CUSA\d{5}|PPSA\d{5})/i);
    if (match) return match[1].toUpperCase();
    
    var n = name.toLowerCase();
    n = n.replace(/\.(exfat|pkg|fpkg|ffpkg|ufs)$/i, '');
    n = n.replace(/_v\d+\.\d+/g, ''); 
    n = n.replace(/_\[.*?\]/g, ''); 
    n = n.replace(/_backport/gi, '');
    n = n.replace(/_opoisso\d+/gi, '');
    n = n.replace(/_cyb1k/gi, '');
    n = n.replace(/-\[dlpsgame.*?\]/gi, '');
    return n;
  }

  function computeUniqueGames() {
    var groups = {};
    _uniqueGames = [];
    for (var i = 0; i < _games.length; i++) {
      var g = _games[i];
      /* Deduplicate strictly by content_id if available, fallback to filename logic */
      var fp = (g.meta && g.meta.content_id && g.meta.content_id.length > 0) ? g.meta.content_id : getFingerprintFromName(g.name);
      
      if (!groups[fp]) {
        var ref = { fingerprint: fp, name: g.name, locations: [g], meta: g.meta, coverUrl: null };
        groups[fp] = ref;
        _uniqueGames.push(ref);
      } else {
        groups[fp].locations.push(g);
      }
    }
    renderGames();
  }

  /* ── Render game cards row ── */
  function renderGames() {
    var row = $('dash-games');
    if (!row) return;
    row.innerHTML = '';

    if (!_uniqueGames.length) {
      row.innerHTML = '<div class="dash-game-card" style="width:280px;display:flex;align-items:center;justify-content:center;padding:20px;color:var(--tx3);font-size:12px;">' +
        ICO.gamepad + ' <span style="margin-left:8px">No game files found</span></div>';
      return;
    }

    /* We only show a limited number on the hero row */
    for (var i = 0; i < _uniqueGames.length && i < 20; i++) {
      var ug = _uniqueGames[i];
      var card = D.createElement('div');
      card.className = 'dash-game-card';

      var badgeHtml = '';
      if (ug.locations.length > 1) {
         badgeHtml = '<div class="dash-loc-badge">&times;' + ug.locations.length + ' Locs</div>';
      }

      var iconUrl = null;
      if (ug.meta && (ug.meta.has_icon || ug.meta.icon_base64)) {
        iconUrl = ug.meta.icon_base64 ? ('data:image/png;base64,' + ug.meta.icon_base64) : Z.api.gameIconUrl(ug.locations[0].path);
        ug.coverUrl = iconUrl;
        setHeroBanner(ug, iconUrl);
      }

      var coverHtml = iconUrl ? '<img class="dash-game-cover" src="' + iconUrl + '">' : '<div class="dash-game-cover placeholder">' + ICO.gamepad + '</div>';
      var title = ug.meta && ug.meta.title_name ? ug.meta.title_name : ug.name.replace(/\.(exfat|pkg|fpkg|ffpkg)$/i, '');
      var cusa = ug.meta && ug.meta.title_id ? ug.meta.title_id : ug.fingerprint;

      card.innerHTML = coverHtml + badgeHtml +
        '<div class="dash-game-info">' +
          '<div class="dash-game-title" title="' + title + '">' + title + '</div>' +
          '<div class="dash-game-id">' + cusa + '</div>' +
        '</div>';

      (function (fp) {
        card.onclick = function () {
           dashboard.playGame(fp);
        };
      })(ug.fingerprint);

      row.appendChild(card);
    }
  }

  function setHeroBanner(ug, iconUrl) {
    if (dashboard.heroSet) return;
    dashboard.heroSet = true;
    var hc = $('dash-hero-container');
    if (!hc) return;

    var title = ug.meta && ug.meta.title_name ? ug.meta.title_name : ug.name.replace(/\.(pkg|fpkg|exfat)$/i, '');
    var cusa = ug.meta && ug.meta.title_id ? ug.meta.title_id : ug.fingerprint;

    var html = 
      '<div class="dash-hero-banner">' +
        '<div class="dash-hero-bg" style="background-image:url(\''+iconUrl+'\')"></div>' +
        '<div class="dash-hero-content">' +
          '<img class="dash-hero-cover" src="'+iconUrl+'">' +
          '<div class="dash-hero-info">' +
            '<div class="dash-hero-meta">Featured Game</div>' +
            '<div class="dash-hero-title" title="'+title+'">'+title+'</div>' +
            '<div class="dash-hero-id">'+cusa+'</div>' +
          '</div>' +
          '<div class="dash-hero-actions">' +
            '<button class="btn" style="background:var(--ac);color:#fff;border:none;padding:12px 24px;font-weight:700;font-size:14px;border-radius:24px;cursor:pointer;box-shadow:0 8px 16px rgba(0,0,0,0.5);" onclick="ZFTPD.dashboard.playGame(\''+ug.fingerprint+'\')">Play / Browse</button>' +
          '</div>' +
        '</div>' +
      '</div>';
    hc.innerHTML = html;
    hc.style.display = 'flex';
  }

  dashboard.playGame = function(fp) {
    var ug = null;
    for (var i = 0; i < _uniqueGames.length; i++) {
        if (_uniqueGames[i].fingerprint === fp) { ug = _uniqueGames[i]; break; }
    }
    if (!ug) return;

    var titleId = ug.meta && ug.meta.title_id ? ug.meta.title_id : null;
    var title = ug.meta && ug.meta.title_name ? ug.meta.title_name : ug.name.replace(/\.(pkg|fpkg|exfat)$/i, '');

    var d = document.getElementById('dash-action-modal');
    if (d) d.parentNode.removeChild(d);

    var html = 
      '<div id="dash-action-modal" class="dash-loc-dropdown">' +
        '<div class="dash-loc-box">' +
          '<div class="dash-loc-box-title">' + title + ' <span class="close-btn" onclick="var d=document.getElementById(\'dash-action-modal\');d.parentNode.removeChild(d);">&times;</span></div>' +
          '<div class="dash-loc-box-sub">Choose an action for this game</div>' +
          '<div class="dash-loc-list" style="display:flex;flex-direction:column;gap:10px;padding:10px;">';

    var launchPath = (ug.locations && ug.locations[0] && ug.locations[0].path) ? ug.locations[0].path : null;
    if (titleId || launchPath) {
      var launchUrl = titleId
        ? ("/api/admin/launch?id=" + encodeURIComponent(titleId))
        : ("/api/admin/launch?path=" + encodeURIComponent(launchPath));
      var launchJs = "fetch('" + launchUrl + "').then(function(r){return r.json();}).then(function(j){ ZFTPD.toast((j&&j.message) || 'Launch signal sent', (j&&j.status)==='ok'?'success':'error'); var m=document.getElementById('dash-action-modal'); if(m)m.parentNode.removeChild(m); }).catch(function(){ZFTPD.toast('Launch failed','error');})";
      html += 
        '<button class="btn" style="background:var(--ac);color:#fff;border:none;padding:12px;border-radius:8px;cursor:pointer;font-weight:bold;display:flex;align-items:center;justify-content:center;gap:8px;" onclick="' + launchJs + '">' +
          ICO.gamepad + ' Launch Game' + (titleId ? (' (' + titleId + ')') : '') +
        '</button>';
    }

    if (ug.locations.length === 1) {
      html += 
        '<button class="btn" style="background:var(--bg2);color:var(--tx1);border:1px solid var(--bd);padding:12px;border-radius:8px;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:8px;" onclick="ZFTPD.dashboard.navTo(\'' + Z.E(ug.locations[0].path) + '\')">' +
          ICO.folder + ' Browse Files' +
        '</button>';
    } else {
      html += '<div style="font-size:12px;color:var(--tx3);margin-top:10px;">Browse Duplicate Files:</div>';
      for (var i = 0; i < ug.locations.length; i++) {
        var loc = ug.locations[i];
        var drive = loc.path.indexOf('usb0') > -1 ? 'USB0' : (loc.path.indexOf('usb1') > -1 ? 'USB1' : (loc.path.indexOf('data') > -1 ? 'DATA' : 'SYS'));
        html += 
          '<div class="dash-loc-list-item" onclick="ZFTPD.dashboard.navTo(\''+Z.E(loc.path)+'\')">' +
            '<div class="dash-loc-drive">'+drive+'</div>' +
            '<div class="dash-loc-path" title="'+loc.path+'">'+loc.path+'</div>' +
            '<div class="dash-loc-size">'+Z.bytes(loc.size||0)+'</div>' +
          '</div>';
      }
    }
    
    html += '</div></div></div>';
    var div = document.createElement('div');
    div.innerHTML = html;
    document.body.appendChild(div.firstChild);
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
          window.location.href = Z.api.downloadUrl(p);
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
      var html = '<div style="display:flex;flex-wrap:wrap;gap:16px;align-content:start;">';
      for (var i = 0; i < _uniqueGames.length; i++) {
        var ug = _uniqueGames[i];
        
        var badgeHtml = '';
        if (ug.locations.length > 1) {
           badgeHtml = '<div class="dash-loc-badge">&times;' + ug.locations.length + ' Locs</div>';
        }
        var coverUrl = ug.coverUrl ? ug.coverUrl : '';
        var coverHtml = coverUrl ? '<img class="dash-game-cover" src="'+coverUrl+'">' : '<div class="dash-game-cover placeholder">' + ICO.gamepad + '</div>';
        var title = ug.meta && ug.meta.title_name ? ug.meta.title_name : ug.name.replace(/\.(exfat|pkg|fpkg|ffpkg)$/i, '');
        var id = ug.meta && ug.meta.title_id ? ug.meta.title_id : getFingerprintFromName(ug.name);

        html += 
          '<div class="dash-game-card" onclick="ZFTPD.dashboard.playGame(\''+ug.fingerprint+'\'); ZFTPD.modal.close()">' +
            coverHtml + badgeHtml +
            '<div class="dash-game-info">' +
              '<div class="dash-game-title" title="'+ug.name+'">' + title + '</div>' +
              '<div class="dash-game-id">' + id + '</div>' +
            '</div>' +
          '</div>';
      }
      html += '</div>';

      Z.modal.showHTML('Full Game Library', html);
      
      var d = document.getElementById('zftpd-modal-content');
      if (d) {
        d.style.overflowY = 'auto';
        d.style.maxHeight = '60vh';
        d.style.padding = '24px 32px';
      }
    }
  };

  Z.dashboard = dashboard;

})(ZFTPD);
