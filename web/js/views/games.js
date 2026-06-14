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
  var _uploadXhr = null;
  var _uploadFile = null;
  var _uploadCancelled = false;
  var _lastTaskId = -1;
  var _lastMilestone = -1;
  var _lastErrorCode = 0;
  var _installed = [];
  var _selectedPackagePath = '';
  var DEFAULT_UPLOAD_DIR = '/data/Packages';

  games.refresh = function () {
    bindUI();
    syncPkgInstallFeature();
    if (pkgInstallEnabled()) {
      syncPackageSelection();
      syncUploadSelection();
      startStatusPolling();
      pollInstallStatus();
    }
    loadInstalled();
  };

  games.refreshStatus = function () {
    if (pkgInstallEnabled()) pollInstallStatus();
    loadInstalled();
  };

  function pkgInstallEnabled() {
    return !!(Z.featureEnabled && Z.featureEnabled('pkgInstall'));
  }

  function syncPkgInstallFeature() {
    var enabled = pkgInstallEnabled();
    var nodes = D.querySelectorAll('.pkg-install-feature');
    for (var i = 0; i < nodes.length; i++) {
      nodes[i].hidden = !enabled;
      nodes[i].setAttribute('aria-hidden', enabled ? 'false' : 'true');
    }
  }

  function bindUI() {
    if (_bound) return;
    _bound = true;

    var refreshBtn = $('games-refresh-btn');
    if (refreshBtn) refreshBtn.onclick = function () {
      loadInstalled();
      if (pkgInstallEnabled()) pollInstallStatus();
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

    var browseBtn = $('games-pkg-browse-btn');
    if (browseBtn) browseBtn.onclick = pickPackage;

    var installBtn = $('games-install-btn');
    if (installBtn) {
      installBtn.onclick = function () {
        var p = selectedPackagePath();
        if (!p) {
          Z.toast('Select a PKG package first', 'wn');
          return;
        }
        doInstall(p, false);
      };
    }

    var reinstallBtn = $('games-reinstall-btn');
    if (reinstallBtn) {
      reinstallBtn.onclick = function () {
        var p = selectedPackagePath();
        if (!p) {
          Z.toast('Select a PKG package first', 'wn');
          return;
        }
        doInstall(p, true);
      };
    }

    var uploadPickBtn = $('games-upload-pick-btn');
    if (uploadPickBtn) uploadPickBtn.onclick = function () {
      if (!pkgInstallEnabled()) return installDisabledNotice();
      var input = $('games-upload-file');
      if (input) input.click();
    };

    var uploadFile = $('games-upload-file');
    if (uploadFile) {
      uploadFile.onchange = function (ev) {
        var files = ev && ev.target ? ev.target.files : null;
        setUploadFile(files && files.length ? files[0] : null);
        uploadFile.value = '';
      };
    }

    var uploadDstPick = $('games-upload-dst-pick');
    if (uploadDstPick) uploadDstPick.onclick = pickUploadDestination;

    var uploadInstallBtn = $('games-upload-install-btn');
    if (uploadInstallBtn) uploadInstallBtn.onclick = startPcUploadInstall;
  }

  function installDisabledNotice() {
    Z.toast('PKG installation is disabled for now. Remote app launch remains available.', 'wn');
  }

  function pickPackage() {
    if (!pkgInstallEnabled()) {
      installDisabledNotice();
      return;
    }
    if (!Z.modal || !Z.modal.filePicker) {
      Z.toast('File picker unavailable', 'er');
      return;
    }

    var startPath = (Z.state && Z.state.path) ? Z.state.path : '/';
    Z.modal.filePicker('Select PKG package', startPath, {
      extensions: ['pkg', 'fpkg', 'ffpkg'],
      hint: 'Browse folders and choose a local PKG/fPKG package.'
    }).then(function (path) {
      if (!path) return;
      setSelectedPackage(path);
      Z.toast('Package selected', 'ok');
    });
  }

  function selectedPackagePath() {
    var inp = $('games-install-path');
    var p = inp ? (inp.value || '').trim() : '';
    return p || _selectedPackagePath;
  }

  function setSelectedPackage(path) {
    _selectedPackagePath = path || '';
    syncPackageSelection();
  }

  function syncPackageSelection() {
    var p = _selectedPackagePath || '';
    var hidden = $('games-install-path');
    var selected = $('games-selected-pkg');
    var installBtn = $('games-install-btn');
    var reinstallBtn = $('games-reinstall-btn');

    if (hidden) hidden.value = p;
    if (selected) {
      selected.textContent = p || 'No package selected';
      selected.title = p || '';
      if (p) selected.classList.remove('empty');
      else selected.classList.add('empty');
    }
    if (installBtn) installBtn.disabled = !p || !pkgInstallEnabled();
    if (reinstallBtn) reinstallBtn.disabled = !p || !pkgInstallEnabled();
  }

  function setUploadFile(file) {
    if (!pkgInstallEnabled()) {
      _uploadFile = null;
      syncUploadSelection();
      installDisabledNotice();
      return;
    }
    if (file && Z.isGamePackage && !Z.isGamePackage(file.name)) {
      Z.toast('Select a PKG/fPKG package from your computer', 'wn');
      _uploadFile = null;
    } else {
      _uploadFile = file || null;
    }
    syncUploadSelection();
  }

  function syncUploadSelection() {
    var label = $('games-upload-file-label');
    var dst = $('games-upload-dst');
    var btn = $('games-upload-install-btn');
    if (dst && !(dst.value || '').trim()) dst.value = DEFAULT_UPLOAD_DIR;
    if (label) {
      if (_uploadFile) {
        label.textContent = _uploadFile.name + ' • ' + Z.bytes(_uploadFile.size || 0);
        label.title = _uploadFile.name;
        label.classList.remove('empty');
      } else {
        label.textContent = 'No PC package selected';
        label.title = '';
        label.classList.add('empty');
      }
    }
    if (btn) btn.disabled = !_uploadFile || !pkgInstallEnabled();
  }

  function pickUploadDestination() {
    if (!pkgInstallEnabled()) {
      installDisabledNotice();
      return;
    }
    if (!Z.modal || !Z.modal.folderPicker) {
      Z.toast('Folder picker unavailable', 'er');
      return;
    }
    var dst = $('games-upload-dst');
    var start = dst && (dst.value || '').trim() ? dst.value.trim() : DEFAULT_UPLOAD_DIR;
    Z.modal.folderPicker('Choose upload destination', start).then(function (path) {
      if (!path) return;
      if (dst) dst.value = path;
    });
  }

  function startPcUploadInstall() {
    if (!pkgInstallEnabled()) {
      installDisabledNotice();
      return;
    }
    if (!_uploadFile) {
      Z.toast('Choose a PC package first', 'wn');
      return;
    }
    if (Z.isGamePackage && !Z.isGamePackage(_uploadFile.name)) {
      Z.toast('Select a PKG/fPKG package from your computer', 'wn');
      return;
    }
    if (!Z.ensureTransferIdle()) return;

    var file = _uploadFile;
    var dstEl = $('games-upload-dst');
    var dst = dstEl ? (dstEl.value || '').trim() : DEFAULT_UPLOAD_DIR;
    dst = dst || DEFAULT_UPLOAD_DIR;

    var uploadedPath = Z.join(dst, file.name);
    var started = Date.now();
    var previousBytes = 0;
    var previousTime = Date.now();

    _uploadCancelled = false;
    renderUploadStatus({ label: 'Streaming ' + file.name + ' from PC…', progress: 0, kind: 'ok' });

    Z.showTransferLock({
      label: 'UPLOADING PKG',
      filename: file.name,
      dest: dst,
      onCancel: function () {
        _uploadCancelled = true;
        if (_uploadXhr) _uploadXhr.abort();
        _uploadXhr = null;
        Z.hideTransferLock();
        renderUploadStatus({ label: 'Upload cancelled', progress: 100, kind: 'er' });
      }
    });

    var uploadPromise = Z.api.upload(dst, file, function (pct, loaded, total) {
      var now = Date.now();
      var dt = (now - previousTime) / 1000;
      var speed = dt > 0 ? (loaded - previousBytes) / dt : 0;
      previousBytes = loaded;
      previousTime = now;

      renderUploadStatus({
        label: 'Uploading ' + file.name + ' • ' + pct + '% • ' + Z.bytes(loaded) + ' / ' + Z.bytes(total),
        progress: pct,
        kind: 'ok'
      });
      Z.updateTransferLock({
        pct: pct,
        speed: speed > 0 ? Z.bps(speed) : '',
        elapsed: Math.floor((now - started) / 1000) + 's'
      });
    });

    _uploadXhr = uploadPromise._xhr || null;
    uploadPromise.then(function () {
      _uploadXhr = null;
      Z.hideTransferLock();
      renderUploadStatus({ label: 'Upload complete • starting install from ' + uploadedPath, progress: 100, kind: 'ok' });
      setSelectedPackage(uploadedPath);
      doInstall(uploadedPath, false);
    }).catch(function (e) {
      _uploadXhr = null;
      Z.hideTransferLock();
      if (_uploadCancelled) {
        _uploadCancelled = false;
        renderUploadStatus({ label: 'Upload cancelled', progress: 100, kind: 'er' });
        return;
      }
      renderUploadStatus({ label: 'Upload failed: ' + (e && e.message ? e.message : 'error'), progress: 100, kind: 'er' });
      Z.notify('PC package upload failed', (e && e.message) ? e.message : file.name, 'er');
    });
  }

  function setText(id, value) {
    var el = $(id);
    if (el) el.textContent = value;
  }

  function loadInstalled() {
    var el = $('games-installed-list');
    if (el) el.innerHTML = '<div class="games-empty">Loading installed apps…</div>';
    setText('games-installed-count', '…');

    Z.api.gamesInstalled().then(function (res) {
      _installed = (res && res.entries && Array.isArray(res.entries)) ? res.entries : [];
      renderInstalled();
    }).catch(function (err) {
      _installed = [];
      setText('games-installed-count', '0');
      if (el) el.innerHTML = '<div class="games-empty">Failed to load installed list</div>';
      Z.toast('Installed list failed: ' + (err && err.message ? err.message : 'error'), 'er');
    });
  }

  function renderInstalled() {
    var el = $('games-installed-list');
    if (!el) return;
    el.innerHTML = '';
    setText('games-installed-count', String(_installed.length));

    if (!_installed.length) {
      el.innerHTML = '<div class="games-empty">No installed apps found</div>';
      return;
    }

    for (var i = 0; i < _installed.length; i++) {
      var g = _installed[i] || {};
      var id = g.id || '';
      var name = g.name || id || 'Unknown';
      var path = g.path || '';

      var row = D.createElement('article');
      row.className = 'games-card';
      row.innerHTML =
        '<div class="games-controller-icon" aria-hidden="true">' + (Z.ICO && Z.ICO.gamepad ? Z.ICO.gamepad : '') + '</div>' +
        '<div class="games-card-body">' +
          '<div class="games-card-title" title="' + esc(name) + '">' + esc(name) + '</div>' +
          '<div class="games-card-meta">' + esc(id || path || 'Registered title') + '</div>' +
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

  function doInstall(path, reinstall) {
    if (!pkgInstallEnabled()) {
      installDisabledNotice();
      return;
    }
    if (!path) {
      Z.toast('Missing path', 'er');
      return;
    }
    if (Z.isGamePackage && !Z.isGamePackage(path)) {
      Z.toast('Select a PKG/fPKG package', 'wn');
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

  function renderUploadStatus(info) {
    var el = $('games-upload-status');
    if (!el) return;
    info = info || {};
    el.classList.remove('ok');
    el.classList.remove('er');
    if (info.kind === 'ok') el.classList.add('ok');
    if (info.kind === 'er') el.classList.add('er');
    var pct = Math.max(0, Math.min(100, info.progress || 0));
    el.innerHTML =
      '<span>' + esc(info.label || 'No active PC package upload') + '</span>' +
      '<i><em style="width:' + pct + '%"></em></i>';
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
    var safeProgress = Math.max(0, Math.min(100, progress || 0));

    function setStatus(text, pct, taskLabel) {
      el.innerHTML =
        '<span>' + esc(text) + '</span>' +
        '<i><em style="width:' + Math.max(0, Math.min(100, pct || 0)) + '%"></em></i>';
      setText('games-task-count', taskLabel || 'Idle');
    }

    if (taskId !== _lastTaskId) {
      _lastTaskId = taskId;
      _lastMilestone = -1;
      _lastErrorCode = 0;
    }

    if (active) {
      el.classList.add('ok');
      setStatus('BGFT task #' + taskId + ' • ' + safeProgress + '% • ' + (titleId || 'unknown title'), safeProgress, '#' + taskId + ' ' + safeProgress + '%');

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
      setStatus('Last BGFT status error: ' + err, 100, 'Error ' + err);
      if (_lastErrorCode !== err && Z.notify) {
        _lastErrorCode = err;
        Z.notify('Install error', (titleId || 'task #' + taskId) + ' • code ' + err, 'er');
      }
      return;
    }

    if (taskId >= 0 && progress >= 100) {
      el.classList.add('ok');
      setStatus('Install task completed • ' + (titleId || 'done'), 100, 'Done');
      if (_lastMilestone < 100 && Z.notify) {
        _lastMilestone = 100;
        Z.notify('Install completed', titleId || ('task #' + taskId), 'ok');
      }
      loadInstalled();
      return;
    }

    setStatus('No active install task', 0, 'Idle');
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

  games.pickPackage = pickPackage;
  games.selectPackage = setSelectedPackage;

  Z.gamesView = games;

})(ZFTPD);
