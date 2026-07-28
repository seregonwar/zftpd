/* ══ API LAYER ════════════════════════════════════════════════════════════
 * Centralized fetch wrappers for all backend endpoints.
 * ES5 compatible for PS5 browser.
 *
 * Rest-mode resilience:
 *   - Transport failures bump epoch and enter WaitingForWake
 *   - /api/status probe with exponential backoff until the daemon returns
 *   - instance_id rotation clears the "same process" assumption
 *   - remoteReady() gates mutating operations while stale/reconnecting
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var api = {};

  /* ── Rest-mode reconnect state ── */
  var BACKOFF_MS = [500, 1000, 2000, 4000, 8000, 10000];
  var reconnect = {
    enabled: true,
    phase: 'online', /* online | waiting | reconnecting */
    stale: false,
    epoch: 0,
    attempt: 0,
    failures: 0,
    instanceId: null,
    timer: null,
    healthTimer: null
  };

  function backoffMs(attempt) {
    if (attempt >= BACKOFF_MS.length) return BACKOFF_MS[BACKOFF_MS.length - 1];
    return BACKOFF_MS[attempt];
  }

  function setPhase(phase, reason) {
    reconnect.phase = phase;
    if (Z.onReconnectPhase) {
      try { Z.onReconnectPhase(phase, reason || ''); } catch (e) { /* ignore */ }
    }
  }

  function beginReconnect(reason) {
    if (!reconnect.enabled) return;
    if (reconnect.phase !== 'online') return;
    reconnect.epoch++;
    reconnect.stale = true;
    reconnect.attempt = 0;
    reconnect.failures = 0;
    setPhase('waiting', reason || 'transport lost');
    scheduleReconnect();
  }

  function scheduleReconnect() {
    if (reconnect.timer) {
      clearTimeout(reconnect.timer);
      reconnect.timer = null;
    }
    if (!reconnect.enabled) return;
    if (reconnect.phase !== 'waiting' && reconnect.phase !== 'reconnecting') return;

    var delay = backoffMs(reconnect.attempt);
    setPhase('waiting', 'retry in ' + delay + 'ms');
    reconnect.timer = setTimeout(function () {
      reconnect.timer = null;
      tryReconnect();
    }, delay);
  }

  function tryReconnect() {
    if (!reconnect.enabled) return;
    if (reconnect.phase === 'online' && !reconnect.stale) return;

    setPhase('reconnecting', 'attempt ' + (reconnect.attempt + 1));
    var epoch = reconnect.epoch;

    fetch('/api/status').then(function (r) {
      if (epoch !== reconnect.epoch) return null;
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).then(function (j) {
      if (epoch !== reconnect.epoch) return;
      if (!j || !j.ok) throw new Error('bad status');

      var rotated = (reconnect.instanceId !== null &&
                     j.instance_id &&
                     j.instance_id !== reconnect.instanceId);
      reconnect.instanceId = j.instance_id || reconnect.instanceId;
      reconnect.stale = false;
      reconnect.attempt = 0;
      reconnect.failures = 0;
      setPhase('online', rotated ? 'daemon restarted' : 'reconnected');

      if (rotated && Z.onDaemonRotated) {
        try { Z.onDaemonRotated(j); } catch (e) { /* ignore */ }
      }
      if (Z.onReconnected) {
        try { Z.onReconnected(j); } catch (e) { /* ignore */ }
      }
    }).catch(function () {
      if (epoch !== reconnect.epoch) return;
      reconnect.attempt++;
      setPhase('waiting', 'daemon unreachable');
      scheduleReconnect();
    });
  }

  function noteTransportFailure() {
    reconnect.failures++;
    /* Two consecutive failures before auto-reconnect. */
    if (reconnect.failures >= 2) {
      beginReconnect('network error');
    }
  }

  function noteTransportOk(j) {
    reconnect.failures = 0;
    if (j && j.instance_id) {
      if (reconnect.instanceId === null) {
        reconnect.instanceId = j.instance_id;
      } else if (j.instance_id !== reconnect.instanceId && reconnect.phase === 'online') {
        reconnect.instanceId = j.instance_id;
        if (Z.onDaemonRotated) {
          try { Z.onDaemonRotated(j); } catch (e) { /* ignore */ }
        }
      }
    }
  }

  function startHealthPoll() {
    if (reconnect.healthTimer) return;
    reconnect.healthTimer = setInterval(function () {
      if (!reconnect.enabled) return;
      if (reconnect.phase !== 'online') return;
      fetch('/api/status').then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      }).then(function (j) {
        noteTransportOk(j);
      }).catch(function () {
        noteTransportFailure();
      });
    }, 5000);
  }

  /** True when remote mutations are allowed. */
  api.remoteReady = function () {
    return reconnect.phase === 'online' && !reconnect.stale;
  };

  api.reconnectState = function () {
    return {
      phase: reconnect.phase,
      stale: reconnect.stale,
      epoch: reconnect.epoch,
      attempt: reconnect.attempt,
      instanceId: reconnect.instanceId
    };
  };

  api.startReconnectMonitor = function () {
    reconnect.enabled = true;
    startHealthPoll();
    /* Seed instance id immediately. */
    fetch('/api/status').then(function (r) {
      return r.ok ? r.json() : null;
    }).then(function (j) {
      if (j) noteTransportOk(j);
    }).catch(function () {
      beginReconnect('initial probe failed');
    });
  };

  /* ── Internal helpers ── */
  function get(url) {
    return fetch(url).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).catch(function (e) {
      if (!e || !e.message || e.message.indexOf('HTTP ') !== 0) {
        noteTransportFailure();
      }
      throw e;
    });
  }

  function post(url, body) {
    if (!api.remoteReady()) {
      return Promise.reject(new Error('Reconnecting — try again shortly'));
    }
    var opts = {
      method: 'POST',
      headers: { 'X-CSRF-Token': Z.csrf() }
    };
    if (body !== undefined) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    return fetch(url, opts).then(function (r) {
      return r.json().then(function (j) {
        if (!r.ok) {
          throw new Error((j && j.message) ? j.message : ('HTTP ' + r.status));
        }
        return j;
      }).catch(function (e) {
        if (!r.ok && e && /^Unexpected/.test(String(e.message || ''))) {
          throw new Error('HTTP ' + r.status);
        }
        throw e;
      });
    }).catch(function (e) {
      if (!e || !e.message || (e.message.indexOf('HTTP ') !== 0 &&
          e.message.indexOf('Reconnecting') !== 0)) {
        noteTransportFailure();
      }
      throw e;
    });
  }

  /* ── Status ── */
  api.status = function () {
    return get('/api/status');
  };

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
    if (!api.remoteReady()) {
      return Promise.reject(new Error('Reconnecting — try again shortly'));
    }
    return fetch('/api/create_file?path=' + Z.E(dirPath) + '&name=' + Z.E(name), {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain', 'X-CSRF-Token': Z.csrf() },
      body: ''
    }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).catch(function (e) {
      if (!e || !e.message || e.message.indexOf('HTTP ') !== 0) {
        noteTransportFailure();
      }
      throw e;
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
    if (!api.remoteReady()) {
      return Promise.reject(new Error('Reconnecting — try again shortly'));
    }
    var xhrHandle = null;
    var promise = new Promise(function (resolve, reject) {
      var xhr = new XMLHttpRequest();
      xhrHandle = xhr;
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
      xhr.onerror = function () {
        noteTransportFailure();
        reject(new Error('Network error'));
      };
      xhr.onabort = function () { reject(new Error('Upload cancelled')); };
      xhr.send(file);
    });
    promise._xhr = xhrHandle;
    return promise;
  };

  /* ── Download URL ── */
  api.downloadUrl = function (path) {
    return '/api/file/get?path=' + Z.E(path);
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
    if (!Z.featureEnabled || !Z.featureEnabled('pkgInstall')) {
      return Promise.reject(new Error('PKG installation is disabled'));
    }
    return post('/api/admin/games/install?path=' + Z.E(path || ''));
  };

  api.gameReinstall = function (path) {
    if (!Z.featureEnabled || !Z.featureEnabled('pkgInstall')) {
      return Promise.reject(new Error('PKG installation is disabled'));
    }
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
