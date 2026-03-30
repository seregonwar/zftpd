/* ══ API LAYER ════════════════════════════════════════════════════════════
 * Centralized fetch wrappers for all backend endpoints.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var api = {};

  /* ── Internal helpers ── */
  function get(url) {
    return fetch(url).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    });
  }

  function post(url, body) {
    var opts = {
      method: 'POST',
      headers: { 'X-CSRF-Token': Z.csrf() }
    };
    if (body !== undefined) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    return fetch(url, opts).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    });
  }

  /* ── Directory listing ── */
  api.list = function (path) {
    return get('/api/list?path=' + Z.E(path));
  };

  /* ── Directory size (lazy) ── */
  api.dirsize = function (path) {
    return get('/api/dirsize?path=' + Z.E(path));
  };

  /* ── Stats ── */
  api.stats = function (path) {
    return get('/api/stats?path=' + Z.E(path));
  };

  api.statsRam = function () {
    return get('/api/stats/ram');
  };

  api.statsSystem = function () {
    return get('/api/stats/system');
  };

  /* ── Disk ── */
  api.diskInfo = function () {
    return get('/api/disk/info');
  };

  api.diskTree = function (path) {
    return get('/api/disk/tree?path=' + Z.E(path));
  };

  /* ── Processes ── */
  api.processes = function () {
    return get('/api/processes');
  };

  api.processKill = function (pid) {
    return post('/api/process/kill', { pid: pid });
  };

  /* ── File operations (require ENABLE_WEB_UPLOAD) ── */
  api.createFile = function (dirPath, name) {
    return fetch('/api/create_file?path=' + Z.E(dirPath) + '&name=' + Z.E(name), {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain', 'X-CSRF-Token': Z.csrf() },
      body: ''
    }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    });
  };

  api.mkdir = function (dirPath, name) {
    return post('/api/mkdir?path=' + Z.E(dirPath) + '&name=' + Z.E(name));
  };

  api.del = function (path, recursive) {
    var url = '/api/delete?path=' + Z.E(path);
    if (recursive) url += '&recursive=1';
    return post(url);
  };

  api.rename = function (path, newName) {
    return post('/api/rename?path=' + Z.E(path) + '&name=' + Z.E(newName));
  };

  api.copy = function (srcPath, dstDir, totalSize) {
    var url = '/api/copy?path=' + Z.E(srcPath) + '&dst=' + Z.E(dstDir);
    if (totalSize) url += '&totalsize=' + totalSize;
    return post(url);
  };

  api.copyProgress = function () {
    return get('/api/copy_progress');
  };

  api.copyPause = function () {
    return post('/api/copy_pause');
  };

  api.copyCancel = function () {
    return post('/api/copy_cancel');
  };

  /* ── Network reset ── */
  api.networkReset = function () {
    return post('/api/network/reset');
  };

  /* ── Upload (XMLHttpRequest for progress tracking) ── */
  api.upload = function (dirPath, file, onProgress) {
    return new Promise(function (resolve, reject) {
      var xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/upload?path=' + Z.E(dirPath) + '&name=' + Z.E(file.name), true);
      var token = Z.csrf();
      if (token) xhr.setRequestHeader('X-CSRF-Token', token);
      xhr.upload.onprogress = function (e) {
        if (e.lengthComputable && onProgress) {
          onProgress(Math.floor(e.loaded / e.total * 100), e.loaded, e.total);
        }
      };
      xhr.onload = function () {
        if (xhr.status >= 200 && xhr.status < 300) resolve(xhr);
        else reject(new Error('HTTP ' + xhr.status));
      };
      xhr.onerror = function () { reject(new Error('Network error')); };
      xhr.send(file);
      /* Return xhr handle for cancellation */
      resolve._xhr = xhr;
    });
  };

  /* ── Download URL ── */
  api.downloadUrl = function (path) {
    return '/api/download?path=' + Z.E(path);
  };

  /* ── Game metadata (Phase 4 — stub for now) ── */
  api.gameMeta = function (path) {
    return get('/api/game/meta?path=' + Z.E(path));
  };

  api.gameIconUrl = function (path) {
    return '/api/game/icon?path=' + Z.E(path);
  };

  /* ── Games management ── */
  api.gamesInstalled = function () {
    return get('/api/admin/games/installed');
  };

  api.gameInstalledIconUrl = function (id, path) {
    var u = '/api/admin/games/icon?id=' + Z.E(id || '');
    if (path) u += '&path=' + Z.E(path);
    return u;
  };

  api.gamesRepairVisibility = function (id) {
    var u = '/api/admin/games/repair_visibility';
    if (id) u += '?id=' + Z.E(id);
    return post(u);
  };

  api.gameLaunch = function (id, path) {
    if (id) {
      return get('/api/admin/launch?id=' + Z.E(id));
    }
    return get('/api/admin/launch?path=' + Z.E(path || ''));
  };

  api.gameUninstall = function (id) {
    return post('/api/admin/games/uninstall?id=' + Z.E(id || ''));
  };

  api.gameInstall = function (path) {
    return post('/api/admin/games/install?path=' + Z.E(path || ''));
  };

  api.gameReinstall = function (path) {
    return post('/api/admin/games/reinstall?path=' + Z.E(path || ''));
  };

  api.gameInstallStatus = function () {
    return get('/api/admin/games/install_status');
  };

  /* ── File copy cancel (Phase 5 — stub) ── */
  api.copyCancel = function () {
    return post('/api/copy_cancel');
  };

  /* ── Archive extraction ── */
  api.extract = function (archivePath, dstDir) {
    return post('/api/extract?path=' + Z.E(archivePath) + '&dst=' + Z.E(dstDir));
  };

  api.extractProgress = function () {
    return get('/api/extract_progress');
  };

  api.extractCancel = function () {
    return post('/api/extract_cancel');
  };

  /* ── Download Manager (Phase 6 — stub for now) ── */
  api.downloadStart = function (url, dst) {
    return post('/api/download/start', { url: url, dst: dst });
  };

  api.downloadStatus = function () {
    return get('/api/download/status');
  };

  api.downloadPause = function (id) {
    return post('/api/download/pause', { id: id });
  };

  api.downloadCancel = function (id) {
    return post('/api/download/cancel', { id: id });
  };

  Z.api = api;

})(ZFTPD);
