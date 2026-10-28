/* zftpd web — HTTP API client and connection monitor.
 *
 * Connection model (see docs/restmode.md):
 *   - /api/status is polled every 5 s while the page is visible.
 *   - Two consecutive transport failures switch to "offline" and start
 *     probing with backoff 0.5 → 1 → 2 → 4 → 8 → 10 s.
 *   - A changed instance_id means the daemon was restarted.
 *   - Mutating requests are refused while not online.
 */
(function (ZF) {
  'use strict';

  var api = {};
  var E = encodeURIComponent;
  var BACKOFF = [500, 1000, 2000, 4000, 8000, 10000];
  var HEALTH_MS = 5000;

  var conn = {
    phase: 'connecting',   /* online | offline | connecting */
    failures: 0,
    attempt: 0,
    epoch: 0,
    instanceId: null,
    retryAt: 0,
    timer: null,
    health: null
  };

  api.info = null;

  function qs(params) {
    var out = [];
    for (var k in params) {
      if (!params.hasOwnProperty(k)) continue;
      var v = params[k];
      if (v === undefined || v === null) continue;
      out.push(E(k) + '=' + E(v));
    }
    return out.length ? '?' + out.join('&') : '';
  }
  api.qs = qs;

  api.csrf = function () {
    var m = document.querySelector('meta[name="csrf-token"]');
    return m ? (m.getAttribute('content') || '') : '';
  };

  /* ── Connection state ───────────────────────────────────────────────── */

  function setPhase(phase) {
    conn.phase = phase;
    ZF.emit('connection', { phase: phase, retryAt: conn.retryAt, attempt: conn.attempt });
  }

  function rememberInstance(status) {
    var restarted = false;
    if (status && status.instance_id) {
      restarted = conn.instanceId !== null && status.instance_id !== conn.instanceId;
      conn.instanceId = status.instance_id;
    }
    api.info = status || api.info;
    /* Compile-time switches travel with the status so the views can hide what
     * the daemon was built without (e.g. the package installer). */
    if (status && status.features) {
      ZF.features.pkgInstall = status.features.pkg_install === true;
    }
    return restarted;
  }

  function goOffline() {
    if (conn.phase === 'offline') return;
    conn.epoch++;
    conn.attempt = 0;
    scheduleProbe();
  }

  function scheduleProbe() {
    clearTimeout(conn.timer);
    var delay = BACKOFF[Math.min(conn.attempt, BACKOFF.length - 1)];
    conn.retryAt = Date.now() + delay;
    setPhase('offline');
    conn.timer = setTimeout(probe, delay);
  }

  function fetchStatus() {
    return fetch('/api/status', { credentials: 'same-origin' }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).then(function (j) {
      if (!j || j.ok !== true) throw new Error('Unexpected status response');
      return j;
    });
  }

  function probe() {
    clearTimeout(conn.timer);
    conn.timer = null;
    var epoch = conn.epoch;
    setPhase('connecting');
    fetchStatus().then(function (status) {
      if (epoch !== conn.epoch) return;
      var restarted = rememberInstance(status);
      conn.failures = 0;
      conn.attempt = 0;
      setPhase('online');
      ZF.emit('reconnected', { restarted: restarted, status: status });
    }, function () {
      if (epoch !== conn.epoch) return;
      conn.attempt++;
      scheduleProbe();
    });
  }

  function transportOk() {
    conn.failures = 0;
  }

  function transportFailed() {
    conn.failures++;
    if (conn.phase === 'online' && conn.failures >= 2) goOffline();
  }

  function healthCheck() {
    if (conn.phase !== 'online') return;
    if (document.hidden) return;
    fetchStatus().then(function (status) {
      transportOk();
      if (rememberInstance(status)) ZF.emit('reconnected', { restarted: true, status: status });
    }, transportFailed);
  }

  api.start = function () {
    setPhase('connecting');
    fetchStatus().then(function (status) {
      rememberInstance(status);
      setPhase('online');
      ZF.emit('status', status);
    }, function () {
      conn.phase = 'online';
      goOffline();
    });
    if (!conn.health) conn.health = setInterval(healthCheck, HEALTH_MS);
    document.addEventListener('visibilitychange', function () {
      if (!document.hidden) healthCheck();
    });
  };

  api.ready = function () { return conn.phase === 'online'; };
  api.phase = function () { return conn.phase; };
  api.retryAt = function () { return conn.retryAt; };
  api.retryNow = function () {
    if (conn.phase === 'offline') probe();
  };

  /* ── Requests ───────────────────────────────────────────────────────── */

  function offlineError() {
    var e = new Error('Not connected to zftpd. Wait for the connection to come back and try again.');
    e.offline = true;
    return e;
  }

  function httpError(status, text) {
    var data = null;
    if (text) {
      try { data = JSON.parse(text); } catch (e) { data = null; }
    }
    var msg = (data && (data.error || data.message)) ||
      (text && text.length < 160 && text.charAt(0) !== '<' ? text : '') ||
      ('Request failed (HTTP ' + status + ')');
    var err = new Error(msg);
    err.status = status;
    err.data = data;
    return err;
  }

  function request(method, url, body) {
    var mutating = method !== 'GET';
    if (mutating && !api.ready()) return Promise.reject(offlineError());
    var init = { method: method, headers: {}, credentials: 'same-origin' };
    if (mutating) init.headers['X-CSRF-Token'] = api.csrf();
    if (body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(body);
    }
    return fetch(url, init).then(function (r) {
      transportOk();
      return r.text().then(function (text) {
        if (!r.ok) throw httpError(r.status, text);
        if (!text) return {};
        try { return JSON.parse(text); } catch (e) { return text; }
      });
    }, function () {
      transportFailed();
      var err = new Error('Cannot reach zftpd.');
      err.network = true;
      throw err;
    });
  }

  function get(url) { return request('GET', url); }
  function post(url, body) { return request('POST', url, body); }

  /* Some endpoints answer HTTP 200 with { ok:false, message, code }. */
  function soft(res) {
    if (res && res.ok === false) {
      var err = new Error(res.message || 'Operation failed');
      err.code = res.code;
      err.data = res;
      throw err;
    }
    return res;
  }

  /* ── System ─────────────────────────────────────────────────────────── */

  api.status = function () { return get('/api/status'); };
  api.stats = function (path) { return get('/api/stats' + qs({ path: path || '/' })); };
  /* Removable volumes actually mounted (the console creates the /mnt/usbN
   * directories even with nothing plugged in). */
  api.mounts = function () { return get('/api/mounts'); };
  api.ram = function () { return get('/api/stats/ram'); };
  api.system = function () { return get('/api/stats/system'); };
  api.diskInfo = function () { return get('/api/disk/info'); };
  api.processes = function () { return get('/api/processes'); };
  api.killProcess = function (pid) { return post('/api/process/kill', { pid: pid }); };
  api.fan = function (threshold) { return get('/api/admin/fan?threshold=' + parseInt(threshold, 10)); };
  api.networkReset = function () { return post('/api/network/reset').then(soft); };
  api.discState = function () { return get('/api/system/disc'); };
  api.discEject = function () { return post('/api/system/eject').then(soft); };
  api.notify = function (text) { return post('/api/notify' + qs({ text: text })); };

  /* ── Files ──────────────────────────────────────────────────────────── */

  api.list = function (path) { return get('/api/list' + qs({ path: path })); };
  api.dirSize = function (path) { return get('/api/dirsize' + qs({ path: path })); };
  api.fileUrl = function (path, decrypt) {
    /* `decrypt` overrides ZF.settings.decryptSelf for one link. */
    var want = (decrypt === undefined) ? ZF.settings.decryptSelf : decrypt;
    return '/api/file/get' + qs({ path: path, decrypt: want ? 1 : undefined });
  };
  api.decryptCheck = function (path) { return get('/api/file/decrypt_check' + qs({ path: path })); };
  /* Bulk download: the selection is posted once and the daemon answers with a
   * short id, because a long path list does not fit in a request URL. */
  /* Decrypted game dump: start it, then either download the prepared archive
   * or let the daemon write it on the console. */
  api.dumpStart = function (titleId, options) {
    var body = { title_id: titleId, target: (options && options.target) || 'download' };
    if (options && options.format) body.format = options.format;
    if (options && options.dest) body.dest = options.dest;
    if (options && options.decrypt !== undefined) body.decrypt = options.decrypt ? 1 : 0;
    return post('/api/dump/start', body);
  };

  api.dumpStatus = function () { return get('/api/dump/status'); };
  api.dumpCancel = function (id) { return post('/api/dump/cancel', { id: id }); };
  api.dumpDownloadUrl = function (id) { return '/api/dump?id=' + encodeURIComponent(id); };

  api.prepareZip = function (paths, name) {
    return post('/api/archive/zip', { paths: paths, name: name });
  };

  api.zipDownloadUrl = function (id) {
    return '/api/archive/zip?id=' + encodeURIComponent(id);
  };

  api.mkdir = function (dir, name) { return post('/api/mkdir' + qs({ path: dir, name: name })); };
  api.createFile = function (dir, name) { return post('/api/create_file' + qs({ path: dir, name: name })); };
  api.remove = function (path, recursive) {
    return post('/api/delete' + qs({ path: path, recursive: recursive ? 1 : null }));
  };
  api.rename = function (path, name) { return post('/api/rename' + qs({ path: path, name: name })); };
  /* Reply carries the mode read back from disk: check "supported" first. */
  api.chmod = function (path, mode) { return post('/api/chmod' + qs({ path: path, mode: mode })); };

  api.copy = function (src, dstDir, move) {
    return post('/api/copy' + qs({ path: src, dst: dstDir, move: move ? 1 : null }));
  };
  api.copyProgress = function () { return get('/api/copy_progress'); };
  api.copyCancel = function () { return post('/api/copy_cancel'); };
  api.copyPause = function () { return post('/api/copy_pause'); };

  api.extract = function (path, dst) { return post('/api/extract' + qs({ path: path, dst: dst })); };
  api.extractProgress = function () { return get('/api/extract_progress'); };
  api.extractCancel = function () { return post('/api/extract_cancel'); };

  /**
   * Raw-body upload with progress. Returns { promise, abort }.
   * The server takes the file bytes as the request body (not multipart).
   */
  api.upload = function (dir, name, file, onProgress) {
    var xhr = new XMLHttpRequest();
    var promise = new Promise(function (resolve, reject) {
      if (!api.ready()) { reject(offlineError()); return; }
      xhr.open('POST', '/api/upload' + qs({ path: dir, name: name }), true);
      xhr.setRequestHeader('X-CSRF-Token', api.csrf());
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      if (xhr.upload && onProgress) {
        xhr.upload.onprogress = function (e) {
          if (e.lengthComputable) onProgress(e.loaded, e.total);
        };
      }
      xhr.onload = function () {
        if (xhr.status >= 200 && xhr.status < 300) resolve();
        else reject(httpError(xhr.status, xhr.responseText));
      };
      xhr.onerror = function () {
        reject(new Error('The server closed the connection. The name or destination may not be allowed.'));
      };
      xhr.onabort = function () {
        var e = new Error('Cancelled');
        e.cancelled = true;
        reject(e);
      };
      xhr.send(file);
    });
    return { promise: promise, abort: function () { try { xhr.abort(); } catch (e) { /* ignore */ } } };
  };

  /* ── Remote downloads ───────────────────────────────────────────────── */

  api.downloadStart = function (url, dst) { return post('/api/download/start', { url: url, dst: dst }); };
  api.downloadStatus = function () { return get('/api/download/status'); };
  api.downloadPause = function (id) { return post('/api/download/pause', { id: id }); };
  api.downloadCancel = function (id) { return post('/api/download/cancel', { id: id }); };
  api.downloadRetry = function (id) { return post('/api/download/retry', { id: id }); };
  api.downloadDelete = function (id) { return post('/api/download/delete', { id: id }); };
  api.downloadOrphans = function () { return get('/api/download/orphans'); };
  api.downloadOrphanDelete = function (path) { return post('/api/download/orphan/delete', { path: path }); };

  /* ── Games ──────────────────────────────────────────────────────────── */

  api.games = function () { return get('/api/admin/games/installed').then(soft); };
  api.gameIconUrl = function (g) { return '/api/admin/games/icon' + qs({ id: g.id, path: g.path }); };
  api.gameLaunch = function (id) {
    return get('/api/admin/launch' + qs({ id: id })).then(function (res) {
      if (res && (res.ok === true || res.status === 'ok')) return res;
      return soft(res && res.ok === false ? res : { ok: false, message: 'Launch failed' });
    });
  };
  api.gameRepair = function (id) { return post('/api/admin/games/repair_visibility' + qs({ id: id })).then(soft); };
  api.gameUninstall = function (id) { return post('/api/admin/games/uninstall' + qs({ id: id })).then(soft); };
  api.gameInstall = function (path, reinstall) {
    return post('/api/admin/games/' + (reinstall ? 'reinstall' : 'install') + qs({ path: path })).then(soft);
  };
  api.installStatus = function () { return get('/api/admin/games/install_status'); };
  /* PKG installation. The daemon refuses to replace an installation on its own:
   * overwrite is passed only after the user confirms. */
  api.pkgInstall = function (source, dest, overwrite) {
    /* A link is downloaded by the console itself as part of the install; a
     * path is a package that is already on console storage. */
    var remote = /^https?:\/\//i.test(source || '');
    var q = { dest: dest, overwrite: overwrite ? 1 : undefined };
    if (remote) q.url = source; else q.path = source;
    return post('/api/admin/games/install' + qs(q)).then(soft);
  };
  /* Packages staged by the web interface live here; they are removed by the
   * daemon once the installer has taken them. */
  api.pkgStagingDir = '/data/zftpd/pkg';
  api.packageMeta = function (path) { return get('/api/game/meta' + qs({ path: path })); };
  api.packageIconUrl = function (path) { return '/api/game/icon' + qs({ path: path }); };

  ZF.api = api;
})(ZF);
