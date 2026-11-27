/* zftpd web — Games view. */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var api = ZF.api;
  var $ = ZF.byId;

  var dom = {
    sub: $('games-sub'),
    filter: $('games-filter'),
    repairAll: $('games-repair-all'),
    refresh: $('games-refresh'),
    install: $('games-install'),
    list: $('games-list')
  };

  var games = [];
  var loaded = false;
  var error = null;
  var seq = 0;
  var installTimer = null;
  var dumpTimer = null;
  var dumpSeq = 0;
  var lastDump = null;
  var trackedDumpId = 0;
  var notifiedDumpId = 0;

  function renderDump(st) {
    ZF.jobs.observeDump(st);
  }

  function pollDump() {
    clearTimeout(dumpTimer);
    var my = ++dumpSeq;
    api.dumpStatus().then(function (st) {
      if (my !== dumpSeq) return;
      renderDump(st);
      if (st && st.active && (st.state === 'done' || st.state === 'failed') &&
          st.id !== notifiedDumpId &&
          (trackedDumpId === st.id || (lastDump && lastDump.id !== st.id) ||
            (lastDump && lastDump.id === st.id && lastDump.state !== st.state))) {
        notifiedDumpId = st.id;
        if (st.state === 'done') {
          ZF.toast('Dump completed: ' + st.title_id, { type: 'success', timeout: 8000 });
        } else if (st.cancelled) {
          /* The tray already shows the job as cancelled. */
        } else {
          ZF.toast('Dump failed: ' + (st.message || st.title_id), { type: 'error', timeout: 8000 });
        }
      }
      lastDump = st;
      dumpTimer = setTimeout(pollDump, st && st.active &&
        (st.state === 'launching' || st.state === 'ready' || st.state === 'running') ? 2000 : 10000);
    }, function () {
      if (my === dumpSeq) dumpTimer = setTimeout(pollDump, 10000);
    });
  }

  /**
   * Package installation, next to the dump row: upload a package from this
   * computer or install one already on the console, choosing the destination.
   * The replacement question comes from the daemon (409 already_installed), so
   * it is asked only when it actually applies.
   */
  var confirmPrompted = '';

  function renderInstall(st) {
    if (!dom.install) return;

    /* The helper reports a title that is already installed: ask before
     * replacing it, then repeat the request with the explicit overwrite. */
    if (st && st.needs_confirm) {
      var key = st.title_id || st.path;
      if (key && key !== confirmPrompted) {
        confirmPrompted = key;
        ZF.confirm({
          title: 'Already installed',
          message: (st.title_id || st.path) +
            ' is already installed. Replace it with this package?',
          confirmLabel: 'Replace',
          danger: true
        }).then(function (ok) {
          if (!ok) return;
          api.pkgInstall(st.path, st.slot === 1 ? 'extended' : 'internal', true)
            .then(function () { pollInstall(); })
            .catch(function (err) { ZF.toastError(err, 'Could not install the package'); });
        });
      }
      dom.install.hidden = true;
      return;
    }

    if (!st || (!st.active && !st.path)) { dom.install.hidden = true; return; }
    var failed = !!st.error;
    var completed = !st.active && !failed;
    var pct = completed ? 100 : st.length ? Math.max(0, Math.min(100, Math.round((st.transferred / st.length) * 100))) : (st.progress || 0);
    ZF.clear(dom.install);
    dom.install.appendChild(el('div', { class: 'xfer-top' }, [
      el('div', { class: 'xfer-name', text: 'Installing ' + (st.title_id || 'package') }),
      el('span', { class: 'status status-' + (failed ? 'error' : completed ? 'done' : 'active'),
        text: failed ? 'Failed' : (completed ? 'Completed' : 'Installing') })
    ]));
    dom.install.appendChild(el('div', { class: 'muted', text: st.path || '' }));
    if (failed) dom.install.appendChild(el('div', { class: 'xfer-error', text: st.detail || ('Installer error: ' + st.error) }));
    else if (st.active && st.detail) dom.install.appendChild(el('div', { class: 'muted', text: st.detail }));
    if (!failed) {
      dom.install.appendChild(el('div', { class: 'progress' + (pct >= 100 ? ' is-done' : ''),
        role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': String(pct) },
        el('span', { style: 'width:' + pct + '%' })));
      dom.install.appendChild(el('div', { class: 'xfer-meta', text: st.length
        ? ZF.bytes(st.transferred || 0) + ' of ' + ZF.bytes(st.length) + ' \u00b7 ' + pct + '%'
        : pct + '%' }));
    }
    dom.install.hidden = false;
  }

  function pollInstall() {
    clearTimeout(installTimer);
    /* Builds without the installer answer 409: keep quiet and re-check later,
     * so a reconnect to a build that has it keeps working. */
    if (!ZF.features.pkgInstall) {
      installTimer = setTimeout(pollInstall, 10000);
      return;
    }
    api.installStatus().then(function (st) {
      renderInstall(st);
      installTimer = setTimeout(pollInstall, st && st.active ? 2000 : 8000);
    }, function () {
      installTimer = setTimeout(pollInstall, 8000);
    });
  }

  function runInstall(path, dest, overwrite) {
    api.pkgInstall(path, dest, overwrite).then(function () {
      ZF.toast('Installation started', { type: 'success' });
      pollInstall();
    }).catch(function (err) {
      var data = err && err.data;
      if (data && data.error === 'already_installed') {
        ZF.confirm({
          title: 'Already installed',
          message: (data.title_id || path) +
            ' is already installed on this console. Replace it with this package?',
          confirmLabel: 'Replace',
          danger: true
        }).then(function (ok) { if (ok) runInstall(path, dest, true); });
        return;
      }
      ZF.toastError(err, 'Could not install the package');
    });
  }

  /**
   * Install a package: drop one from this computer or pick one already on the
   * console, with the destination chosen before the install starts.  The
   * default destination is the normal application install location.
   */
  function installPkg() {
    var source = null;
    var consolePath = null;
    var destination = 'internal';
    var mode = 'computer';
    var input = el('input', { type: 'file', accept: '.pkg,.fpkg,.ffpkg', hidden: true });
    var url = el('input', { type: 'url', class: 'input', placeholder: 'https://example.com/game.pkg', 'aria-label': 'Package URL' });
    var chosen = el('div', { class: 'pkg-selected muted', text: 'No package selected' });
    var progress = el('div', { class: 'pkg-progress muted', role: 'status', 'aria-live': 'polite' });
    var currentUpload = null;
    var abortUpload = ZF.button({ label: 'Stop sending', kind: 'ghost', size: 'sm', onclick: function () {
      if (currentUpload) currentUpload.abort();
    } });
    abortUpload.hidden = true;
    var zone = el('button', { type: 'button', class: 'pkg-dropzone' }, [
      ZF.icon('upload'), el('span', { text: 'Choose a PKG from this computer' }),
      el('small', { text: 'or drop it here' })
    ]);
    var computerBtn = el('button', { type: 'button', class: 'pkg-source is-active', text: 'Computer' });
    var consoleBtn = el('button', { type: 'button', class: 'pkg-source', text: 'Console' });
    var urlBtn = el('button', { type: 'button', class: 'pkg-source', text: 'Link' });
    var computerPane = el('div', { class: 'pkg-source-pane' }, [zone, input]);
    var consolePick = ZF.button({ label: 'Browse console files', icon: 'folder', onclick: explorer });
    var consolePane = el('div', { class: 'pkg-source-pane', hidden: true }, consolePick);
    var urlPane = el('div', { class: 'pkg-source-pane', hidden: true }, url);
    var internalBtn = el('button', { type: 'button', class: 'pkg-destination is-active', text: 'Internal storage' });
    var extendedBtn = el('button', { type: 'button', class: 'pkg-destination', text: 'Extended storage' });
    if (api.info && api.info.platform === 'ps5') {
      extendedBtn.disabled = true;
      extendedBtn.title = 'The PS5 system installer chooses its storage location';
    }

    function setDestination(value) {
      destination = value;
      internalBtn.className = 'pkg-destination' + (value === 'internal' ? ' is-active' : '');
      extendedBtn.className = 'pkg-destination' + (value === 'extended' ? ' is-active' : '');
    }
    internalBtn.addEventListener('click', function () { setDestination('internal'); });
    extendedBtn.addEventListener('click', function () { setDestination('extended'); });

    function setMode(value) {
      mode = value;
      computerBtn.className = 'pkg-source' + (value === 'computer' ? ' is-active' : '');
      consoleBtn.className = 'pkg-source' + (value === 'console' ? ' is-active' : '');
      urlBtn.className = 'pkg-source' + (value === 'url' ? ' is-active' : '');
      computerPane.hidden = value !== 'computer';
      consolePane.hidden = value !== 'console';
      urlPane.hidden = value !== 'url';
      chosen.textContent = value === 'computer' ?
        (source ? source.name + ' \u00b7 ' + ZF.bytes(source.size) : 'No package selected') :
        value === 'console' ? (consolePath || 'No package selected') :
        'The console will fetch this link directly';
      refresh();
    }
    computerBtn.addEventListener('click', function () { setMode('computer'); });
    consoleBtn.addEventListener('click', function () { setMode('console'); });
    urlBtn.addEventListener('click', function () { setMode('url'); });

    function pickFile(file) {
      if (!file) return;
      if (!/\.(pkg|fpkg|ffpkg)$/i.test(file.name)) {
        ZF.toast('Choose a PKG file', { type: 'error' });
        return;
      }
      source = file;
      chosen.textContent = file.name + ' \u00b7 ' + ZF.bytes(file.size);
      setMode('computer');
      refresh();
    }

    function pickConsolePath(path) {
      if (!path) return;
      consolePath = path;
      chosen.textContent = path;
      setMode('console');
      refresh();
    }

    var dialog = null;
    function refresh() {
      if (dialog) dialog.buttons.install.disabled = !(mode === 'computer' && source ||
        mode === 'console' && consolePath ||
        mode === 'url' && /^https?:\/\/\S+$/i.test(url.value.trim()));
    }

    zone.addEventListener('click', function () { input.click(); });
    zone.addEventListener('dragover', function (e) {
      e.preventDefault();
      zone.classList.add('is-over');
    });
    zone.addEventListener('dragleave', function () { zone.classList.remove('is-over'); });
    zone.addEventListener('drop', function (e) {
      e.preventDefault();
      zone.classList.remove('is-over');
      var files = e.dataTransfer && e.dataTransfer.files;
      if (files && files.length) pickFile(files[0]);
    });
    input.addEventListener('change', function () { pickFile(input.files && input.files[0]); });
    url.addEventListener('input', refresh);

    function explorer() {
      ZF.pickFolder({
        title: 'Choose a package on the console',
        start: ZF.settings.startPath || '/mnt',
        files: true,
        filter: function (name) { return /\.(pkg|fpkg|ffpkg)$/i.test(name); },
        confirmLabel: 'Use this package'
      }).then(pickConsolePath);
    }

    function requestInstall(value, overwrite) {
      return api.pkgInstall(value, destination, overwrite).catch(function (err) {
        var data = err && err.data;
        if (!data || data.error !== 'already_installed' || overwrite) throw err;
        return ZF.confirm({
          title: 'Already installed',
          message: (data.title_id || value) + ' is already installed. Replace it?',
          confirmLabel: 'Replace', danger: true
        }).then(function (ok) {
          if (!ok) return false;
          return requestInstall(value, true);
        });
      });
    }

    dialog = ZF.dialog({
      title: 'Install a package',
      size: 'md',
      content: el('div', { class: 'pkg-dialog' }, [
        el('p', { class: 'muted', text: 'Select where the PKG comes from. The console installs it through its system installer.' }),
        el('div', { class: 'pkg-field-label', text: 'Package source' }),
        el('div', { class: 'pkg-options' }, [computerBtn, consoleBtn, urlBtn]),
        computerPane, consolePane, urlPane,
        chosen,
        el('div', { class: 'pkg-field-label', text: 'Install to' }),
        el('div', { class: 'pkg-options' }, [internalBtn, extendedBtn]),
        el('div', { class: 'pkg-progress-row' }, [progress, abortUpload])
      ]),
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'install', label: 'Install', kind: 'primary', submit: true, disabled: true }
      ],
      onAction: function (id) {
        if (id !== 'install') return true;
        var work;
        if (mode === 'computer') {
          if (!source) return false;
          progress.textContent = 'Sending ' + source.name + '\u2026';
          var upload = api.upload(api.pkgStagingDir, source.name, source, function (loaded, total) {
            progress.textContent = 'Sending ' + source.name + ': ' + ZF.bytes(loaded) + ' of ' + ZF.bytes(total);
          });
          currentUpload = upload;
          abortUpload.hidden = false;
          var staged = api.pkgStagingDir + '/' + source.name;
          /* The staged copy only exists for the installer: when the install
           * never starts (replace declined, request refused) drop it, or it
           * keeps occupying space the console reports as "Other". */
          var discardStaged = function () { api.remove(staged).catch(function () {}); };
          work = upload.promise.then(function () {
            return requestInstall(staged, false).then(function (result) {
              if (result === false) discardStaged();
              return result;
            }, function (error) {
              discardStaged();
              throw error;
            });
          });
        } else if (mode === 'console') {
          if (!consolePath) return false;
          work = requestInstall(consolePath, false);
        } else {
          if (!/^https?:\/\/\S+$/i.test(url.value.trim())) return false;
          work = requestInstall(url.value.trim(), false);
        }
        return work.then(function (result) {
          if (result === false) return false;
          ZF.toast('Installation started', { type: 'success' });
          pollInstall();
          return true;
        }).then(function (result) {
          currentUpload = null;
          abortUpload.hidden = true;
          return result;
        }, function (error) {
          currentUpload = null;
          abortUpload.hidden = true;
          throw error;
        });
      }
    });
    refresh();
  }

  /** Adds the header action once the view markup is on the page. */
  function wireInstallButton() {
    if (dom.installWired || !ZF.features.pkgInstall) return;
    var actions = document.querySelector('#view-games .page-actions-wide');
    if (!actions) return;
    var btn = ZF.button({ label: 'Install PKG', size: 'sm', icon: 'package', onclick: installPkg });
    actions.insertBefore(btn, actions.firstChild);
    dom.installWired = true;
  }

  function trackDump(id) {
    trackedDumpId = id;
    notifiedDumpId = 0;
    pollDump();
  }

  function load() {
    wireInstallButton();
    var my = ++seq;
    if (!loaded) showMessage('Loading titles\u2026');
    return api.games().then(function (res) {
      if (my !== seq) return;
      games = (res.entries || []).slice().sort(function (a, b) { return ZF.naturalCompare(a.name || a.id, b.name || b.id); });
      loaded = true;
      error = null;
      render();
    }, function (err) {
      if (my !== seq) return;
      error = err;
      loaded = true;
      render();
    });
  }

  function showMessage(text, isError) {
    dom.list.className = 'card';
    ZF.clear(dom.list).appendChild(el('div', { class: 'empty-inline' + (isError ? ' text-danger' : ''), text: text }));
  }

  function stateIcon(name, isError) {
    return el('span', { class: 'list-state-icon' + (isError ? ' is-error' : '') }, ZF.icon(name, 'ic-xl'));
  }

  function render() {
    dom.repairAll.disabled = !!error || !games.length;
    if (error) {
      dom.sub.textContent = '';
      dom.list.className = 'card';
      ZF.clear(dom.list).appendChild(el('div', { class: 'list-state' }, [
        stateIcon('alert', true),
        el('div', { class: 'list-state-title', text: 'Could not load installed titles' }),
        el('p', { class: 'list-state-desc', text: error.status === 404 ? 'This build of zftpd does not include game management.' : error.message }),
        el('div', { class: 'list-state-actions' }, ZF.button({ label: 'Try again', icon: 'refresh', onclick: load }))
      ]));
      return;
    }
    dom.sub.textContent = games.length ? ZF.plural(games.length, 'title') : '';

    var q = dom.filter.value.trim().toLowerCase();
    var list = [];
    for (var i = 0; i < games.length; i++) {
      var g = games[i];
      if (!q || (g.name || '').toLowerCase().indexOf(q) >= 0 || (g.id || '').toLowerCase().indexOf(q) >= 0) list.push(g);
    }
    ZF.clear(dom.list);
    if (!games.length) {
      dom.list.className = 'card';
      dom.list.appendChild(el('div', { class: 'list-state' }, [
        stateIcon('gamepad'),
        el('div', { class: 'list-state-title', text: 'No titles found' }),
        el('p', { class: 'list-state-desc', text: 'Installed games and apps appear here. If a title is missing from the home screen, try Repair all.' })
      ]));
      return;
    }
    if (!list.length) {
      showMessage('No titles match \u201c' + dom.filter.value.trim() + '\u201d.');
      return;
    }
    dom.list.className = 'games-grid';
    for (var j = 0; j < list.length; j++) dom.list.appendChild(tile(list[j]));
  }

  function initials(name) {
    var words = String(name || '').replace(/[^A-Za-z0-9 ]+/g, ' ').split(/\s+/).filter(Boolean);
    if (!words.length) return '?';
    if (words.length === 1) return words[0].slice(0, 2).toUpperCase();
    return (words[0].charAt(0) + words[1].charAt(0)).toUpperCase();
  }

  function coverTone(id) {
    var h = 0;
    for (var i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) | 0;
    return 'cover-' + (Math.abs(h) % 6);
  }

  function fallbackCover(cover, g) {
    ZF.clear(cover);
    cover.className = 'game-cover ' + coverTone(g.id || g.name || '');
    cover.appendChild(el('span', { class: 'game-initials', 'aria-hidden': 'true', text: initials(g.name || g.id) }));
  }

  function tile(g) {
    var title = g.name || g.id;
    var cover = el('div', { class: 'game-cover' });
    if (g.has_icon) {
      var img = el('img', { src: api.gameIconUrl(g), alt: '', loading: 'lazy' });
      img.addEventListener('error', function () { fallbackCover(cover, g); });
      cover.appendChild(img);
    } else {
      fallbackCover(cover, g);
    }
    var launch = ZF.button({ icon: 'play', label: 'Launch', size: 'sm', cls: 'game-launch' });
    launch.addEventListener('click', function () { doLaunch(g, launch); });
    var more = ZF.button({ icon: 'more', aria: 'More actions for ' + title, title: 'More actions', size: 'sm' });
    more.addEventListener('click', function () {
      ZF.menu([
        { label: 'Repair home screen entry', icon: 'wrench', onclick: function () { repair(g); } },
        { label: 'Dump to this PC', icon: 'download', onclick: function () { dumpToPc(g); } },
        { label: 'Dump on the console\u2026', icon: 'archive', onclick: function () { dumpToConsole(g); } },
        g.path ? { label: 'Show in Files', icon: 'folder', onclick: function () { ZF.go(ZF.files.hashFor(g.path)); } } : null,
        { label: 'Copy title ID', icon: 'copy', onclick: function () {
          ZF.copyText(g.id).then(function () { ZF.toast('Title ID copied', { type: 'success', timeout: 2000 }); });
        } },
        '-',
        { label: 'Uninstall\u2026', icon: 'trash', danger: true, onclick: function () { uninstall(g); } }
      ], more);
    });
    return el('div', { class: 'game-tile' }, [
      el('div', { class: 'game-cover-wrap' }, [
        cover,
        g.source ? el('span', { class: 'game-src', text: g.source, title: g.path || g.source }) : null
      ]),
      el('div', { class: 'game-info' }, [
        el('div', { class: 'game-name', text: title, title: title }),
        (g.id && g.id !== title) ? el('div', { class: 'game-id', text: g.id }) : null
      ]),
      el('div', { class: 'game-actions' }, [launch, more])
    ]);
  }

  function doLaunch(g, btn) {
    btn.disabled = true;
    api.gameLaunch(g.id).then(function () {
      ZF.toast('Launching ' + (g.name || g.id) + ' on the console', { type: 'success' });
    }, function (err) {
      if (err.code === -30) {
        ZF.dialog({
          title: 'Title not registered',
          content: el('div', null, [
            el('p', { text: (g.name || g.id) + ' is not registered on this console\u2019s home screen, so it cannot be launched.' }),
            el('p', { class: 'muted', text: 'Repairing rebuilds the home screen entry from the installed files. It is safe to run.' })
          ]),
          actions: [{ id: 'cancel', label: 'Cancel' }, { id: 'repair', label: 'Repair and launch', kind: 'primary', submit: true }],
          onAction: function (id) {
            if (id !== 'repair') return true;
            return api.gameRepair(g.id).then(function () { return api.gameLaunch(g.id); }).then(function () {
              ZF.toast('Launching ' + (g.name || g.id), { type: 'success' });
            });
          }
        });
      } else {
        ZF.toastError(err, 'Could not launch ' + (g.name || g.id));
      }
    }).then(function () { btn.disabled = false; });
  }

  function repairMessage(res) {
    var rows = res && res.sqlite_repair && typeof res.sqlite_repair.rows === 'number' ? res.sqlite_repair.rows : null;
    if (rows === null) return 'Home screen entries checked';
    return rows ? 'Repaired ' + ZF.plural(rows, 'home screen entry', 'home screen entries') : 'Home screen entries are already up to date';
  }

  /* Decrypted dump of the running title: streamed straight to the browser, or
   * written on the console (USB, extended storage, /data…).  The daemon takes
   * the files from the title's sandbox mount, so the game has to be running;
   * when it is not, the daemon answers with a clear message. */
  function delay(ms) {
    return new Promise(function (resolve) { setTimeout(resolve, ms); });
  }

  /* The daemon launches the title and waits for its sandbox, so the download
   * starts when the status turns to "ready" — the dump is always the
   * decrypted sandbox content, never the encrypted install image. */
  function waitForReady(id, g) {
    return api.dumpStatus().then(function (st) {
      if (!st || st.id !== id) throw new Error('Dump not found');
      if (st.state === 'ready') {
        ZF.triggerDownload(api.dumpDownloadUrl(id), g.id + '.zip');
        /* The global status watcher reports completion or a failed download. */
        return null;
      }
      if (st.state === 'failed') throw new Error(st.message || 'Dump failed');
      return delay(1500).then(function () { return waitForReady(id, g); });
    });
  }

  function dumpToPc(g) {
    ZF.toast('Launching ' + (g.name || g.id) + ' and preparing the dump', { type: 'success' });
    api.dumpStart(g.id, { target: 'download', format: 'zip' }).then(function (res) {
      if (!res || res.ok === false) throw new Error((res && res.message) || 'Dump failed');
      trackDump(res.id);
      return waitForReady(res.id, g);
    }).catch(function (e) { ZF.toastError(e, 'Dump failed'); });
  }

  function dumpToConsole(g) {
    ZF.pickFolder({
      title: 'Dump ' + (g.name || g.id) + ' to',
      start: '/data',
      confirmLabel: 'Choose this folder'
    }).then(function (dest) {
      if (!dest) return null;
      return dumpOptions(g, dest);
    }).catch(function (e) { ZF.toastError(e, 'Dump failed'); });
  }

  /* Options for a console-side dump: the reference layout is a plain tree of
   * files, but a single ZIP is easier to move around; decryption stays on by
   * default because the encrypted containers are of no use anywhere else. */
  function dumpOptions(g, dest) {
    var format = el('select', { class: 'input' }, [
      el('option', { value: 'files', text: 'Folder with the files' }),
      el('option', { value: 'zip', text: 'One ZIP archive' })
    ]);
    var decrypt = el('input', { type: 'checkbox', checked: true });
    var content = el('div', null, [
      el('p', { class: 'muted mono', text: dest }),
      el('div', { class: 'field' }, [
        el('label', { class: 'field-label', text: 'Format' }),
        format
      ]),
      el('label', { class: 'switch' }, [
        decrypt,
        el('span', { text: 'Decrypt protected files (SELF/SPRX)' })
      ])
    ]);

    return ZF.dialog({
      title: 'Dump ' + (g.name || g.id),
      content: content,
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'ok', label: 'Start dump', kind: 'primary', submit: true }
      ],
      onAction: function (id) {
        if (id !== 'ok') return true;
        return api.dumpStart(g.id, {
          target: 'local',
          format: format.value,
          dest: dest,
          decrypt: decrypt.checked ? 1 : 0
        }).then(function (res) {
          if (!res || res.ok === false) throw new Error((res && res.message) || 'Dump failed');
          trackDump(res.id);
          ZF.toast('Dump running on the console: ' + (g.name || g.id), { type: 'success' });
        });
      }
    }).promise;
  }

  function repair(g) {
    api.gameRepair(g.id).then(function (res) {
      ZF.toast(repairMessage(res), { type: 'success' });
    }, function (err) { ZF.toastError(err, 'Repair failed'); });
  }

  dom.repairAll.addEventListener('click', function () {
    dom.repairAll.disabled = true;
    api.gameRepair().then(function (res) {
      ZF.toast(repairMessage(res), { type: 'success' });
      return load();
    }, function (err) { ZF.toastError(err, 'Repair failed'); }).then(function () { dom.repairAll.disabled = !games.length; });
  });

  function uninstall(g) {
    ZF.dialog({
      title: 'Uninstall \u201c' + (g.name || g.id) + '\u201d?',
      content: el('div', null, [
        el('p', { text: 'The title is removed from this console. This cannot be undone.' }),
        el('p', { class: 'muted mono', text: g.id + (g.path ? ' \u00b7 ' + g.path : '') })
      ]),
      actions: [{ id: 'cancel', label: 'Cancel' }, { id: 'ok', label: 'Uninstall', kind: 'danger', submit: true }],
      onOpen: function (d) { d.buttons.cancel.focus(); },
      onAction: function (id) {
        if (id !== 'ok') return true;
        return api.gameUninstall(g.id).then(function () {
          ZF.toast((g.name || g.id) + ' uninstalled', { type: 'success' });
          load();
        });
      }
    });
  }

  dom.filter.addEventListener('input', ZF.debounce(render, 80));
  dom.filter.addEventListener('keydown', function (e) {
    if ((e.key === 'Escape' || e.key === 'Esc') && dom.filter.value) { e.preventDefault(); dom.filter.value = ''; render(); }
  });
  dom.refresh.addEventListener('click', load);

  ZF.views.games = {
    title: 'Games',
    enter: function () { load(); pollInstall(); },
    leave: function () { clearTimeout(installTimer); }
  };
  pollDump();
})(ZF);
