/* zftpd web — Files view. */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var P = ZF.path;
  var api = ZF.api;
  var $ = ZF.byId;

  var S = {
    path: null,
    entries: [],
    view: [],
    hiddenCount: 0,
    index: {},
    sel: {},
    anchor: null,
    cursor: null,
    filter: '',
    loading: false,
    error: null,
    seq: 0,
    dirSizes: {},
    disk: null,
    clip: null
  };

  var dom = {
    view: $('view-files'),
    list: $('f-list'),
    rows: $('f-rows'),
    state: $('f-state'),
    crumbs: $('f-crumbs'),
    pathbar: $('f-pathbar'),
    pathInput: $('f-path-input'),
    filter: $('f-filter'),
    commands: $('f-commands'),
    checkAll: $('f-check-all'),
    count: $('f-count'),
    selinfo: $('f-selinfo'),
    disk: $('f-disk'),
    drop: $('f-drop'),
    dropPath: $('f-drop-path'),
    upFiles: $('f-upload-files'),
    upFolder: $('f-upload-folder'),
    back: $('f-back'),
    forward: $('f-forward'),
    up: $('f-up'),
    more: $('f-more'),
    table: ZF.$('#view-files .files-table')
  };

  function canWrite() { return !!api.csrf(); }

  /* ── Helpers ────────────────────────────────────────────────────────── */

  function up(node, test, stop) {
    while (node && node !== stop && node !== document) {
      if (test(node)) return node;
      node = node.parentNode;
    }
    return null;
  }
  function hasClass(node, cls) {
    return node && node.className && typeof node.className === 'string' &&
      (' ' + node.className + ' ').indexOf(' ' + cls + ' ') >= 0;
  }

  function hashFor(path) {
    var segs = P.segments(path);
    var out = [];
    for (var i = 0; i < segs.length; i++) out.push(encodeURIComponent(segs[i]));
    return '#/files' + (out.length ? '/' + out.join('/') : '');
  }

  function entryPath(e) { return P.join(S.path, e.name); }

  function selectedEntries() {
    var out = [];
    for (var i = 0; i < S.view.length; i++) if (S.sel[S.view[i].name]) out.push(S.view[i]);
    return out;
  }

  function selectedCount() {
    var n = 0;
    for (var k in S.sel) if (S.sel.hasOwnProperty(k) && S.sel[k]) n++;
    return n;
  }

  function existingNames() {
    var out = [];
    for (var i = 0; i < S.entries.length; i++) out.push(S.entries[i].name);
    return out;
  }

  function go(path) { ZF.go(hashFor(path)); }

  /* ── Loading ────────────────────────────────────────────────────────── */

  var loadingTimer = null;
  var pendingReveal = null;

  function reveal(path) {
    path = P.norm(path);
    pendingReveal = { dir: P.parent(path) || '/', name: P.base(path) };
    ZF.go(hashFor(pendingReveal.dir));
  }

  function load(path, opts) {
    opts = opts || {};
    path = P.norm(path);
    var my = ++S.seq;
    var sameDir = path === S.path;
    var focusName = opts.focusName || null;

    if (!sameDir && S.path && P.within(S.path, path) && S.path !== path) {
      focusName = P.segments(S.path)[P.segments(path).length];
    }
    if (!sameDir) {
      S.filter = '';
      dom.filter.value = '';
      S.dirSizes = {};
    }

    S.loading = true;
    renderCrumbs(path);
    dom.up.disabled = path === '/';

    clearTimeout(loadingTimer);
    if (!opts.silent) {
      loadingTimer = setTimeout(function () {
        if (my !== S.seq) return;
        ZF.clear(dom.rows);
        showState({ icon: 'refresh', title: 'Loading\u2026' });
      }, 180);
    }

    return api.list(path).then(function (res) {
      if (my !== S.seq) return;
      clearTimeout(loadingTimer);
      var keepScroll = sameDir ? dom.list.scrollTop : 0;
      S.loading = false;
      S.error = null;
      var prevPath = S.path;
      S.path = path;
      S.entries = normalize(res.entries || []);
      if (!sameDir) {
        S.sel = {};
        S.anchor = null;
        S.cursor = null;
      }
      rebuild();
      if (focusName && S.index.hasOwnProperty(focusName)) {
        S.cursor = focusName;
        if (opts.select) { S.sel = {}; S.sel[focusName] = true; S.anchor = focusName; }
      }
      renderRows();
      dom.list.scrollTop = keepScroll;
      if (S.cursor) scrollToName(S.cursor);
      if (prevPath !== path) {
        ZF.emit('files-path', path);
        loadDisk();
      }
      if (opts.focus) dom.list.focus();
    }, function (err) {
      if (my !== S.seq) return;
      clearTimeout(loadingTimer);
      S.loading = false;
      if (opts.silent && sameDir) return;
      S.path = path;
      S.entries = [];
      S.view = [];
      S.sel = {};
      S.error = err;
      ZF.clear(dom.rows);
      ZF.emit('files-path', path);
      renderAll();
    });
  }

  function normalize(entries) {
    var out = [];
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      var isDir = e.type === 'directory';
      out.push({
        name: e.name,
        isDir: isDir,
        size: isDir ? -1 : (typeof e.size === 'number' ? e.size : -1),
        /* Undefined on daemons that do not send it. */
        mode: (typeof e.mode === 'number' && isFinite(e.mode)) ? (e.mode & 511) : undefined,
        ft: ZF.fileType(e.name, isDir)
      });
    }
    return out;
  }

  function loadDisk() {
    var at = S.path;
    api.stats(at).then(function (st) {
      if (at !== S.path) return;
      S.disk = st && st.disk_total ? { free: st.disk_free, total: st.disk_total, used: st.disk_used } : null;
      renderStatus();
    }, function () { /* optional */ });
  }

  function reload(silent) {
    if (S.path !== null) return load(S.path, { silent: silent !== false });
    return Promise.resolve();
  }

  /* ── Derived view: hide, filter, sort ───────────────────────────────── */

  function sortFn() {
    var key = ZF.settings.sortKey || 'name';
    var dir = ZF.settings.sortDir === -1 ? -1 : 1;
    return function (a, b) {
      if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
      var r = 0;
      if (key === 'size') {
        var as = a.isDir ? (S.dirSizes[a.name] || -1) : a.size;
        var bs = b.isDir ? (S.dirSizes[b.name] || -1) : b.size;
        r = as === bs ? 0 : (as < bs ? -1 : 1);
      } else if (key === 'type') {
        r = ZF.naturalCompare(a.ft.label, b.ft.label);
      }
      if (r === 0) r = ZF.naturalCompare(a.name, b.name);
      return r * dir;
    };
  }

  function rebuild() {
    var q = S.filter.toLowerCase();
    var out = [];
    var hidden = 0;
    for (var i = 0; i < S.entries.length; i++) {
      var e = S.entries[i];
      if (!ZF.settings.showHidden && e.name.charAt(0) === '.') { hidden++; continue; }
      if (!ZF.settings.showProtected && ZF.isProtected(S.path, e.name)) { hidden++; continue; }
      if (q && e.name.toLowerCase().indexOf(q) < 0) continue;
      out.push(e);
    }
    out.sort(sortFn());
    S.view = out;
    S.hiddenCount = hidden;
    S.index = {};
    for (var j = 0; j < out.length; j++) S.index[out[j].name] = j;
    var sel = {};
    for (var k in S.sel) if (S.sel.hasOwnProperty(k) && S.sel[k] && S.index.hasOwnProperty(k)) sel[k] = true;
    S.sel = sel;
    if (S.cursor && !S.index.hasOwnProperty(S.cursor)) S.cursor = null;
    if (S.anchor && !S.index.hasOwnProperty(S.anchor)) S.anchor = null;
  }

  /* ── Rendering ──────────────────────────────────────────────────────── */

  function renderCrumbs(path) {
    ZF.renderCrumbs(dom.crumbs, path, function (p) { go(p); }, 6);
  }

  function sizeCell(e) {
    if (!e.isDir) return ZF.bytes(e.size);
    if (S.dirSizes.hasOwnProperty(e.name)) return ZF.bytes(S.dirSizes[e.name]);
    return '';
  }

  function isCut(e) {
    return S.clip && S.clip.mode === 'cut' && S.clip.from === S.path && S.clip.names.indexOf(e.name) >= 0;
  }

  function rowHtml(e, i) {
    var esc = ZF.esc;
    var cls = [];
    if (S.sel[e.name]) cls.push('is-selected');
    if (S.cursor === e.name) cls.push('is-cursor');
    if (isCut(e)) cls.push('is-cut');
    var name = esc(e.name);
    return '<tr data-i="' + i + '"' + (cls.length ? ' class="' + cls.join(' ') + '"' : '') +
      ' aria-selected="' + (S.sel[e.name] ? 'true' : 'false') + '">' +
      '<td class="col-check"><input type="checkbox" class="check" tabindex="-1" aria-label="Select ' + name + '"' +
      (S.sel[e.name] ? ' checked' : '') + '></td>' +
      '<td class="col-name"><div class="fname">' + ZF.iconHtml(e.ft.icon, 'ft-' + e.ft.kind) +
      '<span class="fname-text" title="' + name + '">' + name + '</span></div></td>' +
      '<td class="col-type">' + esc(e.ft.label) + '</td>' +
      '<td class="col-size">' + sizeCell(e) + '</td>' +
      '<td class="col-actions"><button type="button" class="btn btn-ghost btn-icon btn-sm row-menu" tabindex="-1" ' +
      'aria-label="Actions for ' + name + '" title="Actions">' + ZF.iconHtml('more') + '</button></td></tr>';
  }

  function renderRows() {
    var html = [];
    for (var i = 0; i < S.view.length; i++) html.push(rowHtml(S.view[i], i));
    dom.rows.innerHTML = html.join('');
    renderAll();
  }

  function renderAll() {
    renderSortHeaders();
    renderEmptyState();
    renderCommands();
    renderStatus();
    renderCheckAll();
  }

  function showState(o) {
    ZF.clear(dom.state);
    dom.state.appendChild(el('span', { class: 'list-state-icon' + (o.icon === 'alert' ? ' is-error' : '') }, ZF.icon(o.icon, 'ic-xl')));
    dom.state.appendChild(el('div', { class: 'list-state-title', text: o.title }));
    if (o.desc) dom.state.appendChild(el('p', { class: 'list-state-desc', text: o.desc }));
    if (o.actions && o.actions.length) {
      var box = el('div', { class: 'list-state-actions' });
      for (var i = 0; i < o.actions.length; i++) box.appendChild(ZF.button(o.actions[i]));
      dom.state.appendChild(box);
    }
    dom.state.hidden = false;
    dom.table.tHead.hidden = !!o.hideHead;
  }

  function hideState() {
    dom.state.hidden = true;
    dom.table.tHead.hidden = false;
  }

  function renderEmptyState() {
    if (S.error) {
      var e = S.error;
      var denied = e.status === 403;
      var missing = e.status === 404;
      showState({
        icon: 'alert',
        title: denied ? 'Access denied' : missing ? 'Folder not found' : 'This folder could not be opened',
        desc: e.message,
        hideHead: true,
        actions: [
          { label: 'Try again', icon: 'refresh', onclick: function () { load(S.path); } },
          S.path !== '/' ? { label: 'Go to parent folder', icon: 'arrow-up', kind: 'primary', onclick: function () { go(P.parent(S.path)); } } : null
        ].filter(Boolean)
      });
      return;
    }
    if (S.view.length) { hideState(); return; }
    if (S.filter) {
      showState({
        icon: 'search', title: 'No matches', hideHead: false,
        desc: 'Nothing in this folder matches \u201c' + S.filter + '\u201d.',
        actions: [{ label: 'Clear filter', onclick: function () { setFilter(''); dom.filter.focus(); } }]
      });
      return;
    }
    var hiddenNote = S.hiddenCount ? ZF.plural(S.hiddenCount, 'hidden item') + ' not shown. ' : '';
    showState({
      icon: 'folder', title: 'This folder is empty', hideHead: true,
      desc: hiddenNote + (canWrite() ? 'Drop files here or use Upload to add content.' : ''),
      actions: canWrite() ? [
        { label: 'Upload files', icon: 'upload', kind: 'primary', onclick: function () { dom.upFiles.click(); } },
        { label: 'New folder', icon: 'folder-plus', onclick: newFolder }
      ] : []
    });
  }

  function renderSortHeaders() {
    var key = ZF.settings.sortKey || 'name';
    var desc = ZF.settings.sortDir === -1;
    var btns = ZF.$$('.th-sort', dom.table);
    for (var i = 0; i < btns.length; i++) {
      var on = btns[i].getAttribute('data-sort') === key;
      btns[i].className = 'th-sort' + (on ? ' is-sorted' : '') + (on && desc ? ' is-desc' : '');
      btns[i].parentNode.setAttribute('aria-sort', on ? (desc ? 'descending' : 'ascending') : 'none');
    }
  }

  function renderCheckAll() {
    var n = selectedCount();
    dom.checkAll.checked = n > 0 && n === S.view.length;
    dom.checkAll.indeterminate = n > 0 && n < S.view.length;
    dom.checkAll.disabled = !S.view.length;
  }

  function renderStatus() {
    if (S.error) {
      dom.count.textContent = '';
    } else if (S.loading && !S.entries.length) {
      dom.count.textContent = 'Loading\u2026';
    } else {
      var total = S.entries.length - S.hiddenCount;
      var txt = S.filter ? S.view.length + ' of ' + ZF.plural(total, 'item') : ZF.plural(total, 'item');
      if (S.hiddenCount) txt += ' \u00b7 ' + S.hiddenCount + ' hidden';
      if (!canWrite()) txt += ' \u00b7 Read-only';
      dom.count.textContent = txt;
    }

    var sel = selectedEntries();
    if (sel.length) {
      var bytes = 0, files = 0;
      for (var i = 0; i < sel.length; i++) if (!sel[i].isDir) { bytes += sel[i].size; files++; }
      dom.selinfo.textContent = sel.length + ' selected' + (files ? ' \u00b7 ' + ZF.bytes(bytes) : '');
    } else {
      dom.selinfo.textContent = '';
    }

    ZF.clear(dom.disk);
    if (S.disk && S.disk.total) {
      var pct = ZF.percent(S.disk.total - S.disk.free, S.disk.total);
      dom.disk.appendChild(el('span', { text: ZF.bytes(S.disk.free) + ' free of ' + ZF.bytes(S.disk.total) }));
      var meter = el('span', { class: 'meter' + (pct > 95 ? ' is-danger' : pct > 85 ? ' is-warn' : ''), title: Math.round(pct) + '% used' },
        el('span', { style: 'width:' + pct.toFixed(1) + '%' }));
      dom.disk.appendChild(meter);
    }
  }

  function paintSelection() {
    var rows = dom.rows.rows;
    for (var i = 0; i < rows.length; i++) {
      var e = S.view[i];
      if (!e) continue;
      var sel = !!S.sel[e.name];
      var cls = (sel ? 'is-selected' : '') + (S.cursor === e.name ? ' is-cursor' : '') + (isCut(e) ? ' is-cut' : '');
      if (rows[i].className !== cls) rows[i].className = cls;
      rows[i].setAttribute('aria-selected', sel ? 'true' : 'false');
      var cb = rows[i].cells[0].firstChild;
      if (cb.checked !== sel) cb.checked = sel;
    }
    renderCommands();
    renderStatus();
    renderCheckAll();
  }

  /* ── Command bar ────────────────────────────────────────────────────── */

  function cmd(o) {
    var b = ZF.button({
      icon: o.icon, label: o.label, kind: o.kind || (o.primary ? 'primary' : null),
      title: o.title || o.label, aria: o.label, disabled: o.disabled, onclick: o.onclick,
      cls: (o.collapse ? 'btn-collapse' : '') + (o.cls ? ' ' + o.cls : '')
    });
    if (o.collapse && o.label) b.setAttribute('aria-label', o.label);
    return b;
  }

  function renderCommands() {
    var bar = dom.commands;
    ZF.clear(bar);
    var sel = selectedEntries();
    var write = canWrite();
    if (S.error) {
      bar.appendChild(el('span', { class: 'sel-summary', text: 'Folder unavailable' }));
      return;
    }

    if (!sel.length) {
      if (write) {
        var upBtn = cmd({ icon: 'upload', label: 'Upload', primary: true });
        upBtn.appendChild(ZF.icon('chevron-down'));
        upBtn.setAttribute('aria-haspopup', 'menu');
        upBtn.addEventListener('click', function () {
          ZF.menu([
            { label: 'Upload files', icon: 'file', onclick: function () { dom.upFiles.click(); } },
            { label: 'Upload folder', icon: 'folder', onclick: function () { dom.upFolder.click(); } }
          ], upBtn, { alignLeft: true });
        });
        bar.appendChild(upBtn);
        bar.appendChild(cmd({ icon: 'folder-plus', label: 'New folder', collapse: true, onclick: newFolder }));
        bar.appendChild(cmd({ icon: 'file-plus', label: 'New file', collapse: true, onclick: newFile }));
        if (S.clip) {
          bar.appendChild(el('span', { class: 'cmd-sep' }));
          bar.appendChild(cmd({
            icon: S.clip.mode === 'cut' ? 'move' : 'copy',
            label: 'Paste ' + ZF.plural(S.clip.paths.length, 'item'),
            title: (S.clip.mode === 'cut' ? 'Move' : 'Copy') + ' here (Ctrl+V)',
            onclick: paste
          }));
          bar.appendChild(cmd({ icon: 'x', aria: 'Clear clipboard', title: 'Clear clipboard', kind: 'ghost', onclick: clearClip }));
        }
      } else {
        bar.appendChild(el('span', { class: 'sel-summary', text: 'Read-only: this build does not allow changes from the browser.' }));
      }
      bar.appendChild(el('span', { class: 'spacer' }));
      bar.appendChild(cmd({ icon: 'refresh', title: 'Refresh', aria: 'Refresh', kind: 'ghost', onclick: function () { reload(false); } }));
      return;
    }

    var one = sel.length === 1 ? sel[0] : null;
    var allFiles = true;
    for (var i = 0; i < sel.length; i++) if (sel[i].isDir) allFiles = false;

    bar.appendChild(cmd({ icon: 'x', title: 'Clear selection (Esc)', aria: 'Clear selection', kind: 'ghost', onclick: clearSelection }));
    var summary = el('span', { class: 'sel-summary' });
    summary.appendChild(el('strong', { text: String(sel.length) }));
    summary.appendChild(document.createTextNode(' selected'));
    bar.appendChild(summary);
    bar.appendChild(el('span', { class: 'cmd-sep' }));

    if (one && one.isDir) {
      bar.appendChild(cmd({ icon: 'folder', label: 'Open', onclick: function () { openEntry(one); } }));
    }
    bar.appendChild(cmd({
      icon: 'download', label: 'Download', collapse: true, disabled: !allFiles,
      title: allFiles ? 'Download' : 'Folders cannot be downloaded from the browser. Use FTP instead.',
      onclick: function () { download(sel); }
    }));
    bar.appendChild(cmd({
      icon: 'archive', label: 'ZIP', collapse: true,
      title: 'Download the selection as a ZIP archive (folders included)',
      onclick: function () { downloadZip(sel); }
    }));
    if (write) {
      if (one) bar.appendChild(cmd({ icon: 'pencil', label: 'Rename', collapse: true, title: 'Rename (F2)', onclick: function () { rename(one); } }));
      if (one) bar.appendChild(cmd({ icon: 'shield', label: 'Permissions', collapse: true, title: 'Permissions', onclick: function () { permissions(one); } }));
      bar.appendChild(cmd({ icon: 'copy', label: 'Copy to\u2026', collapse: true, onclick: function () { transferTo('copy', sel); } }));
      bar.appendChild(cmd({ icon: 'move', label: 'Move to\u2026', collapse: true, onclick: function () { transferTo('move', sel); } }));
      if (one && !one.isDir && ZF.isArchive(one.name)) {
        var ex = cmd({ icon: 'archive', label: 'Extract', collapse: true });
        ex.addEventListener('click', function () { ZF.menu(extractItems(one), ex, { alignLeft: true }); });
        bar.appendChild(ex);
      }
      bar.appendChild(cmd({ icon: 'trash', label: 'Delete', collapse: true, cls: 'btn-danger-text', title: 'Delete (Del)', onclick: function () { remove(sel); } }));
    }
    bar.appendChild(el('span', { class: 'spacer' }));
    if (one) bar.appendChild(cmd({ icon: 'info', title: 'Details', aria: 'Details', kind: 'ghost', onclick: function () { details(one); } }));
    var more = cmd({ icon: 'more', title: 'More actions', aria: 'More actions', kind: 'ghost' });
    more.addEventListener('click', function () { ZF.menu(itemMenu(sel, true), more); });
    bar.appendChild(more);
  }

  /* ── Selection ──────────────────────────────────────────────────────── */

  function selectOnly(name) {
    S.sel = {};
    if (name !== null) S.sel[name] = true;
    S.anchor = name;
  }

  function selectRange(from, to, additive) {
    var a = S.index[from], b = S.index[to];
    if (a === undefined || b === undefined) { selectOnly(to); return; }
    if (!additive) S.sel = {};
    var lo = Math.min(a, b), hi = Math.max(a, b);
    for (var i = lo; i <= hi; i++) S.sel[S.view[i].name] = true;
  }

  function clearSelection() {
    S.sel = {};
    S.anchor = null;
    paintSelection();
  }

  function selectAll() {
    S.sel = {};
    for (var i = 0; i < S.view.length; i++) S.sel[S.view[i].name] = true;
    paintSelection();
  }

  function invertSelection() {
    var next = {};
    for (var i = 0; i < S.view.length; i++) if (!S.sel[S.view[i].name]) next[S.view[i].name] = true;
    S.sel = next;
    paintSelection();
  }

  function setCursor(name, scroll) {
    S.cursor = name;
    paintSelection();
    if (scroll) scrollToName(name);
  }

  function scrollToName(name) {
    var i = S.index[name];
    if (i === undefined) return;
    var row = dom.rows.rows[i];
    if (!row) return;
    var head = dom.table.tHead.offsetHeight || 0;
    var top = row.offsetTop;
    var bottom = top + row.offsetHeight;
    var viewTop = dom.list.scrollTop + head;
    var viewBottom = dom.list.scrollTop + dom.list.clientHeight;
    if (top < viewTop) dom.list.scrollTop = top - head;
    else if (bottom > viewBottom) dom.list.scrollTop = bottom - dom.list.clientHeight;
  }

  /* ── Actions ────────────────────────────────────────────────────────── */

  function openEntry(e) {
    if (e.isDir) go(entryPath(e));
    else details(e);
  }

  /* One archive streamed by the daemon: works for folders too, and for
   * selections far larger than the browser's multiple-download limit. */
  function downloadZip(entries) {
    if (!entries.length) return;
    var paths = [];
    for (var i = 0; i < entries.length; i++) paths.push(entryPath(entries[i]));
    var name = (entries.length === 1 ? entries[0].name : 'zftpd-selection') + '.zip';

    ZF.toast('Preparing ' + ZF.plural(entries.length, 'item') + ' for ' + name, { type: 'success' });
    api.prepareZip(paths, name).then(function (res) {
      if (!res || res.ok === false || !res.id) {
        throw new Error((res && res.message) || 'Could not prepare the archive');
      }
      ZF.triggerDownload(api.zipDownloadUrl(res.id), name);
    }, function (e) {
      ZF.toastError(e, 'ZIP failed');
    });
  }

  /* Ask the daemon before saving a SELF container that may come out encrypted. */
  function downloadChecked(e) {
    var path = entryPath(e);
    api.decryptCheck(path).then(function (res) {
      if (!res || !res.self) {
        ZF.triggerDownload(api.fileUrl(path, false), e.name);
        return;
      }
      if (res.decryptable) {
        ZF.triggerDownload(api.fileUrl(path, true), e.name);
        return;
      }
      ZF.confirm({
        title: 'Download the original?',
        message: e.name + ' is a SELF container whose payload cannot be decrypted.',
        note: res.reason || 'The daemon did not report a reason.',
        noteKind: 'warning',
        confirmLabel: 'Download original',
        cancelLabel: 'Cancel'
      }).then(function (ok) {
        if (ok) ZF.triggerDownload(api.fileUrl(path, false), e.name);
      });
    }, function () {
      ZF.triggerDownload(api.fileUrl(path), e.name);
    });
  }

  function download(entries) {
    var files = entries.filter(function (e) { return !e.isDir; });
    if (!files.length) return;
    if (ZF.settings.decryptSelf && files.length === 1) {
      downloadChecked(files[0]);
      return;
    }
    for (var i = 0; i < files.length; i++) {
      (function (e, delay) {
        setTimeout(function () { ZF.triggerDownload(api.fileUrl(entryPath(e)), e.name); }, delay);
      })(files[i], i * 600);
    }
    if (files.length > 1) ZF.toast('Downloading ' + files.length + ' files. Your browser may ask to allow multiple downloads.');
  }

  function newFolder() {
    var dir = S.path;
    ZF.prompt({
      title: 'New folder', label: 'Name', value: 'New folder', confirmLabel: 'Create',
      validate: function (v) { return ZF.validateName(v, existingNames()); },
      submit: function (v) { return api.mkdir(dir, v); }
    }).then(function (name) {
      if (name && dir === S.path) load(dir, { silent: true, focusName: name, select: true });
    });
  }

  function newFile() {
    var dir = S.path;
    ZF.prompt({
      title: 'New file', label: 'Name', value: 'untitled.txt', confirmLabel: 'Create', selectBase: true,
      hint: 'Creates an empty file.',
      validate: function (v) { return ZF.validateName(v, existingNames()); },
      submit: function (v) { return api.createFile(dir, v); }
    }).then(function (name) {
      if (name && dir === S.path) load(dir, { silent: true, focusName: name, select: true });
    });
  }

  function rename(e) {
    var dir = S.path;
    var names = existingNames();
    ZF.prompt({
      title: 'Rename', label: 'New name', value: e.name, confirmLabel: 'Rename', selectBase: !e.isDir,
      validate: function (v) {
        if (v === e.name) return null;
        return ZF.validateName(v, names, e.name);
      },
      submit: function (v) {
        if (v === e.name) return Promise.resolve();
        return api.rename(P.join(dir, e.name), v);
      }
    }).then(function (name) {
      if (name && name !== e.name && dir === S.path) {
        if (S.dirSizes.hasOwnProperty(e.name)) { S.dirSizes[name] = S.dirSizes[e.name]; delete S.dirSizes[e.name]; }
        load(dir, { silent: true, focusName: name, select: true });
        ZF.emit('bookmark-renamed', { from: P.join(dir, e.name), to: P.join(dir, name) });
      }
    });
  }

  /* Permission editor: pre-filled from e.mode, 644/755 when the daemon is old. */
  function permissions(e) {
    var path = entryPath(e);
    var dir = S.path;
    var known = typeof e.mode === 'number';
    var initial = known ? (e.mode & 511) : (e.isDir ? 493 : 420);
    var boxes = [];
    var octal = el('span', { class: 'perm-octal mono', text: '' });
    var symbolic = el('span', { class: 'mono muted', text: '' });

    /* Boxes are owner-rwx, group-rwx, others-rwx: bit 0400 down to 0001. */
    function bits() {
      var m = 0;
      for (var i = 0; i < boxes.length; i++) if (boxes[i].checked) m |= 1 << (8 - i);
      return m;
    }

    function pad3(s) { return s.length >= 3 ? s.slice(-3) : ('000' + s).slice(-3); }

    function rwx(m) {
      var out = '';
      for (var i = 8; i >= 0; i--) out += (m & (1 << i)) ? 'rwx'.charAt((8 - i) % 3) : '-';
      return out;
    }

    function update() {
      var m = bits();
      octal.textContent = pad3(m.toString(8));
      symbolic.textContent = rwx(m);
    }

    var groups = [
      { label: 'Owner', values: [256, 128, 64] },
      { label: 'Group', values: [32, 16, 8] },
      { label: 'Others', values: [4, 2, 1] }
    ];
    var names = ['Read', 'Write', 'Execute'];
    var grid = el('div', { class: 'perm-grid' });

    for (var g = 0; g < groups.length; g++) {
      (function (grp) {
        var row = el('div', { class: 'perm-row' }, el('div', { class: 'perm-group', text: grp.label }));
        for (var b = 0; b < 3; b++) {
          (function (bit, letter, name) {
            var input = el('input', {
              type: 'checkbox', class: 'check', checked: (initial & bit) !== 0, onchange: update,
              'aria-label': grp.label + ' ' + name.toLowerCase()
            });
            row.appendChild(el('label', { class: 'perm-box' }, [
              input,
              el('span', { text: name + ' (' + letter + ')' })
            ]));
            boxes.push(input);
          })(grp.values[b], 'rwx'.charAt(b), names[b]);
        }
        grid.appendChild(row);
      })(groups[g]);
    }

    var content = el('div', null, [
      el('div', { class: 'perm-target' }, [
        el('div', { class: 'perm-name', text: e.name }),
        el('div', { class: 'perm-path mono muted', text: path })
      ]),
      el('div', { class: 'perm-readout' }, [octal, symbolic]),
      grid,
      known ? null : el('div', { class: 'note' }, [ZF.icon('info'), el('span', {
        text: 'This daemon did not report the current permissions: the boxes show the default for a ' +
          (e.isDir ? 'folder (755).' : 'file (644).')
      })]),
      e.isDir ? el('div', { class: 'field-hint', text: 'This changes the folder itself. Items inside it keep their own permissions.' }) : null
    ]);
    update();

    ZF.dialog({
      title: 'Permissions',
      content: content,
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'apply', label: 'Apply', kind: 'primary', submit: true }
      ],
      onOpen: function () { boxes[0].focus(); },
      onAction: function (id) {
        if (id !== 'apply') return true;
        var mode = pad3(bits().toString(8));
        return api.chmod(path, mode).then(function (res) {
          var effective = (res && res.mode) ? String(res.mode) : mode;
          if (res && res.supported === false) {
            ZF.toast('This filesystem ignored the permission change. Effective mode: ' + effective, { type: 'warning' });
          } else {
            ZF.toast('Permissions changed to ' + effective, { type: 'success' });
          }
          if (dir === S.path) load(dir, { silent: true });
          return true;
        }, function (err) {
          ZF.toastError(err, 'Could not change permissions');
          return false;
        });
      }
    });
  }

  function remove(entries) {
    if (!entries.length) return;
    var dir = S.path;
    var dirs = 0;
    for (var i = 0; i < entries.length; i++) if (entries[i].isDir) dirs++;
    var title = entries.length === 1
      ? 'Delete \u201c' + entries[0].name + '\u201d?'
      : 'Delete ' + entries.length + ' items?';
    var names = entries.map(function (e) { return e.name + (e.isDir ? '/' : ''); });
    var failures = [];

    ZF.dialog({
      title: title,
      content: el('div', null, [
        el('p', { text: 'This permanently deletes ' + (entries.length === 1 ? 'the item' : 'these items') + ' from ' + dir + '. It cannot be undone.' }),
        entries.length > 1 ? el('ul', { class: 'name-list' }, names.slice(0, 8).map(function (n) { return el('li', { text: n }); })
          .concat(names.length > 8 ? [el('li', { class: 'subtle', text: 'and ' + (names.length - 8) + ' more' })] : [])) : null,
        dirs ? el('div', { class: 'note note-warning' }, [ZF.icon('alert'), el('span', {
          text: dirs === 1 && entries.length === 1 ? 'The folder and everything inside it will be deleted.' : 'Folders are deleted together with everything inside them.'
        })]) : null
      ]),
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'delete', label: 'Delete', kind: 'danger', submit: true }
      ],
      onOpen: function (d) { d.buttons.cancel.focus(); },
      onAction: function (id, d) {
        if (id !== 'delete') return true;
        d.buttons['delete'].textContent = 'Deleting\u2026';
        function next(i) {
          if (i >= entries.length) return Promise.resolve();
          var e = entries[i];
          return api.remove(P.join(dir, e.name), e.isDir).then(null, function (err) {
            failures.push(e.name + ': ' + err.message);
            if (err.offline) throw err;
          }).then(function () { return next(i + 1); });
        }
        return next(0).then(function () { return true; }, function () { return true; });
      }
    }).promise.then(function (id) {
      if (id !== 'delete') return;
      var removed = entries.length - failures.length;
      if (failures.length) {
        ZF.toast(failures.length === 1 ? 'Could not delete ' + failures[0] : failures.length + ' items could not be deleted. ' + failures[0], { type: 'error' });
      } else {
        ZF.toast(removed === 1 ? 'Deleted \u201c' + entries[0].name + '\u201d' : 'Deleted ' + removed + ' items', { type: 'success' });
      }
      for (var k = 0; k < entries.length; k++) ZF.emit('path-removed', P.join(dir, entries[k].name));
      if (dir === S.path) {
        S.sel = {};
        load(dir, { silent: true });
      }
    });
  }

  function transferTo(mode, entries) {
    var srcs = entries.map(entryPath);
    ZF.pickFolder({
      title: (mode === 'move' ? 'Move ' : 'Copy ') + (entries.length === 1 ? '\u201c' + entries[0].name + '\u201d' : entries.length + ' items'),
      start: S.path,
      confirmLabel: mode === 'move' ? 'Move here' : 'Copy here',
      check: function (dest) { return ZF.jobs.transferBlocker(srcs, dest, mode); }
    }).then(function (dest) {
      if (!dest) return;
      ZF.jobs.transfer(mode, srcs, dest).then(function (job) {
        if (job && mode === 'move') { S.sel = {}; paintSelection(); }
      });
    });
  }

  function setClip(mode, entries) {
    S.clip = { mode: mode, from: S.path, names: entries.map(function (e) { return e.name; }), paths: entries.map(entryPath) };
    paintSelection();
    ZF.toast((mode === 'cut' ? 'Ready to move ' : 'Copied ') + ZF.plural(entries.length, 'item') + '. Open a folder and press Paste.', { timeout: 3000 });
  }

  function clearClip() {
    S.clip = null;
    paintSelection();
  }

  function paste() {
    if (!S.clip) return;
    var clip = S.clip;
    ZF.jobs.transfer(clip.mode === 'cut' ? 'move' : 'copy', clip.paths, S.path).then(function (job) {
      if (job && clip.mode === 'cut') { S.clip = null; paintSelection(); }
    });
  }

  function extractItems(e) {
    var archive = entryPath(e);
    var dir = S.path;
    var base = e.name.replace(/\.(tar\.(gz|bz2|xz|zst)|[^.]+)$/i, '') || 'extracted';
    return [
      { label: 'Extract to \u201c' + base + '/\u201d', icon: 'folder-plus', onclick: function () { ZF.jobs.extract(archive, dir, base); } },
      { label: 'Extract here', icon: 'archive', onclick: function () { ZF.jobs.extract(archive, dir); } },
      { label: 'Extract to\u2026', icon: 'folder', onclick: function () {
        ZF.pickFolder({ title: 'Extract \u201c' + e.name + '\u201d', start: dir, confirmLabel: 'Extract here' }).then(function (dest) {
          if (dest) ZF.jobs.extract(archive, dest);
        });
      } }
    ];
  }

  function copyPaths(entries) {
    var text = entries.map(entryPath).join('\n');
    ZF.copyText(text).then(function () {
      ZF.toast(entries.length === 1 ? 'Path copied' : entries.length + ' paths copied', { type: 'success', timeout: 2000 });
    }, function () {
      ZF.prompt({ title: 'Copy path', label: 'Path', value: text, confirmLabel: 'Done' });
    });
  }

  function isBookmarked(path) { return ZF.settings.bookmarks.indexOf(path) >= 0; }

  function toggleBookmark(path) {
    var list = ZF.settings.bookmarks.slice();
    var i = list.indexOf(path);
    if (i >= 0) list.splice(i, 1); else list.push(path);
    ZF.setSetting('bookmarks', list);
    ZF.toast(i >= 0 ? 'Removed from Places' : 'Added to Places', { type: 'success', timeout: 2000 });
  }

  /**
   * Install a package from the console copy at @p one.
   *
   * The destination is asked first; the replacement question is asked only when
   * the daemon reports that the very same title is already installed, so the
   * common path stays one dialog long.
   */
  function installPackage(one) {
    var path = entryPath(one);

    function run(dest, overwrite) {
      api.pkgInstall(path, dest, overwrite).then(function () {
        ZF.toast('Installing ' + one.name +
          (dest === 'extended' ? ' to extended storage' : ' to internal storage') + '\u2026',
          { type: 'success' });
      }).catch(function (err) {
        var data = err && err.data;
        if (data && data.error === 'already_installed') {
          ZF.confirm({
            title: 'Already installed',
            message: (data.title_id || one.name) +
              ' is already installed on this console. Replace it with this package?',
            confirmLabel: 'Replace',
            danger: true
          }).then(function (ok) { if (ok) run(dest, true); });
          return;
        }
        ZF.toastError(err, 'Could not install ' + one.name);
      });
    }

    ZF.dialog({
      title: 'Install package',
      content: el('div', null, [
        el('p', { text: one.name + ' will be installed from ' + path + '.' }),
        el('p', { class: 'subtle', text: 'Choose where it should be installed.' })
      ]),
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'internal', label: 'Internal storage', kind: 'primary', submit: true },
        { id: 'extended', label: 'Extended storage', submit: true }
      ],
      onAction: function (id) {
        if (id === 'internal') { run('internal', false); return true; }
        if (id === 'extended') { run('extended', false); return true; }
        return true;
      }
    });
  }

  /** Context / overflow menu for the selection. */
  function itemMenu(sel, overflow) {
    var write = canWrite();
    var one = sel.length === 1 ? sel[0] : null;
    var allFiles = true;
    for (var i = 0; i < sel.length; i++) if (sel[i].isDir) allFiles = false;
    var items = [];

    if (!overflow) {
      if (one) items.push({ label: one.isDir ? 'Open' : 'Open details', icon: one.isDir ? 'folder' : 'eye', hint: 'Enter', onclick: function () { openEntry(one); } });
      if (allFiles) items.push({ label: 'Download', icon: 'download', onclick: function () { download(sel); } });
      items.push({ label: 'Download as ZIP', icon: 'archive', onclick: function () { downloadZip(sel); } });
      if (write && one && !one.isDir && ZF.isArchive(one.name)) {
        items.push('-');
        items = items.concat(extractItems(one));
      }
      if (write && one && !one.isDir && ZF.features.pkgInstall && /\.(pkg|fpkg|ffpkg)$/i.test(one.name)) {
        items.push('-');
        items.push({ label: 'Install package', icon: 'package', onclick: function () { installPackage(one); } });
      }
      items.push('-');
      if (write) {
        items.push({ label: 'Copy to\u2026', icon: 'copy', onclick: function () { transferTo('copy', sel); } });
        items.push({ label: 'Move to\u2026', icon: 'move', onclick: function () { transferTo('move', sel); } });
        if (one) items.push({ label: 'Rename', icon: 'pencil', hint: 'F2', onclick: function () { rename(one); } });
        if (one) items.push({ label: 'Permissions\u2026', icon: 'shield', onclick: function () { permissions(one); } });
        items.push('-');
      }
    }
    if (write) {
      items.push({ label: 'Cut', icon: 'move', hint: 'Ctrl+X', onclick: function () { setClip('cut', sel); } });
      items.push({ label: 'Copy', icon: 'copy', hint: 'Ctrl+C', onclick: function () { setClip('copy', sel); } });
      items.push('-');
    }
    items.push({ label: sel.length === 1 ? 'Copy path' : 'Copy paths', icon: 'link', onclick: function () { copyPaths(sel); } });
    if (one && one.isDir) {
      var p = entryPath(one);
      items.push({ label: isBookmarked(p) ? 'Remove from Places' : 'Add to Places', icon: 'bookmark', onclick: function () { toggleBookmark(p); } });
    }
    if (one) items.push({ label: 'Details', icon: 'info', onclick: function () { details(one); } });
    if (overflow) {
      items.push('-');
      items.push({ label: 'Select all', hint: 'Ctrl+A', onclick: selectAll });
      items.push({ label: 'Invert selection', onclick: invertSelection });
    }
    if (write && !overflow) {
      items.push('-');
      items.push({ label: 'Delete', icon: 'trash', danger: true, hint: 'Del', onclick: function () { remove(sel); } });
    }
    return items;
  }

  function backgroundMenu() {
    var write = canWrite();
    var items = [];
    if (write) {
      items.push({ label: 'Upload files', icon: 'upload', onclick: function () { dom.upFiles.click(); } });
      items.push({ label: 'Upload folder', icon: 'folder', onclick: function () { dom.upFolder.click(); } });
      items.push('-');
      items.push({ label: 'New folder', icon: 'folder-plus', onclick: newFolder });
      items.push({ label: 'New file', icon: 'file-plus', onclick: newFile });
      if (S.clip) {
        items.push('-');
        items.push({ label: 'Paste ' + ZF.plural(S.clip.paths.length, 'item'), icon: 'copy', hint: 'Ctrl+V', onclick: paste });
      }
      items.push('-');
    }
    items.push({ label: 'Refresh', icon: 'refresh', onclick: function () { reload(false); } });
    if (S.view.length) items.push({ label: 'Select all', hint: 'Ctrl+A', onclick: selectAll });
    items.push('-');
    items = items.concat(folderItems());
    return items;
  }

  function folderItems() {
    var path = S.path;
    return [
      { label: isBookmarked(path) ? 'Remove folder from Places' : 'Add folder to Places', icon: 'bookmark', disabled: path === '/', onclick: function () { toggleBookmark(path); } },
      { label: 'Copy folder path', icon: 'link', onclick: function () {
        ZF.copyText(path).then(function () { ZF.toast('Path copied', { type: 'success', timeout: 2000 }); });
      } }
    ];
  }

  function moreMenu() {
    var items = [
      { label: 'Show hidden files', checked: !!ZF.settings.showHidden, onclick: function () { ZF.setSetting('showHidden', !ZF.settings.showHidden); } },
      { label: 'Show system folders', checked: !!ZF.settings.showProtected, onclick: function () { ZF.setSetting('showProtected', !ZF.settings.showProtected); } },
      '-',
      { label: 'Sort by name', checked: ZF.settings.sortKey === 'name', onclick: function () { setSort('name', true); } },
      { label: 'Sort by type', checked: ZF.settings.sortKey === 'type', onclick: function () { setSort('type', true); } },
      { label: 'Sort by size', checked: ZF.settings.sortKey === 'size', onclick: function () { setSort('size', true); } },
      '-'
    ];
    return items.concat(folderItems()).concat([
      { label: 'Refresh', icon: 'refresh', onclick: function () { reload(false); } }
    ]);
  }

  function setSort(key, keepDir) {
    if (!keepDir && ZF.settings.sortKey === key) ZF.setSetting('sortDir', ZF.settings.sortDir === -1 ? 1 : -1);
    else if (ZF.settings.sortKey !== key) { ZF.setSetting('sortKey', key); ZF.setSetting('sortDir', 1); }
    rebuild();
    renderRows();
  }

  function setFilter(v) {
    S.filter = v;
    if (dom.filter.value !== v) dom.filter.value = v;
    rebuild();
    renderRows();
  }

  /* ── Details dialog ─────────────────────────────────────────────────── */

  var TEXT_PREVIEW_MAX = 512 * 1024;
  var MEDIA_PREVIEW_MAX = 1024 * 1024 * 1024;

  function kvRow(dl, label, value) {
    dl.appendChild(el('dt', { text: label }));
    var dd = el('dd');
    ZF.append(dd, value);
    dl.appendChild(dd);
    return dd;
  }

  function details(e) {
    var path = entryPath(e);
    var url = api.fileUrl(path);
    var kind = e.ft.kind;
    var content = el('div');
    var preview = null;

    if (!e.isDir) {
      if (kind === 'image') {
        preview = el('div', { class: 'preview' }, el('img', { src: url, alt: e.name }));
      } else if ((kind === 'video' || kind === 'audio') && e.size <= MEDIA_PREVIEW_MAX) {
        preview = el('div', { class: 'preview' }, el(kind, { src: url, controls: true, preload: 'metadata' }));
      } else if (kind === 'text' || kind === 'code') {
        preview = el('div', { class: 'preview' });
        if (e.size > TEXT_PREVIEW_MAX) {
          preview.appendChild(el('div', { class: 'preview-msg', text: 'This file is too large to preview (' + ZF.bytes(e.size) + '). Download it to view the contents.' }));
        } else if (e.size === 0) {
          preview.appendChild(el('div', { class: 'preview-msg', text: 'This file is empty.' }));
        } else {
          preview.appendChild(el('div', { class: 'preview-msg', text: 'Loading preview\u2026' }));
          fetch(url, { credentials: 'same-origin' }).then(function (r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.text();
          }).then(function (t) {
            ZF.clear(preview).appendChild(el('pre', { text: t }));
          }, function (err) {
            ZF.clear(preview).appendChild(el('div', { class: 'preview-msg', text: 'Preview unavailable: ' + err.message }));
          });
        }
      } else if (kind === 'package') {
        preview = el('div', { class: 'preview' }, el('div', { class: 'preview-msg', text: 'Reading package\u2026' }));
      }
    }
    if (preview) content.appendChild(preview);

    var dl = el('dl', { class: 'kv' });
    kvRow(dl, 'Name', e.name);
    kvRow(dl, 'Type', e.ft.label);
    var sizeDd = kvRow(dl, 'Size', e.isDir ? null : ZF.bytes(e.size) + (e.size >= 1024 ? ' (' + ZF.number(e.size) + ' bytes)' : ''));
    kvRow(dl, 'Location', el('span', { class: 'mono', text: S.path }));
    content.appendChild(dl);

    if (e.isDir) {
      var calc = function () {
        ZF.clear(sizeDd).appendChild(el('span', { class: 'muted', text: 'Calculating\u2026' }));
        api.dirSize(path).then(function (r) {
          S.dirSizes[e.name] = r.size;
          ZF.clear(sizeDd).appendChild(document.createTextNode(ZF.bytes(r.size)));
          if (r.partial) sizeDd.appendChild(el('span', { class: 'muted', text: ' or more \u2014 the folder is too large to scan completely' }));
          var row = dom.rows.rows[S.index[e.name]];
          if (row) row.cells[3].textContent = ZF.bytes(r.size);
        }, function (err) {
          ZF.clear(sizeDd).appendChild(el('span', { class: 'text-danger', text: err.message }));
        });
      };
      if (S.dirSizes.hasOwnProperty(e.name)) sizeDd.appendChild(document.createTextNode(ZF.bytes(S.dirSizes[e.name])));
      else sizeDd.appendChild(el('button', { type: 'button', class: 'link-btn', text: 'Calculate', onclick: calc }));
    }

    if (kind === 'package') {
      api.packageMeta(path).then(function (m) {
        ZF.clear(preview);
        var info = el('div', { class: 'preview-game' }, [
          m.has_icon ? el('img', { src: api.packageIconUrl(path), alt: '' }) : null,
          el('div', null, [
            el('div', { class: 'preview-game-title', text: m.title_name || e.name }),
            el('div', { class: 'muted mono', text: [m.title_id, m.version ? 'v' + m.version : null].filter(Boolean).join(' \u00b7 ') })
          ])
        ]);
        preview.appendChild(info);
        if (m.content_id) kvRow(dl, 'Content ID', el('span', { class: 'mono', text: m.content_id }));
        if (m.category) kvRow(dl, 'Category', m.category);
      }, function () {
        ZF.clear(preview).appendChild(el('div', { class: 'preview-msg', text: 'Package information is not available.' }));
      });
    }

    var copyBtn = ZF.button({ icon: 'link', label: 'Copy path', kind: 'ghost' });
    copyBtn.addEventListener('click', function () { copyPaths([e]); });

    var actions = [];
    if (e.isDir) actions.push({ id: 'open', label: 'Open folder' });
    else actions.push({ id: 'download', label: 'Download' });
    actions.push({ id: 'close', label: 'Close', kind: 'primary', submit: true });

    ZF.dialog({
      title: e.name,
      size: preview ? 'lg' : 'md',
      content: content,
      footLeft: copyBtn,
      actions: actions,
      onOpen: function (d) { d.buttons.close.focus(); },
      onAction: function (id) {
        if (id === 'download') download([e]);
        if (id === 'open') setTimeout(function () { go(path); }, 0);
        return true;
      },
      onClose: function () {
        var media = ZF.$$('video, audio', content);
        for (var i = 0; i < media.length; i++) { try { media[i].pause(); media[i].removeAttribute('src'); media[i].load(); } catch (x) { /* ignore */ } }
      }
    });
  }

  /* ── Upload sources ─────────────────────────────────────────────────── */

  function uploadFromInput(input, useRelative) {
    var files = input.files;
    var items = [];
    for (var i = 0; i < files.length; i++) {
      var rel = useRelative && files[i].webkitRelativePath ? files[i].webkitRelativePath : files[i].name;
      items.push({ file: files[i], rel: rel });
    }
    input.value = '';
    if (items.length) ZF.jobs.upload(S.path, items);
  }

  function readAllEntries(reader) {
    return new Promise(function (resolve, reject) {
      var all = [];
      (function more() {
        reader.readEntries(function (batch) {
          if (!batch.length) { resolve(all); return; }
          for (var i = 0; i < batch.length; i++) all.push(batch[i]);
          more();
        }, reject);
      })();
    });
  }

  /** Walks dropped FileSystemEntry objects into { items, dirs }. */
  function walkEntries(entries) {
    var items = [];
    var dirs = [];
    function walk(entry, prefix) {
      if (entry.isFile) {
        return new Promise(function (resolve) {
          entry.file(function (f) { items.push({ file: f, rel: prefix + entry.name }); resolve(); }, function () { resolve(); });
        });
      }
      if (entry.isDirectory) {
        dirs.push(prefix + entry.name);
        return readAllEntries(entry.createReader()).then(function (children) {
          var p = Promise.resolve();
          children.forEach(function (c) { p = p.then(function () { return walk(c, prefix + entry.name + '/'); }); });
          return p;
        }, function () { /* unreadable folder */ });
      }
      return Promise.resolve();
    }
    var p = Promise.resolve();
    entries.forEach(function (en) { p = p.then(function () { return walk(en, ''); }); });
    return p.then(function () { return { items: items, dirs: dirs }; });
  }

  function hasFiles(e) {
    var t = e.dataTransfer && e.dataTransfer.types;
    if (!t) return false;
    for (var i = 0; i < t.length; i++) if (t[i] === 'Files') return true;
    return false;
  }

  var dragDepth = 0;
  dom.view.addEventListener('dragenter', function (e) {
    if (!hasFiles(e) || !canWrite() || S.error) return;
    e.preventDefault();
    dragDepth++;
    dom.dropPath.textContent = S.path;
    dom.drop.hidden = false;
  });
  dom.view.addEventListener('dragover', function (e) {
    if (!hasFiles(e) || !canWrite() || S.error) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  });
  dom.view.addEventListener('dragleave', function (e) {
    if (!hasFiles(e)) return;
    dragDepth = Math.max(0, dragDepth - 1);
    if (!dragDepth) dom.drop.hidden = true;
  });
  dom.view.addEventListener('drop', function (e) {
    if (!hasFiles(e) || !canWrite() || S.error) return;
    e.preventDefault();
    dragDepth = 0;
    dom.drop.hidden = true;
    var dest = S.path;
    var dt = e.dataTransfer;
    var entries = [];
    if (dt.items && dt.items.length && dt.items[0].webkitGetAsEntry) {
      for (var i = 0; i < dt.items.length; i++) {
        var en = dt.items[i].kind === 'file' ? dt.items[i].webkitGetAsEntry() : null;
        if (en) entries.push(en);
      }
    }
    if (entries.length) {
      walkEntries(entries).then(function (r) { ZF.jobs.upload(dest, r.items, r.dirs); });
    } else {
      var items = [];
      for (var j = 0; j < dt.files.length; j++) items.push({ file: dt.files[j], rel: dt.files[j].name });
      ZF.jobs.upload(dest, items);
    }
  });

  dom.upFiles.addEventListener('change', function () { uploadFromInput(dom.upFiles, false); });
  dom.upFolder.addEventListener('change', function () { uploadFromInput(dom.upFolder, true); });

  /* ── Path editing ───────────────────────────────────────────────────── */

  function editPath() {
    dom.pathInput.value = S.path || '/';
    dom.pathInput.hidden = false;
    dom.pathInput.focus();
    dom.pathInput.select();
  }
  function endEditPath(commit) {
    if (dom.pathInput.hidden) return;
    var v = dom.pathInput.value.trim();
    dom.pathInput.hidden = true;
    if (commit && v) {
      go(P.norm(v));
      dom.list.focus();
    }
  }

  dom.pathbar.addEventListener('click', function (e) {
    if (up(e.target, function (n) { return hasClass(n, 'crumb'); }, dom.pathbar)) return;
    if (e.target === dom.pathInput) return;
    editPath();
  });
  dom.pathInput.addEventListener('keydown', function (e) {
    if (e.key === 'Enter') { e.preventDefault(); endEditPath(true); }
    else if (e.key === 'Escape' || e.key === 'Esc') { e.preventDefault(); e.stopPropagation(); endEditPath(false); dom.list.focus(); }
  });
  dom.pathInput.addEventListener('blur', function () { endEditPath(false); });

  /* ── Toolbar ────────────────────────────────────────────────────────── */

  dom.back.addEventListener('click', function () { history.back(); });
  dom.forward.addEventListener('click', function () { history.forward(); });
  dom.up.addEventListener('click', function () {
    if (S.path && S.path !== '/') { go(P.parent(S.path)); dom.list.focus(); }
  });
  dom.more.addEventListener('click', function () { ZF.menu(moreMenu(), dom.more); });

  var applyFilter = ZF.debounce(function () { setFilter(dom.filter.value); }, 80);
  dom.filter.addEventListener('input', applyFilter);
  dom.filter.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' || e.key === 'Esc') {
      e.preventDefault();
      if (dom.filter.value) setFilter('');
      else dom.list.focus();
    } else if (e.key === 'ArrowDown' || e.key === 'Down' || e.key === 'Enter') {
      e.preventDefault();
      if (S.view.length) {
        dom.list.focus();
        setCursor(S.view[0].name, true);
        if (e.key === 'Enter' && S.view.length === 1) openEntry(S.view[0]);
      }
    }
  });

  var sortBtns = ZF.$$('.th-sort', dom.table);
  for (var sb = 0; sb < sortBtns.length; sb++) {
    sortBtns[sb].addEventListener('click', function () { setSort(this.getAttribute('data-sort')); });
  }

  dom.checkAll.addEventListener('click', function () {
    if (dom.checkAll.checked) selectAll(); else clearSelection();
  });

  /* ── Row interaction ────────────────────────────────────────────────── */

  function rowOf(target) {
    var tr = up(target, function (n) { return n.tagName === 'TR'; }, dom.rows);
    if (!tr || tr.parentNode !== dom.rows) return null;
    var i = parseInt(tr.getAttribute('data-i'), 10);
    return S.view[i] ? { tr: tr, entry: S.view[i] } : null;
  }

  dom.rows.addEventListener('click', function (e) {
    var r = rowOf(e.target);
    if (!r) return;
    var name = r.entry.name;
    var menuBtn = up(e.target, function (n) { return hasClass(n, 'row-menu'); }, r.tr);
    if (menuBtn) {
      if (!S.sel[name]) selectOnly(name);
      S.cursor = name;
      paintSelection();
      ZF.menu(itemMenu(selectedEntries()), menuBtn);
      return;
    }
    if (e.target.type === 'checkbox') {
      if (e.shiftKey && S.anchor) selectRange(S.anchor, name, true);
      else { if (e.target.checked) S.sel[name] = true; else delete S.sel[name]; S.anchor = name; }
      S.cursor = name;
      paintSelection();
      return;
    }
    var mod = e.ctrlKey || e.metaKey;
    if (hasClass(e.target, 'fname-text') && !mod && !e.shiftKey) {
      S.cursor = name;
      openEntry(r.entry);
      return;
    }
    if (e.shiftKey && S.anchor) selectRange(S.anchor, name, mod);
    else if (mod) { if (S.sel[name]) delete S.sel[name]; else S.sel[name] = true; S.anchor = name; }
    else selectOnly(name);
    S.cursor = name;
    paintSelection();
  });

  dom.rows.addEventListener('dblclick', function (e) {
    var r = rowOf(e.target);
    if (!r || e.target.type === 'checkbox' || up(e.target, function (n) { return hasClass(n, 'row-menu'); }, r.tr)) return;
    if (hasClass(e.target, 'fname-text')) return;
    openEntry(r.entry);
  });

  dom.rows.addEventListener('mousedown', function (e) {
    if (e.shiftKey) e.preventDefault();
  });

  dom.list.addEventListener('contextmenu', function (e) {
    if (S.error) return;
    e.preventDefault();
    var r = rowOf(e.target);
    var at = { x: e.clientX, y: e.clientY };
    if (r) {
      if (!S.sel[r.entry.name]) selectOnly(r.entry.name);
      S.cursor = r.entry.name;
      paintSelection();
      ZF.menu(itemMenu(selectedEntries()), at, { keyboard: false, returnFocus: dom.list });
    } else {
      if (selectedCount()) clearSelection();
      ZF.menu(backgroundMenu(), at, { keyboard: false, returnFocus: dom.list });
    }
  });

  dom.list.addEventListener('click', function (e) {
    if (rowOf(e.target) || up(e.target, function (n) { return n.tagName === 'THEAD' || hasClass(n, 'list-state-actions'); }, dom.list)) return;
    if (selectedCount()) clearSelection();
  });

  dom.list.addEventListener('focus', function () {
    if (!S.cursor && S.view.length) { S.cursor = S.view[0].name; paintSelection(); }
  });

  /* ── Keyboard ───────────────────────────────────────────────────────── */

  var typeBuf = '';
  var typeTimer = null;

  function moveCursor(delta, e) {
    if (!S.view.length) return;
    var i = S.cursor !== null && S.index.hasOwnProperty(S.cursor) ? S.index[S.cursor] : -1;
    var n;
    if (delta === 'home') n = 0;
    else if (delta === 'end') n = S.view.length - 1;
    else n = Math.max(0, Math.min(S.view.length - 1, (i < 0 ? (delta > 0 ? -1 : S.view.length) : i) + delta));
    var name = S.view[n].name;
    if (e.shiftKey) selectRange(S.anchor || S.cursor || name, name, false);
    else if (!(e.ctrlKey || e.metaKey)) selectOnly(name);
    if (!S.anchor) S.anchor = name;
    setCursor(name, true);
  }

  function pageSize() {
    var row = dom.rows.rows[0];
    return row ? Math.max(1, Math.floor(dom.list.clientHeight / row.offsetHeight) - 1) : 10;
  }

  dom.list.addEventListener('keydown', function (e) {
    if (e.target !== dom.list) return;
    var k = e.key;
    var mod = e.ctrlKey || e.metaKey;
    var sel = selectedEntries();
    var cur = S.cursor !== null && S.index.hasOwnProperty(S.cursor) ? S.view[S.index[S.cursor]] : null;
    var handled = true;

    if (k === 'ArrowDown' || k === 'Down') moveCursor(1, e);
    else if (k === 'ArrowUp' || k === 'Up') {
      if (e.altKey) { if (S.path !== '/') go(P.parent(S.path)); }
      else moveCursor(-1, e);
    }
    else if (k === 'PageDown') moveCursor(pageSize(), e);
    else if (k === 'PageUp') moveCursor(-pageSize(), e);
    else if (k === 'Home') moveCursor('home', e);
    else if (k === 'End') moveCursor('end', e);
    else if (k === 'Enter') { if (cur) openEntry(cur); }
    else if (k === ' ' || k === 'Spacebar') {
      if (cur) {
        if (S.sel[cur.name]) delete S.sel[cur.name]; else S.sel[cur.name] = true;
        S.anchor = cur.name;
        paintSelection();
      }
    }
    else if (k === 'Backspace') { if (S.path !== '/') go(P.parent(S.path)); }
    else if (k === 'Escape' || k === 'Esc') { if (selectedCount()) clearSelection(); else handled = false; }
    else if ((k === 'Delete' || k === 'Del') && canWrite()) { if (sel.length) remove(sel); }
    else if (k === 'F2' && canWrite()) { if (sel.length === 1) rename(sel[0]); else if (!sel.length && cur) rename(cur); }
    else if (mod && (k === 'a' || k === 'A')) selectAll();
    else if (mod && (k === 'c' || k === 'C') && canWrite()) { if (sel.length) setClip('copy', sel); }
    else if (mod && (k === 'x' || k === 'X') && canWrite()) { if (sel.length) setClip('cut', sel); }
    else if (mod && (k === 'v' || k === 'V') && canWrite()) paste();
    else if ((k === 'ContextMenu' || (e.shiftKey && k === 'F10')) && cur) {
      if (!S.sel[cur.name]) { selectOnly(cur.name); paintSelection(); }
      var row = dom.rows.rows[S.index[cur.name]];
      var rect = row.getBoundingClientRect();
      ZF.menu(itemMenu(selectedEntries()), { x: rect.left + 48, y: rect.bottom }, { returnFocus: dom.list });
    }
    else if (k.length === 1 && !mod && !e.altKey && /\S/.test(k)) {
      clearTimeout(typeTimer);
      typeBuf += k.toLowerCase();
      typeTimer = setTimeout(function () { typeBuf = ''; }, 700);
      var start = cur ? S.index[cur.name] + (typeBuf.length === 1 ? 1 : 0) : 0;
      for (var n = 0; n < S.view.length; n++) {
        var e2 = S.view[(start + n) % S.view.length];
        if (e2.name.toLowerCase().indexOf(typeBuf) === 0) { selectOnly(e2.name); setCursor(e2.name, true); break; }
      }
    }
    else handled = false;

    if (handled) e.preventDefault();
  });

  /* Shortcuts that work anywhere in the Files view. */
  document.addEventListener('keydown', function (e) {
    if (dom.view.hidden || ZF.dialogOpen() || ZF.menuOpen()) return;
    var mod = e.ctrlKey || e.metaKey;
    if ((mod && (e.key === 'l' || e.key === 'L')) || (e.altKey && (e.key === 'd' || e.key === 'D'))) {
      e.preventDefault();
      editPath();
      return;
    }
    if (ZF.isTyping(e.target)) return;
    if (e.key === '/' && !mod) {
      e.preventDefault();
      dom.filter.focus();
      dom.filter.select();
    } else if (e.target === document.body && (e.key === 'ArrowDown' || e.key === 'Down' || e.key === 'ArrowUp' || e.key === 'Up')) {
      e.preventDefault();
      dom.list.focus();
    } else if (e.altKey && (e.key === 'ArrowLeft' || e.key === 'Left')) {
      e.preventDefault();
      history.back();
    } else if (e.altKey && (e.key === 'ArrowRight' || e.key === 'Right')) {
      e.preventDefault();
      history.forward();
    }
  });

  /* ── Places (sidebar) ───────────────────────────────────────────────── */

  var placesEl = $('places');
  var builtins = [{ path: '/', label: 'Root', icon: 'drive' }];

  function placeNode(p, removable) {
    var node = el('div', {
      class: 'place' + (S.path === p.path ? ' is-current' : ''),
      role: 'link', tabindex: '0', title: p.path
    }, [
      ZF.icon(p.icon || 'folder'),
      el('span', { class: 'place-name', text: p.label })
    ]);
    if (S.path === p.path) node.setAttribute('aria-current', 'page');
    node.addEventListener('click', function () { go(p.path); ZF.emit('nav-close'); });
    node.addEventListener('keydown', function (e) {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); go(p.path); ZF.emit('nav-close'); }
    });
    if (removable) {
      node.appendChild(el('button', {
        type: 'button', class: 'place-remove', title: 'Remove from Places', 'aria-label': 'Remove ' + p.label + ' from Places',
        onclick: function (e) { e.stopPropagation(); toggleBookmark(p.path); }
      }, ZF.icon('x')));
    }
    return node;
  }

  function renderPlaces() {
    ZF.clear(placesEl);
    var inFiles = !dom.view.hidden;
    for (var i = 0; i < builtins.length; i++) {
      var b = builtins[i];
      var node = placeNode(b, false);
      if (!inFiles) node.className = 'place';
      placesEl.appendChild(node);
    }
    var bm = ZF.settings.bookmarks;
    for (var j = 0; j < bm.length; j++) {
      var n = placeNode({ path: bm[j], label: P.base(bm[j]), icon: 'bookmark' }, true);
      if (!inFiles) n.className = 'place';
      placesEl.appendChild(n);
    }
    if (!bm.length) {
      placesEl.appendChild(el('div', { class: 'places-hint', text: 'Pin folders here with \u201cAdd to Places\u201d.' }));
    }
  }

  function discoverPlaces() {
    Promise.all([
      api.list('/').then(function (r) { return r.entries || []; }, function () { return []; }),
      /* The daemon reports the volumes that are actually mounted: the console
       * creates /mnt/usbN even with nothing plugged in, so listing the
       * directory showed every slot as a drive. */
      api.mounts().then(function (r) { return (r && r.mounts) || []; }, function () { return []; })
    ]).then(function (res) {
      var root = {};
      for (var i = 0; i < res[0].length; i++) if (res[0][i].type === 'directory') root[res[0][i].name] = true;
      var out = [{ path: '/', label: 'Root', icon: 'drive' }];
      if (root.data) out.push({ path: '/data', label: 'data', icon: 'folder' });
      if (root.user) out.push({ path: '/user', label: 'user', icon: 'folder' });

      var mounts = res[1].slice().sort(function (a, b) { return ZF.naturalCompare(a.name, b.name); });
      for (var k = 0; k < mounts.length; k++) {
        var m = mounts[k] || {};
        if (!m.path) continue;
        /* Slot numbering is what users recognise on the console; the volume
         * label stays available from /api/mounts for other views. */
        out.push({
          path: m.path,
          label: (m.kind === 'ext' ? 'Extended storage ' : 'USB drive ') + String(m.name || '').replace(/^[a-z]+/, ''),
          icon: 'drive'
        });
      }
      builtins = out;
      renderPlaces();
    });
  }

  ZF.on('settings', function (key) {
    if (key === 'bookmarks' || key === '*') renderPlaces();
    if (key === 'showHidden' || key === 'showProtected' || key === '*') {
      if (S.path !== null) { rebuild(); renderRows(); }
    }
  });
  ZF.on('files-path', renderPlaces);
  ZF.on('view', renderPlaces);
  ZF.on('bookmark-renamed', function (r) {
    var list = ZF.settings.bookmarks.slice();
    var changed = false;
    for (var i = 0; i < list.length; i++) {
      if (P.within(list[i], r.from)) { list[i] = r.to + list[i].slice(r.from.length); changed = true; }
    }
    if (changed) ZF.setSetting('bookmarks', list);
  });

  ZF.on('fs-changed', function (info) {
    if (S.path === null || dom.view.hidden) return;
    var dirs = (info && info.dirs) || [];
    for (var i = 0; i < dirs.length; i++) {
      if (P.norm(dirs[i]) === S.path) { load(S.path, { silent: true }); return; }
    }
  });
  ZF.on('reconnected', function () {
    if (!dom.view.hidden && S.path !== null) load(S.path, { silent: !S.error });
    discoverPlaces();
  });

  /* ── View contract ──────────────────────────────────────────────────── */

  ZF.views.files = {
    title: 'Files',
    enter: function (params, meta) {
      var path = P.norm(params.path || '/');
      var revealTarget = pendingReveal && pendingReveal.dir === path ? pendingReveal : null;
      pendingReveal = null;
      var opts = { focus: meta && meta.userNav && !ZF.isTyping(document.activeElement) };
      if (revealTarget) { opts.focusName = revealTarget.name; opts.select = true; }
      if (path !== S.path || S.error) {
        load(path, opts);
      } else {
        opts.silent = true;
        load(path, opts);
      }
      renderPlaces();
    },
    leave: function () {
      endEditPath(false);
      dom.drop.hidden = true;
    }
  };

  ZF.files = {
    hashFor: hashFor,
    reveal: reveal,
    current: function () { return S.path; },
    reload: reload
  };

  renderPlaces();
  discoverPlaces();
})(ZF);
