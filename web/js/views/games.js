/* ══ GAMES VIEW — XMB STYLE ═══════════════════════════════════════════════
 * PS4-like horizontal shelves with icons and game actions.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var games = {};

  var _bound = false;
  var _statusTimer = null;
  var _lastTaskId = -1;
  var _lastMilestone = -1;
  var _lastErrorCode = 0;
  var _installed = [];
  var _images = [];
  var _scanRoots = ['/mnt/usb0', '/mnt/usb1', '/data', '/user', '/'];
  var _gameExts = { pkg: 1, fpkg: 1, ffpkg: 1, exfat: 1 };

  games.refresh = function () {
    bindUI();
    loadInstalled();
    startStatusPolling();
    pollInstallStatus();
  };

  function bindUI() {
    if (_bound) return;
    _bound = true;

    var refreshBtn = $('games-refresh-btn');
    if (refreshBtn) refreshBtn.onclick = function () {
      loadInstalled();
      pollInstallStatus();
    };

    var repairBtn = $('games-repair-btn');
    if (repairBtn) {
      repairBtn.onclick = function () {
        Z.api.gamesRepairVisibility().then(function (r) {
          var extra = '';
          if (r && r.sqlite_repair) {
            extra = ' • SQL rows ' + (r.sqlite_repair.rows || 0);
          }
          Z.toast(((r && r.message) || 'Visibility repaired') + extra, 'ok');
          loadInstalled();
        }).catch(function (e) {
          Z.toast('Repair failed: ' + (e && e.message ? e.message : 'error'), 'er');
        });
      };
    }

    var scanBtn = $('games-scan-btn');
    if (scanBtn) scanBtn.onclick = scanImages;

    var installBtn = $('games-install-btn');
    if (installBtn) {
      installBtn.onclick = function () {
        var inp = $('games-install-path');
        var p = inp ? (inp.value || '').trim() : '';
        if (!p) {
          Z.toast('Insert a PKG path', 'wn');
          return;
        }
        doInstall(p, false);
      };
    }

    var reinstallBtn = $('games-reinstall-btn');
    if (reinstallBtn) {
      reinstallBtn.onclick = function () {
        var inp = $('games-install-path');
        var p = inp ? (inp.value || '').trim() : '';
        if (!p) {
          Z.toast('Insert a PKG path', 'wn');
          return;
        }
        doInstall(p, true);
      };
    }
  }

  function loadInstalled() {
    var el = $('games-installed-list');
    if (el) el.innerHTML = '<div class="games-empty">Loading installed apps…</div>';

    Z.api.gamesInstalled().then(function (res) {
      _installed = (res && res.entries && Array.isArray(res.entries)) ? res.entries : [];
      renderInstalled();
    }).catch(function (err) {
      if (el) el.innerHTML = '<div class="games-empty">Failed to load installed list</div>';
      Z.toast('Installed list failed: ' + (err && err.message ? err.message : 'error'), 'er');
    });
  }

  function renderInstalled() {
    var el = $('games-installed-list');
    if (!el) return;
    el.innerHTML = '';

    if (!_installed.length) {
      el.innerHTML = '<div class="games-empty">No installed apps found</div>';
      return;
    }

    for (var i = 0; i < _installed.length; i++) {
      var g = _installed[i] || {};
      var id = g.id || '';
      var name = g.name || id || 'Unknown';
      var path = g.path || '';
      var icon = Z.api.gameInstalledIconUrl(id, path) + '&_t=' + Date.now();

      var row = D.createElement('article');
      row.className = 'games-card';
      row.innerHTML =
        '<img class="games-card-cover" src="' + esc(icon) + '" alt="' + esc(name) + '">' +
        '<div class="games-card-body">' +
          '<div class="games-card-title" title="' + esc(name) + '">' + esc(name) + '</div>' +
          '<div class="games-card-meta">' + esc(id) + '</div>' +
          '<div class="games-card-actions">' +
            '<button class="btn games-btn-launch">Launch</button>' +
            '<button class="btn games-btn-repair">Repair</button>' +
            '<button class="btn games-btn-danger">Uninstall</button>' +
          '</div>' +
        '</div>' +
      '';

      (function (titleId, titleName, launchBtn, repairBtn, uninstallBtn) {
        if (launchBtn) {
          launchBtn.onclick = function () {
            Z.api.gameLaunch(titleId).then(function (r) {
              var ok = !!(r && (r.ok === true || r.status === 'ok'));
              Z.toast((r && r.message) || ('Launch sent: ' + titleId), ok ? 'ok' : 'wn');
              if (!ok && shouldRepairVisibility(r)) {
                repairTitleVisibility(titleId, true);
              }
              if (!ok && Z.notify) Z.notify('Launch not executed', titleId, 'wn');
            }).catch(function () {
              Z.toast('Launch failed: ' + titleId, 'er');
            });
          };
        }

        if (repairBtn) {
          repairBtn.onclick = function () {
            repairTitleVisibility(titleId, false);
          };
        }

        if (uninstallBtn) {
          uninstallBtn.onclick = function () {
            if (!Z.modal || !Z.modal.confirm) {
              Z.toast('Modal unavailable', 'er');
              return;
            }
            Z.modal.confirm('Uninstall game', 'Remove ' + titleName + ' (' + titleId + ')?').then(function (yes) {
              if (!yes) return;
              Z.api.gameUninstall(titleId).then(function (r) {
                Z.toast((r && r.message) || ('Uninstalled ' + titleId), 'ok');
                loadInstalled();
              }).catch(function (e) {
                Z.toast('Uninstall failed: ' + (e && e.message ? e.message : titleId), 'er');
              });
            });
          };
        }
      })(
        id,
        name,
        row.querySelector('.games-btn-launch'),
        row.querySelector('.games-btn-repair'),
        row.querySelector('.games-btn-danger')
      );

      el.appendChild(row);
    }
  }

  function scanImages() {
    var el = $('games-images-list');
    if (el) {
      el.innerHTML = '';
      el.innerHTML = '<div class="games-empty">Scanning PKG/exFAT images…</div>';
    }

    _images = [];
    var seen = {};
    var pending = 0;

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
            continue;
          }

          if (e.type !== 'file') continue;
          if (e.name.indexOf('._') === 0) continue;

          var ext = Z.extname(e.name);
          if (!_gameExts[ext]) continue;
          if (seen[fullPath]) continue;
          seen[fullPath] = 1;

          _images.push({ path: fullPath, name: e.name, size: e.size || 0, meta: null });
        }
      }).catch(function () {
        /* ignore per-root errors */
      }).then(function () {
        pending--;
        if (pending === 0) {
          enrichAndRenderImages();
        }
      });
    }

    for (var r = 0; r < _scanRoots.length; r++) {
      scanDir(_scanRoots[r], 0);
    }
  }

  function enrichAndRenderImages() {
    if (!_images.length) {
      renderImages();
      return;
    }

    var tasks = [];
    for (var i = 0; i < _images.length; i++) {
      (function (img) {
        var t = Z.api.gameMeta(img.path).then(function (m) {
          img.meta = m || null;
        }).catch(function () {});
        tasks.push(t);
      })(_images[i]);
    }

    Promise.all(tasks).then(renderImages).catch(renderImages);
  }

  function renderImages() {
    var el = $('games-images-list');
    if (!el) return;
    el.innerHTML = '';

    if (!_images.length) {
      el.innerHTML = '<div class="games-empty">No PKG/exFAT images found</div>';
      return;
    }

    _images.sort(function (a, b) {
      return (a.name || '').localeCompare(b.name || '');
    });

    for (var i = 0; i < _images.length; i++) {
      var g = _images[i];
      var title = (g.meta && g.meta.title_name) ? g.meta.title_name : g.name;
      var tid = (g.meta && g.meta.title_id) ? g.meta.title_id : '';
      var path = g.path || '';
      var canInstall = /\.(pkg|fpkg|ffpkg)$/i.test(path);

      var cover = (g.meta && g.meta.icon_base64)
        ? ('data:image/png;base64,' + g.meta.icon_base64)
        : '/assets/zftpd-logo.png';

      var row = D.createElement('article');
      row.className = 'games-card';
      row.innerHTML =
        '<img class="games-card-cover" src="' + esc(cover) + '" alt="' + esc(title) + '">' +
        '<div class="games-card-body">' +
          '<div class="games-card-title" title="' + esc(title) + '">' + esc(title) + '</div>' +
          '<div class="games-card-meta">' + (tid ? esc(tid) + ' • ' : '') + esc(Z.bytes(g.size || 0)) + '</div>' +
          '<div class="games-card-actions">' +
            '<button class="btn games-btn-launch">Launch</button>' +
            '<button class="btn" ' + (canInstall ? '' : 'disabled') + '>Install</button>' +
            '<button class="btn" ' + (canInstall ? '' : 'disabled') + '>Reinstall</button>' +
          '</div>' +
        '</div>';

      (function (game, launchBtn, installBtn, reinstallBtn) {
        if (launchBtn) {
          launchBtn.onclick = function () {
            Z.api.gameLaunch(game.meta && game.meta.title_id, game.path).then(function (r) {
              var ok = !!(r && (r.ok === true || r.status === 'ok'));
              Z.toast((r && r.message) || 'Launch signal sent', ok ? 'ok' : 'wn');
            }).catch(function () {
              Z.toast('Launch failed', 'er');
            });
          };
        }

        if (installBtn) {
          installBtn.onclick = function () { doInstall(game.path, false); };
        }

        if (reinstallBtn) {
          reinstallBtn.onclick = function () { doInstall(game.path, true); };
        }
      })(g, row.querySelector('.games-btn-launch'), row.querySelectorAll('.btn')[1], row.querySelectorAll('.btn')[2]);

      el.appendChild(row);
    }
  }

  function doInstall(path, reinstall) {
    if (!path) {
      Z.toast('Missing path', 'er');
      return;
    }

    var fn = reinstall ? Z.api.gameReinstall : Z.api.gameInstall;
    fn(path).then(function (r) {
      var ok = !!(r && r.ok !== false);
      Z.toast((r && r.message) || (reinstall ? 'Reinstall started' : 'Install started'), ok ? 'ok' : 'wn');
      if (ok && Z.notify) {
        Z.notify(reinstall ? 'Reinstall started' : 'Install started', path, 'ok');
      }
      pollInstallStatus();
      loadInstalled();
    }).catch(function (e) {
      Z.toast((reinstall ? 'Reinstall failed: ' : 'Install failed: ') + (e && e.message ? e.message : 'error'), 'er');
    });
  }

  function shouldRepairVisibility(resp) {
    var msg = (resp && resp.message) ? String(resp.message) : '';
    var code = (resp && typeof resp.code === 'number') ? resp.code : 0;
    if (code === -30) return true;
    if (/0x80940005/i.test(msg)) return true;
    if (/title not installed/i.test(msg)) return true;
    return false;
  }

  function repairTitleVisibility(titleId, silent) {
    if (!titleId) return;
    Z.api.gamesRepairVisibility(titleId).then(function (r) {
      var rows = (r && r.sqlite_repair && typeof r.sqlite_repair.rows === 'number')
        ? r.sqlite_repair.rows
        : 0;
      if (!silent) {
        Z.toast('Repair ' + titleId + ' done • SQL rows ' + rows, 'ok');
      } else if (rows > 0) {
        Z.toast('Visibility repaired for ' + titleId, 'ok');
      }
      loadInstalled();
    }).catch(function (e) {
      if (!silent) {
        Z.toast('Repair failed: ' + (e && e.message ? e.message : titleId), 'er');
      }
    });
  }

  function startStatusPolling() {
    if (_statusTimer) return;
    _statusTimer = setInterval(function () {
      if (!Z.state || Z.state.view !== 'games') return;
      pollInstallStatus();
    }, 2000);
  }

  function pollInstallStatus() {
    if (!Z.api || !Z.api.gameInstallStatus) return;
    Z.api.gameInstallStatus().then(function (s) {
      renderInstallStatus(s || {});
    }).catch(function () {
      renderInstallStatus({ ok: false, error: -1, active: false, message: 'status unavailable' });
    });
  }

  function renderInstallStatus(s) {
    var el = $('games-install-status');
    if (!el) return;

    el.classList.remove('ok');
    el.classList.remove('er');

    var active = !!s.active;
    var progress = (typeof s.progress === 'number') ? s.progress : 0;
    var taskId = (typeof s.task_id === 'number') ? s.task_id : -1;
    var titleId = s.title_id || '';
    var err = (typeof s.error === 'number') ? s.error : 0;

    if (taskId !== _lastTaskId) {
      _lastTaskId = taskId;
      _lastMilestone = -1;
      _lastErrorCode = 0;
    }

    if (active) {
      el.classList.add('ok');
      el.textContent = 'BGFT task #' + taskId + ' • ' + progress + '% • ' + (titleId || 'unknown title');

      var milestone = -1;
      if (progress >= 100) milestone = 100;
      else if (progress >= 75) milestone = 75;
      else if (progress >= 50) milestone = 50;
      else if (progress >= 25) milestone = 25;

      if (milestone > _lastMilestone) {
        _lastMilestone = milestone;
        if (Z.notify) {
          Z.notify('Install progress', (titleId || 'task #' + taskId) + ' • ' + milestone + '%', 'ok');
        }
      }
      return;
    }

    if (err && err !== 0) {
      el.classList.add('er');
      el.textContent = 'Last BGFT status error: ' + err;
      if (_lastErrorCode !== err && Z.notify) {
        _lastErrorCode = err;
        Z.notify('Install error', (titleId || 'task #' + taskId) + ' • code ' + err, 'er');
      }
      return;
    }

    if (taskId >= 0 && progress >= 100) {
      el.classList.add('ok');
      el.textContent = 'Install task completed • ' + (titleId || 'done');
      if (_lastMilestone < 100 && Z.notify) {
        _lastMilestone = 100;
        Z.notify('Install completed', titleId || ('task #' + taskId), 'ok');
      }
      loadInstalled();
      return;
    }

    el.textContent = 'No active install task';
  }

  function esc(s) {
    s = (s === undefined || s === null) ? '' : String(s);
    return s
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  Z.gamesView = games;

})(ZFTPD);
