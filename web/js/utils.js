/* ══ UTILITIES ════════════════════════════════════════════════════════════
 * Path helpers, byte formatting, CSRF token, DOM helpers.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;

  /* ── DOM helpers ── */
  Z.$ = function (id) { return D.getElementById(id); };
  Z.E = encodeURIComponent;

  /* ── Feature flags ── */
  Z.features = Z.features || {
    pkgInstall: false
  };

  Z.featureEnabled = function (name) {
    return !!(Z.features && Z.features[name] === true);
  };

  /* ── CSRF ── */
  Z.csrf = function () {
    var m = D.querySelector('meta[name="csrf-token"]');
    return m ? m.content : '';
  };

  /* ── Path helpers ── */
  Z.norm = function (p) {
    if (!p || p[0] !== '/') return '/';
    if (p.length > 1 && p[p.length - 1] === '/') return p.slice(0, -1);
    return p;
  };

  Z.parent = function (p) {
    p = Z.norm(p);
    if (p === '/') return null;
    var i = p.lastIndexOf('/');
    return i <= 0 ? '/' : p.slice(0, i);
  };

  Z.join = function (base, name) {
    return base === '/' ? '/' + name : base + '/' + name;
  };

  Z.basename = function (p) {
    if (!p) return '';
    var parts = p.split('/');
    return parts[parts.length - 1] || '';
  };

  Z.extname = function (name) {
    if (!name) return '';
    var i = name.lastIndexOf('.');
    return i > 0 ? name.slice(i + 1).toLowerCase() : '';
  };

  Z.h = function (value) {
    return String(value == null ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  };

  /* ── Download helper (avoids full-page navigation) ── */
  Z.download = function (url) {
    var a = document.createElement('a');
    a.href = url;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  };

  /* ── Byte formatting ── */
  Z.bytes = function (b) {
    if (typeof b !== 'number' || b < 0) return '\u2014';
    if (b === 0) return '0 B';
    var u = ['B', 'KB', 'MB', 'GB', 'TB'];
    var i = Math.min(Math.floor(Math.log(b) / Math.log(1024)), 4);
    return (b / Math.pow(1024, i)).toFixed(i > 0 ? 1 : 0) + ' ' + u[i];
  };

  Z.bps = function (b) { return Z.bytes(b) + '/s'; };

  /* ── Time formatting ── */
  Z.duration = function (sec) {
    sec = Math.max(0, Math.floor(sec));
    if (sec >= 3600) return Math.floor(sec / 3600) + 'h ' + Math.floor((sec % 3600) / 60) + 'm';
    if (sec >= 60) return Math.floor(sec / 60) + 'm ' + (sec % 60) + 's';
    return sec + 's';
  };

  Z.relativeTime = function (timestamp) {
    if (!timestamp) return '\u2014';
    var now = Math.floor(Date.now() / 1000);
    var diff = now - timestamp;
    if (diff < 60) return 'just now';
    if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
    if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
    return Math.floor(diff / 86400) + 'd ago';
  };

  /* ── File type detection ── */
  Z.fileCategory = function (name, isDir) {
    if (isDir) return 'dir';
    var ext = Z.extname(name);
    var map = {
      dir: [],
      code: ['c', 'cpp', 'h', 'hpp', 'py', 'rs', 'go', 'java', 'rb', 'php', 'swift', 'kt'],
      web: ['html', 'htm', 'jsx', 'tsx', 'vue', 'svelte'],
      style: ['css', 'scss', 'sass', 'less'],
      script: ['js', 'ts', 'sh', 'bash', 'zsh', 'ps1', 'bat', 'cmd', 'lua'],
      data: ['json', 'xml', 'yaml', 'yml', 'toml', 'ini', 'cfg', 'conf', 'env'],
      doc: ['md', 'txt', 'pdf', 'doc', 'docx', 'rtf', 'odt', 'tex'],
      img: ['png', 'jpg', 'jpeg', 'gif', 'svg', 'webp', 'bmp', 'ico', 'tiff', 'heic'],
      video: ['mp4', 'mkv', 'avi', 'mov', 'webm', 'flv', 'wmv', 'm4v'],
      audio: ['mp3', 'wav', 'flac', 'aac', 'ogg', 'wma', 'm4a', 'opus'],
      arch: ['zip', 'tar', 'gz', 'bz2', 'xz', '7z', 'rar', 'zst', 'lz4'],
      bin: ['exe', 'dll', 'so', 'dylib', 'elf', 'bin', 'o', 'a'],
      db: ['db', 'sqlite', 'sqlite3', 'sql', 'mdb'],
      lock: ['pem', 'key', 'crt', 'cer', 'p12', 'pfx'],
      log: ['log', 'out', 'err'],
      sheet: ['csv', 'xls', 'xlsx', 'ods', 'tsv'],
      slide: ['ppt', 'pptx', 'odp'],
      game: ['exfat', 'pkg', 'fpkg', 'ffpkg']
    };
    for (var cat in map) {
      if (map.hasOwnProperty(cat)) {
        for (var i = 0; i < map[cat].length; i++) {
          if (map[cat][i] === ext) return cat;
        }
      }
    }
    return 'generic';
  };

  Z.fileKind = function (name, isDir) {
    if (isDir) return 'Folder';
    var cat = Z.fileCategory(name, false);
    var map = {
      code: 'Source code',
      web: 'Web document',
      style: 'Stylesheet',
      script: 'Script',
      data: 'Data file',
      doc: 'Document',
      img: 'Image',
      video: 'Video',
      audio: 'Audio',
      arch: 'Archive',
      bin: 'Binary',
      db: 'Database',
      lock: 'Certificate or key',
      log: 'Log file',
      sheet: 'Spreadsheet',
      slide: 'Presentation',
      game: 'Game package'
    };
    return map[cat] || 'File';
  };

  Z.mediaKind = function (name, isDir) {
    if (isDir) return 'folder';
    var ext = Z.extname(name);
    if (['png', 'jpg', 'jpeg', 'gif', 'svg', 'webp', 'bmp', 'ico'].indexOf(ext) >= 0) return 'image';
    if (['mp4', 'mkv', 'avi', 'mov', 'webm', 'm4v', 'flv'].indexOf(ext) >= 0) return 'video';
    if (['mp3', 'wav', 'flac', 'aac', 'ogg', 'm4a', 'opus'].indexOf(ext) >= 0) return 'audio';
    if (ext === 'pdf') return 'pdf';
    if (['txt', 'md', 'log', 'json', 'xml', 'yaml', 'yml', 'toml', 'ini', 'cfg', 'conf', 'env', 'csv',
         'c', 'cpp', 'h', 'hpp', 'py', 'rs', 'go', 'java', 'rb', 'php', 'swift', 'kt',
         'html', 'htm', 'css', 'scss', 'sass', 'less', 'js', 'ts', 'sh', 'bash', 'zsh', 'lua'].indexOf(ext) >= 0) {
      return 'text';
    }
    if (['pkg', 'fpkg', 'ffpkg', 'exfat'].indexOf(ext) >= 0) return 'game';
    if (['zip', 'tar', 'gz', 'bz2', 'xz', '7z', 'rar', 'zst', 'lz4'].indexOf(ext) >= 0) return 'archive';
    return 'file';
  };

  Z.isGamePackage = function (name) {
    var ext = Z.extname(name);
    return ext === 'pkg' || ext === 'fpkg' || ext === 'ffpkg';
  };

  Z.isPreviewable = function (name, isDir, size) {
    var kind = Z.mediaKind(name, isDir);
    if (kind === 'folder' || kind === 'image' || kind === 'video' || kind === 'audio' || kind === 'pdf') return true;
    if (kind === 'text') return !size || size <= 2 * 1024 * 1024;
    return false;
  };

  Z.copyText = function (text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      return navigator.clipboard.writeText(text);
    }
    return new Promise(function (resolve, reject) {
      var ta = D.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', 'readonly');
      ta.style.cssText = 'position:fixed;left:-9999px;top:-9999px;';
      D.body.appendChild(ta);
      ta.select();
      try {
        if (!D.execCommand('copy')) throw new Error('copy failed');
        D.body.removeChild(ta);
        resolve();
      } catch (e) {
        D.body.removeChild(ta);
        reject(e);
      }
    });
  };

  /* ── Toast notifications (closable) ── */
  Z.toast = function (msg, type) {
    var wrap = Z.$('toast-wrap');
    if (!wrap) return;
    var el = D.createElement('div');
    el.className = 'toast ' + (type || '');

    var txt = D.createElement('span');
    txt.textContent = msg;
    el.appendChild(txt);

    var closeBtn = D.createElement('span');
    closeBtn.className = 'toast-close';
    closeBtn.innerHTML = '&times;';
    closeBtn.onclick = function () { _removeToast(el); };
    el.appendChild(closeBtn);

    wrap.appendChild(el);
    var timer = setTimeout(function () { _removeToast(el); }, 5000);
    el._timer = timer;
  };

  function _removeToast(el) {
    if (!el || !el.parentNode) return;
    clearTimeout(el._timer);
    el.style.opacity = '0';
    el.style.transform = 'translateY(8px)';
    el.style.transition = 'all .2s ease';
    setTimeout(function () {
      if (el.parentNode) el.parentNode.removeChild(el);
    }, 200);
  }

  /* ── Debounce ── */
  Z.debounce = function (fn, ms) {
    var timer;
    return function () {
      var args = arguments;
      var ctx = this;
      clearTimeout(timer);
      timer = setTimeout(function () { fn.apply(ctx, args); }, ms);
    };
  };

  /*=========================================================================*
   * MODAL SYSTEM — replaces native prompt / confirm / alert
   *
   *   Z.modal.prompt(title, defaultVal)  → Promise<string|null>
   *   Z.modal.confirm(title, message)    → Promise<boolean>
   *   Z.modal.alert(title, message)      → Promise<void>
   *
   *   ┌──────────────────────────────────┐
   *   │  Title                    [✕]    │
   *   │  ──────────────────────────────  │
   *   │  (optional message)             │
   *   │  [ input field ]                │
   *   │       [ Cancel ]  [ OK ]        │
   *   └──────────────────────────────────┘
   *=========================================================================*/
  Z.modal = {};
  var _activeModal = null;

  /**
   * Show a generic HTML modal with a title and raw HTML content.
   * Returns the content container for further manipulation.
   */
  Z.modal.showHTML = function (title, htmlContent) {
    var m = _modalCreate(title);
    _activeModal = m;
    var body = D.createElement('div');
    body.id = 'zftpd-modal-content';
    body.style.cssText = 'padding:24px;overflow-y:auto;max-height:60vh;';
    body.innerHTML = htmlContent;
    m.card.appendChild(body);
    D.body.appendChild(m.overlay);
    return body;
  };

  Z.modal.close = function () {
    if (_activeModal && _activeModal.close) {
      _activeModal.close();
      _activeModal = null;
    }
  };

  /* Shared overlay + card builder */
  function _modalCreate(title) {
    var overlay = D.createElement('div');
    overlay.style.cssText = 'position:fixed;inset:0;z-index:2000;background:rgba(0,0,0,.6);' +
        'display:flex;align-items:center;justify-content:center;animation:fadeIn .12s ease;';

    var card = D.createElement('div');
    card.style.cssText = 'background:var(--sf);border:1px solid var(--bd2);border-radius:14px;' +
        'width:min(380px,88vw);box-shadow:0 24px 64px rgba(0,0,0,.6);overflow:hidden;';

    /* Header */
    var hdr = D.createElement('div');
    hdr.style.cssText = 'display:flex;align-items:center;padding:14px 18px;' +
        'border-bottom:1px solid var(--bd);gap:8px;';
    hdr.innerHTML = '<div style="flex:1;font-weight:700;font-size:13px;color:var(--tx);">' +
        title + '</div>' +
        '<button class="modal-close" style="background:none;border:none;color:var(--tx3);' +
        'font-size:18px;cursor:pointer;padding:0 4px;line-height:1;">&times;</button>';

    card.appendChild(hdr);
    overlay.appendChild(card);

    function close() { if (overlay.parentNode) D.body.removeChild(overlay); }
    hdr.querySelector('.modal-close').onclick = close;

    return { overlay: overlay, card: card, close: close };
  }

  /* Footer with action buttons */
  function _modalFooter(cancelLabel, okLabel, okDanger) {
    var ftr = D.createElement('div');
    ftr.style.cssText = 'padding:12px 18px;border-top:1px solid var(--bd);' +
        'display:flex;gap:8px;justify-content:flex-end;';

    if (cancelLabel) {
      var cancelBtn = D.createElement('button');
      cancelBtn.className = 'btn';
      cancelBtn.style.cssText = 'padding:7px 16px;font-size:12px;';
      cancelBtn.textContent = cancelLabel;
      ftr.appendChild(cancelBtn);
      ftr._cancelBtn = cancelBtn;
    }

    var okBtn = D.createElement('button');
    okBtn.className = 'btn';
    okBtn.style.cssText = 'padding:7px 18px;font-size:12px;font-weight:700;border-radius:8px;' +
        'cursor:pointer;border:none;color:#fff;background:' + (okDanger ? 'var(--er)' : 'var(--ac)') + ';';
    okBtn.textContent = okLabel || 'OK';
    ftr.appendChild(okBtn);
    ftr._okBtn = okBtn;

    return ftr;
  }

  /**
   * Prompt modal — text input with OK/Cancel
   * @param {string} title   - Header text
   * @param {string} defVal  - Default input value
   * @returns {Promise<string|null>}  Resolved with value or null if cancelled
   */
  Z.modal.prompt = function (title, defVal) {
    return new Promise(function (resolve) {
      var m = _modalCreate(title);

      var body = D.createElement('div');
      body.style.cssText = 'padding:14px 18px;';
      var input = D.createElement('input');
      input.type = 'text';
      input.value = defVal || '';
      input.style.cssText = 'width:100%;box-sizing:border-box;padding:9px 12px;font-size:13px;' +
          'border:1px solid var(--bd2);border-radius:8px;background:var(--sf2);' +
          'color:var(--tx);font-family:inherit;outline:none;';
      input.onfocus = function () { input.style.borderColor = 'var(--ac)'; };
      input.onblur = function () { input.style.borderColor = 'var(--bd2)'; };
      body.appendChild(input);
      m.card.appendChild(body);

      var ftr = _modalFooter('Cancel', 'OK', false);
      m.card.appendChild(ftr);

      function done(val) { m.close(); resolve(val); }
      ftr._cancelBtn.onclick = function () { done(null); };
      ftr._okBtn.onclick = function () { done(input.value); };
      m.overlay.addEventListener('click', function (e) { if (e.target === m.overlay) done(null); });
      input.addEventListener('keydown', function (e) {
        if (e.key === 'Enter') done(input.value);
        if (e.key === 'Escape') done(null);
      });

      D.body.appendChild(m.overlay);
      setTimeout(function () { input.focus(); input.select(); }, 50);
    });
  };

  /**
   * Confirm modal — message with OK/Cancel (OK can be danger-styled)
   * @param {string} title   - Header text
   * @param {string} message - Body message
   * @param {boolean} danger - If true, OK button is red
   * @returns {Promise<boolean>}
   */
  Z.modal.confirm = function (title, message, danger) {
    return new Promise(function (resolve) {
      var m = _modalCreate(title);

      if (message) {
        var body = D.createElement('div');
        body.style.cssText = 'padding:14px 18px;font-size:12px;color:var(--tx2);line-height:1.5;' +
            'white-space:pre-wrap;word-break:break-word;max-height:40vh;overflow-y:auto;';
        body.textContent = message;
        m.card.appendChild(body);
      }

      var ftr = _modalFooter('Cancel', 'Confirm', !!danger);
      m.card.appendChild(ftr);

      function done(val) { m.close(); resolve(val); }
      ftr._cancelBtn.onclick = function () { done(false); };
      ftr._okBtn.onclick = function () { done(true); };
      m.overlay.addEventListener('click', function (e) { if (e.target === m.overlay) done(false); });

      D.body.appendChild(m.overlay);
      setTimeout(function () { ftr._okBtn.focus(); }, 50);
    });
  };

  /**
   * Alert modal — informational message with single OK button
   * @param {string} title   - Header text
   * @param {string} message - Body message
   * @returns {Promise<void>}
   */
  Z.modal.alert = function (title, message) {
    return new Promise(function (resolve) {
      var m = _modalCreate(title);

      if (message) {
        var body = D.createElement('div');
        body.style.cssText = 'padding:14px 18px;font-size:12px;color:var(--tx2);line-height:1.5;' +
            'white-space:pre-wrap;word-break:break-word;max-height:40vh;overflow-y:auto;';
        body.textContent = message;
        m.card.appendChild(body);
      }

      var ftr = _modalFooter(null, 'OK', false);
      m.card.appendChild(ftr);

      function done() { m.close(); resolve(); }
      ftr._okBtn.onclick = done;
      m.overlay.addEventListener('click', function (e) { if (e.target === m.overlay) done(); });

      D.body.appendChild(m.overlay);
      setTimeout(function () { ftr._okBtn.focus(); }, 50);
    });
  };

  /* ═══════════════════════════════════════════════════════════════════════
   * FOLDER PICKER MODAL — browseable filesystem picker
   *
   *  Z.modal.folderPicker(title, startPath) → Promise<string|null>
   *
   *  ┌──────────────────────────────────────────────┐
   *  │  Choose destination              [✕]         │
   *  │  / > data > user                             │
   *  │  ┌──────────────────────────────┐            │
   *  │  │ 📁 folder_a                ›  │            │
   *  │  │ 📁 folder_b                ›  │            │
   *  │  └──────────────────────────────┘            │
   *  │  [↑ Back] [+ New Folder]  [ Select folder ] │
   *  └──────────────────────────────────────────────┘
   * ═══════════════════════════════════════════════════════════════════════ */
  Z.modal.folderPicker = function (title, startPath) {
    return new Promise(function (resolve) {
      var browsePath = startPath || '/';

      /* Build overlay */
      var overlay = D.createElement('div');
      overlay.style.cssText = 'position:fixed;inset:0;z-index:2000;background:rgba(0,0,0,.65);' +
          'display:flex;align-items:center;justify-content:center;animation:fadeIn .15s ease;';

      var card = D.createElement('div');
      card.style.cssText = 'background:var(--sf);border:1px solid var(--bd2);border-radius:14px;' +
          'width:min(460px,90vw);max-height:70vh;display:flex;flex-direction:column;' +
          'box-shadow:0 32px 80px rgba(0,0,0,.6);overflow:hidden;';

      /* ── Header ── */
      var hdr = D.createElement('div');
      hdr.style.cssText = 'display:flex;align-items:center;padding:14px 18px;' +
          'border-bottom:1px solid var(--bd);gap:8px;';
      hdr.innerHTML = '<div style="flex:1;font-weight:700;font-size:13px;color:var(--tx);">' +
          (title || 'Choose destination') + '</div>' +
          '<button class="fp-close" style="background:none;border:none;color:var(--tx3);' +
          'font-size:18px;cursor:pointer;padding:0 4px;line-height:1;">&times;</button>';

      /* ── Breadcrumb ── */
      var bcBar = D.createElement('div');
      bcBar.style.cssText = 'padding:10px 18px 4px;font-size:11px;color:var(--tx3);' +
          'font-family:monospace;white-space:nowrap;overflow-x:auto;';

      /* ── Body = folder list ── */
      var body = D.createElement('div');
      body.style.cssText = 'flex:1;overflow-y:auto;padding:8px 12px;min-height:120px;max-height:50vh;';

      /* ── Footer ── */
      var ftr = D.createElement('div');
      ftr.style.cssText = 'padding:12px 18px;border-top:1px solid var(--bd);display:flex;gap:8px;align-items:center;';
      ftr.innerHTML = '<button class="fp-up btn" style="padding:6px 10px;font-size:11px;">' +
          '&uarr; Back</button>' +
          '<button class="fp-mkdir btn" style="padding:6px 10px;font-size:11px;">' +
          '+ New Folder</button>' +
          '<div style="flex:1;"></div>' +
          '<button class="fp-ok btn" style="padding:8px 18px;font-size:12px;font-weight:700;' +
          'background:var(--ac);color:#fff;border:none;border-radius:8px;cursor:pointer;">' +
          'Select this folder</button>';

      card.appendChild(hdr);
      card.appendChild(bcBar);
      card.appendChild(body);
      card.appendChild(ftr);
      overlay.appendChild(card);
      D.body.appendChild(overlay);

      /* ── Close ── */
      var escClose = null;

      function close(val) {
        if (escClose) D.removeEventListener('keydown', escClose);
        if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
        resolve(val);
      }
      overlay.addEventListener('click', function (e) { if (e.target === overlay) close(null); });
      hdr.querySelector('.fp-close').onclick = function () { close(null); };

      /* ── Back (parent dir) ── */
      ftr.querySelector('.fp-up').onclick = function () {
        var parent = Z.parent(browsePath);
        if (parent !== null) { browsePath = parent; loadDir(browsePath); }
      };

      /* ── New Folder ── */
      ftr.querySelector('.fp-mkdir').onclick = function () {
        Z.modal.prompt('New Folder', '').then(function (name) {
          if (!name) return;
          Z.api.mkdir(browsePath, name).then(function () {
            Z.toast('Folder created', 'ok');
            loadDir(browsePath);
          }).catch(function (e) {
            Z.toast('Failed: ' + e.message, 'er');
          });
        });
      };

      /* ── Select this folder ── */
      ftr.querySelector('.fp-ok').onclick = function () { close(browsePath); };

      /* ── Render breadcrumb ── */
      function renderBreadcrumb(path) {
        var parts = path.split('/').filter(function (s) { return s.length > 0; });
        var html = '<span style="color:var(--ac);cursor:pointer;" data-fp="/">/</span>';
        var cum = '';
        for (var i = 0; i < parts.length; i++) {
          cum += '/' + parts[i];
          html += ' <span style="color:var(--tx3);">›</span> ' +
              '<span style="color:var(--ac);cursor:pointer;" data-fp="' + cum + '">' +
              parts[i] + '</span>';
        }
        bcBar.innerHTML = html;
        var spans = bcBar.querySelectorAll('span[data-fp]');
        for (var s = 0; s < spans.length; s++) {
          (function (sp) {
            sp.onclick = function () {
              browsePath = sp.getAttribute('data-fp');
              loadDir(browsePath);
            };
          })(spans[s]);
        }
      }

      /* ── Load directory ── */
      function loadDir(path) {
        renderBreadcrumb(path);
        body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--tx3);font-size:12px;">Loading\u2026</div>';
        Z.api.list(path).then(function (data) {
          body.innerHTML = '';
          if (!data || !data.entries || !data.entries.length) {
            body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--tx3);font-size:12px;">Empty directory</div>';
            return;
          }
          var dirs = data.entries.filter(function (e) { return e.type === 'directory'; });
          dirs.sort(function (a, b) { return a.name.localeCompare(b.name); });
          if (!dirs.length) {
            body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--tx3);font-size:12px;">No subdirectories</div>';
            return;
          }
          for (var i = 0; i < dirs.length; i++) {
            (function (entry) {
              var row = D.createElement('div');
              row.style.cssText = 'display:flex;align-items:center;gap:10px;padding:8px 12px;' +
                  'border-radius:8px;cursor:pointer;color:var(--tx2);transition:all .1s;font-size:12px;';
              row.innerHTML = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
                  'stroke-width="2" style="flex-shrink:0;color:var(--ac);"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 ' +
                  '1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>' +
                  '<span style="flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;">' +
                  entry.name + '</span>' +
                  '<span style="color:var(--tx3);font-size:10px;">›</span>';
              row.onmouseenter = function () { row.style.background = 'var(--sf2)'; row.style.color = 'var(--tx)'; };
              row.onmouseleave = function () { row.style.background = 'none'; row.style.color = 'var(--tx2)'; };
              row.onclick = function () {
                browsePath = Z.join(path, entry.name);
                loadDir(browsePath);
              };
              body.appendChild(row);
            })(dirs[i]);
          }
        }).catch(function (err) {
          body.innerHTML = '<div style="text-align:center;padding:24px;color:var(--er);font-size:12px;">' +
              'Error: ' + err.message + '</div>';
        });
      }

      loadDir(browsePath);
    });
  };

  /**
   * File picker modal — browse folders and select one matching file.
   * @param {string} title
   * @param {string} startPath
   * @param {{extensions?: string[], accept?: Function, hint?: string}} opts
   * @returns {Promise<string|null>}
   */
  Z.modal.filePicker = function (title, startPath, opts) {
    opts = opts || {};

    return new Promise(function (resolve) {
      var browsePath = Z.norm(startPath || '/');
      if (browsePath !== '/' && /\.[^\/]+$/.test(browsePath)) {
        browsePath = Z.parent(browsePath) || '/';
      }

      var extensions = opts.extensions || [];
      var hint = opts.hint || 'Browse folders and choose a file.';

      var overlay = D.createElement('div');
      overlay.style.cssText = 'position:fixed;inset:0;z-index:2000;background:rgba(0,0,0,.65);' +
          'display:flex;align-items:center;justify-content:center;animation:fadeIn .15s ease;';

      var card = D.createElement('div');
      card.style.cssText = 'background:var(--sf);border:1px solid var(--bd2);border-radius:14px;' +
          'width:min(560px,92vw);max-height:76vh;display:flex;flex-direction:column;' +
          'box-shadow:0 32px 80px rgba(0,0,0,.6);overflow:hidden;';

      var hdr = D.createElement('div');
      hdr.style.cssText = 'display:flex;align-items:center;padding:14px 18px;' +
          'border-bottom:1px solid var(--bd);gap:8px;';

      var hdrTitle = D.createElement('div');
      hdrTitle.style.cssText = 'flex:1;font-weight:700;font-size:13px;color:var(--tx);';
      hdrTitle.textContent = title || 'Choose file';

      var closeBtn = D.createElement('button');
      closeBtn.className = 'fp-close';
      closeBtn.style.cssText = 'background:none;border:none;color:var(--tx3);font-size:18px;' +
          'cursor:pointer;padding:0 4px;line-height:1;';
      closeBtn.innerHTML = '&times;';

      hdr.appendChild(hdrTitle);
      hdr.appendChild(closeBtn);

      var bcBar = D.createElement('div');
      bcBar.style.cssText = 'padding:10px 18px 4px;font-size:11px;color:var(--tx3);' +
          'font-family:monospace;white-space:nowrap;overflow-x:auto;';

      var hintBar = D.createElement('div');
      hintBar.style.cssText = 'padding:0 18px 8px;color:var(--tx3);font-size:11px;font-weight:600;';
      hintBar.textContent = hint;

      var body = D.createElement('div');
      body.className = 'fp-body';
      body.style.cssText = 'flex:1;overflow-y:auto;padding:8px 12px;min-height:180px;max-height:54vh;';

      var ftr = D.createElement('div');
      ftr.style.cssText = 'padding:12px 18px;border-top:1px solid var(--bd);display:flex;gap:8px;align-items:center;';

      var upBtn = D.createElement('button');
      upBtn.className = 'fp-up btn';
      upBtn.style.cssText = 'padding:6px 10px;font-size:11px;';
      upBtn.innerHTML = '&uarr; Back';

      var cancelBtn = D.createElement('button');
      cancelBtn.className = 'fp-cancel btn';
      cancelBtn.style.cssText = 'padding:7px 14px;font-size:12px;margin-left:auto;';
      cancelBtn.textContent = 'Cancel';

      ftr.appendChild(upBtn);
      ftr.appendChild(cancelBtn);

      card.appendChild(hdr);
      card.appendChild(bcBar);
      card.appendChild(hintBar);
      card.appendChild(body);
      card.appendChild(ftr);
      overlay.appendChild(card);
      D.body.appendChild(overlay);

      var escClose = null;

      function close(val) {
        if (escClose) D.removeEventListener('keydown', escClose);
        if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
        resolve(val);
      }

      function isDir(entry) {
        return entry && (entry.type === 'directory' || entry.type === 'dir' || entry.is_dir === true);
      }

      function acceptFile(entry) {
        if (!entry || isDir(entry)) return false;
        if (typeof opts.accept === 'function') return !!opts.accept(entry);
        if (!extensions.length) return true;
        var ext = Z.extname(entry.name || '');
        for (var i = 0; i < extensions.length; i++) {
          if (String(extensions[i]).toLowerCase() === ext) return true;
        }
        return false;
      }

      function renderMessage(text, kind) {
        body.innerHTML = '';
        var msg = D.createElement('div');
        msg.style.cssText = 'text-align:center;padding:28px 18px;color:' +
            (kind === 'error' ? 'var(--er)' : 'var(--tx3)') + ';font-size:12px;font-weight:700;';
        msg.textContent = text;
        body.appendChild(msg);
      }

      function renderBreadcrumb(path) {
        bcBar.innerHTML = '';
        var parts = path.split('/').filter(function (s) { return s.length > 0; });
        var cum = '';

        function addCrumb(label, value) {
          var crumb = D.createElement('span');
          crumb.style.cssText = 'color:var(--ac);cursor:pointer;';
          crumb.textContent = label;
          crumb.setAttribute('data-fp', value);
          crumb.onclick = function () {
            browsePath = value;
            loadDir(browsePath);
          };
          bcBar.appendChild(crumb);
        }

        addCrumb('/', '/');
        for (var i = 0; i < parts.length; i++) {
          cum += '/' + parts[i];
          var sep = D.createElement('span');
          sep.style.cssText = 'color:var(--tx3);';
          sep.textContent = ' \u203a ';
          bcBar.appendChild(sep);
          addCrumb(parts[i], cum);
        }
      }

      function icon(kind) {
        if (kind === 'dir') {
          return '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
              'stroke-width="2" style="flex-shrink:0;color:var(--ac);"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 ' +
              '1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>';
        }
        return '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
            'stroke-width="2" style="flex-shrink:0;color:var(--ac2);"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 ' +
            '0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/></svg>';
      }

      function makeRow(entry, directory) {
        var fullPath = Z.join(browsePath, entry.name);
        var row = D.createElement('div');
        row.className = 'fp-row ' + (directory ? 'fp-dir' : 'fp-file');
        row.setAttribute('data-path', fullPath);
        row.style.cssText = 'display:flex;align-items:center;gap:10px;padding:9px 12px;' +
            'border-radius:8px;cursor:pointer;color:var(--tx2);transition:all .1s;font-size:12px;';

        var ico = D.createElement('span');
        ico.innerHTML = icon(directory ? 'dir' : 'file');

        var name = D.createElement('span');
        name.style.cssText = 'flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:700;';
        name.textContent = entry.name || fullPath;

        var meta = D.createElement('span');
        meta.style.cssText = 'color:var(--tx3);font-size:10px;font-weight:700;white-space:nowrap;';
        meta.textContent = directory ? '\u203a' : (typeof entry.size === 'number' ? Z.bytes(entry.size) : 'PKG');

        row.appendChild(ico);
        row.appendChild(name);
        row.appendChild(meta);

        row.onmouseenter = function () { row.style.background = 'var(--sf2)'; row.style.color = 'var(--tx)'; };
        row.onmouseleave = function () { row.style.background = 'none'; row.style.color = 'var(--tx2)'; };
        row.onclick = function () {
          if (directory) {
            browsePath = fullPath;
            loadDir(browsePath);
          } else {
            close(fullPath);
          }
        };

        return row;
      }

      function loadDir(path) {
        browsePath = Z.norm(path);
        renderBreadcrumb(browsePath);
        renderMessage('Loading\u2026');

        Z.api.list(browsePath).then(function (data) {
          body.innerHTML = '';
          var entries = data && data.entries ? data.entries : [];
          var dirs = [];
          var files = [];

          for (var i = 0; i < entries.length; i++) {
            var entry = entries[i] || {};
            if (isDir(entry)) dirs.push(entry);
            else if (acceptFile(entry)) files.push(entry);
          }

          dirs.sort(function (a, b) { return String(a.name || '').localeCompare(String(b.name || '')); });
          files.sort(function (a, b) { return String(a.name || '').localeCompare(String(b.name || '')); });

          for (var d = 0; d < dirs.length; d++) body.appendChild(makeRow(dirs[d], true));
          for (var f = 0; f < files.length; f++) body.appendChild(makeRow(files[f], false));

          if (!dirs.length && !files.length) {
            renderMessage(extensions.length ? 'No matching package files in this folder' : 'No selectable files in this folder');
          }
        }).catch(function (err) {
          renderMessage('Error: ' + (err && err.message ? err.message : 'failed to load directory'), 'error');
        });
      }

      closeBtn.onclick = function () { close(null); };
      cancelBtn.onclick = function () { close(null); };
      overlay.addEventListener('click', function (e) { if (e.target === overlay) close(null); });
      escClose = function (e) {
        if (e.key === 'Escape' && overlay.parentNode) {
          close(null);
        }
      };
      D.addEventListener('keydown', escClose);

      upBtn.onclick = function () {
        var parent = Z.parent(browsePath);
        if (parent !== null) loadDir(parent);
      };

      loadDir(browsePath);
    });
  };

})(ZFTPD);
