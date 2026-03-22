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
      function close(val) {
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

})(ZFTPD);
