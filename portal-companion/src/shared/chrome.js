// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// THE SHARED KIT - behaviour half.
//
// One source, generated into both device documents by sync_chrome.mjs;
// verify_chrome.mjs fails if either copy drifts. See DEVICE-SURFACES-HANDOFF.md
// decision 4.
//
// Deliberately a CLASSIC script, not a module: the portal is a hand-authored
// PROGMEM literal with a plain <script>, and the companion's esbuild bundle can
// reference a global just fine. Everything hangs off CFK so the two documents'
// own top-level names cannot collide with it.
//
// The nav is built in JS rather than written as markup twice - that is what
// makes "byte-identical in both documents" mechanically true instead of a
// promise someone has to keep.

var CFK = (function () {

  // ── The five destinations, in the one order both documents use. ──────────
  //
  // `doc` is the document that RENDERS the destination. When it matches the
  // host document the item gets a local handler; otherwise it is a plain <a>
  // with no external-link marker and no transition copy, because crossing is
  // just a page load on the device's own network and the user is not supposed
  // to be able to tell.
  //
  // Settings is the one that spans both documents (Network here, Transcription
  // and Your data on the companion). It is listed as `portal` because the tab
  // always lands on Network - it renders where most entry points are and it
  // answers "am I connected", which is the top settings question. Segments then
  // link across.
  var DEST = [
    { id: 'media',    label: 'Media',    doc: 'portal',    href: '/#media' },
    { id: 'notes',    label: 'Notes',    doc: 'companion', href: '/web/#notes' },
    { id: 'listen',   label: 'Listen',   doc: 'companion', href: '/web/#listen' },
    { id: 'files',    label: 'Files',    doc: 'portal',    href: '/#files' },
    { id: 'settings', label: 'Settings', doc: 'portal',    href: '/#settings' },
  ];

  var host = 'portal';
  var active = 'media';
  var onLocal = null;

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  function icon(name) { return el('span', 'k-px k-px-' + name); }

  function item(d, cls) {
    var a = el('a', cls);
    a.href = d.href;
    a.dataset.dest = d.id;
    if (d.doc === host) {
      a.addEventListener('click', function (ev) {
        ev.preventDefault();
        if (onLocal) onLocal(d.id);
      });
    }
    return a;
  }

  // ── Build the chrome. Both documents call this once, on load. ────────────
  // `hostDoc` is 'portal' or 'companion'; `localHandler` is called with a
  // destination id when the user picks one this document renders itself.
  function nav(hostDoc, activeId, localHandler) {
    host = hostDoc;
    onLocal = localHandler;

    var mast = el('div', 'k-mast');
    var brand = el('div', 'k-logo', 'CYBER FIDGET');
    mast.appendChild(brand);
    mast.appendChild(el('span', 'k-sp'));
    mast.appendChild(chip('mastDevice', 'k-chip gn', 'device'));
    mast.appendChild(chip('mastNet', 'k-chip cy', 'network'));

    var sb = el('aside', 'k-sb');
    var sbBrand = el('div', 'k-sb-brand');
    sbBrand.appendChild(el('div', 'k-logo', 'CYBER FIDGET'));
    sbBrand.appendChild(el('div', 'k-logo-sub', 'Your Fidget'));
    sb.appendChild(sbBrand);
    var sbNav = el('nav', 'k-sb-nav');
    var tabs = el('nav', 'k-tabs');

    DEST.forEach(function (d) {
      var nv = item(d, 'k-nv');
      nv.appendChild(icon(d.id));
      nv.appendChild(document.createTextNode(d.label));
      sbNav.appendChild(nv);

      var tab = item(d, 'k-tab');
      tab.appendChild(icon(d.id));
      tab.appendChild(el('span', 'k-lb', d.label));
      tabs.appendChild(tab);
    });

    sb.appendChild(sbNav);
    sb.appendChild(el('div', 'k-sb-foot'));

    document.body.insertBefore(sb, document.body.firstChild);
    document.body.insertBefore(mast, document.body.firstChild);
    document.body.appendChild(tabs);
    setActive(activeId);
  }

  function chip(id, cls, text) {
    var c = el('span', cls);
    c.id = id;
    c.appendChild(el('span', 'k-dot' + (cls.indexOf('cy') >= 0 ? ' cy' : '')));
    c.appendChild(el('span', 'k-lbl', text));
    return c;
  }

  function setActive(id) {
    active = id;
    var all = document.querySelectorAll('.k-nv,.k-tab');
    for (var i = 0; i < all.length; i++) {
      all[i].classList.toggle('on', all[i].dataset.dest === id);
    }
  }

  // ── Connection facts: the sidebar footer on desktop, the masthead chips on
  //    a phone. Replaces the portal's old separate connection bar and the
  //    companion's own header, so both surfaces state the same things. ──────
  //    s: { apIp, ssid, staIp, version }
  function conn(s) {
    var foot = document.querySelector('.k-sb-foot');
    if (foot) {
      foot.innerHTML = '';
      foot.appendChild(kv('k-dot', 'Its own network - ' + (s.apIp || '192.168.4.1')));
      foot.appendChild(kv('k-dot cy', s.ssid ? 'Home WiFi - ' + s.ssid : 'No home WiFi'));
      if (!s.ssid) foot.lastChild.firstChild.className = 'k-dot off';
      if (s.version) {
        var v = el('div', 'k-kv k-ver', 'version ' + s.version);
        foot.appendChild(v);
      }
    }
    var dev = document.getElementById('mastDevice');
    var net = document.getElementById('mastNet');
    if (dev) dev.lastChild.textContent = 'device';
    if (net) {
      net.lastChild.textContent = s.ssid || 'its own network';
      net.firstChild.className = s.ssid ? 'k-dot cy' : 'k-dot off';
    }
  }

  function kv(dotCls, text) {
    var row = el('div', 'k-kv');
    row.appendChild(el('span', dotCls));
    row.appendChild(el('span', null, text));
    return row;
  }

  // ── Signal strength as four CSS bars. The portal's WiFi list used a pair of
  //    lock emoji and a raw dBm number; emoji render in fixed multi-colour and
  //    cannot be tinted, which is the defect class the brief called out. ─────
  function sig(rssi) {
    var n = rssi > -50 ? 4 : rssi > -60 ? 3 : rssi > -70 ? 2 : 1;
    return '<span class="k-sig s' + n + '"><i></i><i></i><i></i><i></i></span>';
  }

  // ── Gate panel (register decision 8). One treatment, reused wherever
  //    something needs a part that is not installed. Yellow means notice:
  //    the device is working exactly as shipped. ─────────────────────────────
  //    o: { lead, body, action, href }
  function gate(o) {
    var g = el('div', 'k-gate');
    g.appendChild(el('div', 'k-g1', o.lead));
    g.appendChild(el('div', 'k-g2', o.body));
    var b = el('a', 'k-go', o.action || 'Set it up >');
    b.href = o.href;
    g.appendChild(b);
    return g;
  }

  // ── Formatting shared by both surfaces' lists and players ────────────────
  function fmtBytes(b) {
    if (b < 1024) return b + ' B';
    if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
    return (b / 1048576).toFixed(1) + ' MB';
  }
  function fmtTime(s) {
    if (isNaN(s) || s < 0) return '0:00';
    s = Math.floor(s);
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  }
  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
      .replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }

  // ── The audio player. Replaces every native <audio controls> on both
  //    surfaces. `size` is 'bar' (76px, desktop media), 'mini' (52px, phone,
  //    sits above the tab bar) or 'inline' (28px controls in an expanded row).
  //    Returns a handle whose sync() the caller drives from timeupdate.
  //
  //    Only the main action carries the fill, the chamfer and the bevel; the
  //    one state change between playing and paused is that glyph.
  function player(mount, audio, size, opts) {
    opts = opts || {};
    var full = size !== 'inline';
    mount.className = full ? 'k-player' : 'k-prow';
    mount.innerHTML = '';

    var main = el('button', full ? 'k-pbtn main' : 'k-pbtn sm');
    main.type = 'button';
    var glyph = el('span', 'k-tri-r' + (full ? '' : ' sm'));
    main.appendChild(glyph);
    main.addEventListener('click', function () {
      if (audio.paused) audio.play(); else audio.pause();
    });

    var prev = null, next = null;
    if (size === 'bar' && opts.onPrev) {
      prev = el('button', 'k-pbtn');
      prev.type = 'button';
      var pg = el('span', 'k-skip');
      pg.appendChild(el('span', 'k-bar'));
      pg.appendChild(el('i', 'k-tri-l'));
      prev.appendChild(pg);
      prev.addEventListener('click', opts.onPrev);
      mount.appendChild(prev);
    }
    mount.appendChild(main);
    if (size === 'bar' && opts.onNext) {
      next = el('button', 'k-pbtn');
      next.type = 'button';
      var ng = el('span', 'k-skip');
      ng.appendChild(el('i', 'k-tri-r'));
      ng.appendChild(el('span', 'k-bar'));
      next.appendChild(ng);
      next.addEventListener('click', opts.onNext);
      mount.appendChild(next);
    }

    var info = null, title = null, artist = null;
    if (full) {
      info = el('div', null);
      info.style.cssText = 'min-width:0;flex:1';
      title = el('div', 'k-ptit', '-');
      artist = el('div', 'k-part', '');
      info.appendChild(title);
      info.appendChild(artist);
      mount.appendChild(info);
    }

    var cur = el('span', 'k-time', '0:00');
    var dur = el('span', 'k-time', '0:00');
    var seek = el('div', 'k-seek' + (full ? '' : ' thin'));
    var tr = el('div', 'k-tr');
    var fl = el('div', 'k-fl');
    var th = el('div', 'k-th');
    tr.appendChild(fl); tr.appendChild(th); seek.appendChild(tr);

    // The mini player is 52px of phone width with a tab bar under it - a seek
    // bar there would be a 4px target between two other targets, so it shows
    // the timecode pair only and the bar/inline sizes carry the scrub.
    if (size === 'mini') {
      var t = el('span', 'k-time', '0:00 / 0:00');
      mount.appendChild(t);
      cur = t; dur = null;
    } else {
      mount.appendChild(cur);
      mount.appendChild(seek);
      mount.appendChild(dur);
    }

    function seekTo(ev) {
      if (!audio.duration) return;
      var r = tr.getBoundingClientRect();
      var x = ((ev.touches ? ev.touches[0].clientX : ev.clientX) - r.left) / r.width;
      audio.currentTime = Math.max(0, Math.min(1, x)) * audio.duration;
    }
    seek.addEventListener('click', seekTo);

    function setGlyph() {
      glyph.className = audio.paused
        ? 'k-tri-r' + (full ? '' : ' sm')
        : 'k-pause' + (full ? '' : ' sm');
    }
    function sync() {
      setGlyph();
      var d = audio.duration || 0;
      var p = d ? (audio.currentTime / d) * 100 : 0;
      fl.style.width = p + '%';
      th.style.left = p + '%';
      if (dur) {
        cur.textContent = fmtTime(audio.currentTime);
        dur.textContent = fmtTime(d);
      } else {
        cur.textContent = fmtTime(audio.currentTime) + ' / ' + fmtTime(d);
      }
    }
    audio.addEventListener('timeupdate', sync);
    audio.addEventListener('play', setGlyph);
    audio.addEventListener('pause', setGlyph);
    audio.addEventListener('loadedmetadata', sync);
    sync();

    return {
      el: mount,
      sync: sync,
      show: function (on) { mount.classList.toggle('on', on !== false); },
      setTrack: function (t, a) {
        if (title) title.textContent = t || '-';
        if (artist) artist.textContent = a && a !== '-' ? a : '';
      },
    };
  }

  // ── Bulk download. Lifted from the portal, where it grew for Voice notes and
  //    the Files browser; the Notes merge moved one of those callers to the
  //    other document, so it lives here now and both call the same code.
  //
  //    The device stays a dumb one-file-at-a-time server: all bundling happens
  //    in the browser, transfers are strictly sequential (the async server
  //    wedges on concurrent reads), and a beforeunload guard keeps the user on
  //    the page while bytes are moving. iOS cannot do multi-file downloads, so
  //    there it is the single .zip or nothing.
  var dlItems = [], dlZipName = 'files.zip', dlTotal = 0;
  var dlAbort = null, dlActive = false, dlStartMs = 0, dlBox = null;

  var DL_CAP = { ios: 100 * 1048576, android: 200 * 1048576, desktop: 500 * 1048576 };

  function dlDeviceClass() {
    var ua = navigator.userAgent || '';
    if (/iP(hone|ad|od)/.test(ua) ||
        (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1)) return 'ios';
    if (/Android/.test(ua)) return 'android';
    return 'desktop';
  }

  // Built on first use so a surface that never bulk-downloads pays no markup.
  function dlEnsure() {
    if (dlBox) return dlBox;
    var o = el('div', 'k-dl');
    o.innerHTML =
      '<div class="k-dl-box">' +
      '<div class="k-dl-choice">' +
      '<h3 class="k-dl-title">Download files</h3>' +
      '<div class="k-dl-size"></div>' +
      '<div class="k-dl-btns">' +
      '<button class="k-dl-b pri k-dl-zip">Bundle as one .zip</button>' +
      '<button class="k-dl-b k-dl-indiv">Individual files</button>' +
      '</div><button class="k-dl-b k-dl-cancel">Cancel</button></div>' +
      '<div class="k-dl-prog" style="display:none">' +
      '<h3 class="k-dl-ptitle">Downloading...</h3>' +
      '<div class="k-dl-bar"><div class="k-dl-fill"></div></div>' +
      '<div class="k-dl-stat">0 B / 0 B</div>' +
      '<div class="k-dl-rate"></div><div class="k-dl-file"></div>' +
      '<div class="k-dl-warn">Keep this page open until it finishes.</div>' +
      '<button class="k-dl-b k-dl-stop">Cancel</button></div></div>';
    document.body.appendChild(o);
    dlBox = {
      root: o,
      q: function (c) { return o.querySelector('.' + c); },
    };
    dlBox.q('k-dl-zip').addEventListener('click', function () { dlStart('zip'); });
    dlBox.q('k-dl-indiv').addEventListener('click', function () { dlStart('individual'); });
    dlBox.q('k-dl-cancel').addEventListener('click', dlClose);
    dlBox.q('k-dl-stop').addEventListener('click', function () { if (dlAbort) dlAbort.abort(); });
    return dlBox;
  }

  var crcTab = null;
  function crc32Update(crc, buf) {
    if (!crcTab) {
      crcTab = new Uint32Array(256);
      for (var n = 0; n < 256; n++) {
        var c = n;
        for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        crcTab[n] = c >>> 0;
      }
    }
    for (var i = 0; i < buf.length; i++) crc = (crcTab[(crc ^ buf[i]) & 0xFF] ^ (crc >>> 8)) >>> 0;
    return crc >>> 0;
  }
  function zipLocal(nameBytes, crc, size) {
    var h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true);
    h.setUint16(8, 0, true); h.setUint16(10, 0, true); h.setUint16(12, 0x21, true);
    h.setUint32(14, crc, true); h.setUint32(18, size, true); h.setUint32(22, size, true);
    h.setUint16(26, nameBytes.length, true); h.setUint16(28, 0, true);
    return new Uint8Array(h.buffer);
  }
  function zipCentral(nameBytes, crc, size, offset) {
    var h = new DataView(new ArrayBuffer(46));
    h.setUint32(0, 0x02014b50, true); h.setUint16(4, 20, true); h.setUint16(6, 20, true);
    h.setUint16(8, 0x0800, true); h.setUint16(10, 0, true); h.setUint16(12, 0, true);
    h.setUint16(14, 0x21, true); h.setUint32(16, crc, true); h.setUint32(20, size, true);
    h.setUint32(24, size, true); h.setUint16(28, nameBytes.length, true);
    h.setUint16(30, 0, true); h.setUint16(32, 0, true); h.setUint16(34, 0, true);
    h.setUint16(36, 0, true); h.setUint32(38, 0, true); h.setUint32(42, offset, true);
    return new Uint8Array(h.buffer);
  }
  function zipEOCD(count, cdSize, cdOffset) {
    var h = new DataView(new ArrayBuffer(22));
    h.setUint32(0, 0x06054b50, true); h.setUint16(4, 0, true); h.setUint16(6, 0, true);
    h.setUint16(8, count, true); h.setUint16(10, count, true); h.setUint32(12, cdSize, true);
    h.setUint32(16, cdOffset, true); h.setUint16(20, 0, true);
    return new Uint8Array(h.buffer);
  }

  async function dlFetchBytes(item, baseReceived) {
    var resp = await fetch(item.url, { signal: dlAbort.signal });
    if (!resp.ok) throw new Error('HTTP ' + resp.status);
    var reader = resp.body.getReader();
    var chunks = [], size = 0, crc = 0xFFFFFFFF;
    for (;;) {
      var r = await reader.read();
      if (r.done) break;
      chunks.push(r.value); size += r.value.length; crc = crc32Update(crc, r.value);
      dlSetProgress(baseReceived + size);
    }
    var data = new Uint8Array(size), p = 0;
    for (var i = 0; i < chunks.length; i++) { data.set(chunks[i], p); p += chunks[i].length; }
    return { data: data, crc: (crc ^ 0xFFFFFFFF) >>> 0 };
  }

  async function dlZip() {
    var enc = new TextEncoder();
    var parts = [], central = [], offset = 0, received = 0;
    for (var i = 0; i < dlItems.length; i++) {
      dlSetFile(i, dlItems[i].name);
      var nameBytes = enc.encode(dlItems[i].name);
      var got = await dlFetchBytes(dlItems[i], received);
      received += got.data.length;
      var lh = zipLocal(nameBytes, got.crc, got.data.length);
      parts.push(lh, nameBytes, got.data);
      central.push({ nameBytes: nameBytes, crc: got.crc, size: got.data.length, offset: offset });
      offset += lh.length + nameBytes.length + got.data.length;
    }
    var cdSize = 0;
    for (var j = 0; j < central.length; j++) {
      var e = central[j];
      var ch = zipCentral(e.nameBytes, e.crc, e.size, e.offset);
      parts.push(ch, e.nameBytes);
      cdSize += ch.length + e.nameBytes.length;
    }
    parts.push(zipEOCD(central.length, cdSize, offset));
    dlSave(new Blob(parts, { type: 'application/zip' }), dlZipName);
  }

  async function dlIndividual() {
    var received = 0;
    for (var i = 0; i < dlItems.length; i++) {
      var it = dlItems[i];
      dlSetFile(i, it.name);
      var got = await dlFetchBytes(it, received);
      received += got.data.length;
      // Typed so iOS/Safari names it correctly rather than .txt.
      dlSave(new Blob([got.data], { type: it.type || 'application/octet-stream' }), it.name);
      await new Promise(function (r) { setTimeout(r, 250); });   // let the server release the socket
    }
  }

  function dlSave(blob, filename) {
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url; a.download = filename;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 4000);
  }

  function dlFmtEta(s) {
    return s >= 60 ? (Math.floor(s / 60) + 'm ' + String(s % 60).padStart(2, '0') + 's') : (s + 's');
  }
  function dlSetProgress(received) {
    var b = dlEnsure();
    var pct = dlTotal > 0 ? Math.min(100, Math.round((received / dlTotal) * 100)) : 0;
    b.q('k-dl-fill').style.width = pct + '%';
    b.q('k-dl-stat').textContent = fmtBytes(received) + ' / ' + fmtBytes(dlTotal) + ' (' + pct + '%)';
    var elapsed = (Date.now() - dlStartMs) / 1000;
    if (elapsed > 0.4 && received > 0) {
      var rate = received / elapsed;
      var eta = rate > 0 ? Math.round(Math.max(0, dlTotal - received) / rate) : 0;
      b.q('k-dl-rate').textContent = fmtBytes(rate) + '/s' +
        (received < dlTotal ? '  -  ~' + dlFmtEta(eta) + ' left' : '');
    }
  }
  function dlSetFile(i, name) {
    dlEnsure().q('k-dl-file').textContent =
      'File ' + (i + 1) + ' of ' + dlItems.length + ': ' + name;
  }

  // Open the chooser. items: [{url,name,bytes,type}]; label is the plural noun
  // shown in the title ("voice notes" / "files").
  function dlBegin(items, zipName, label) {
    if (!items.length) return;
    var b = dlEnsure();
    dlItems = items; dlZipName = zipName;
    dlTotal = items.reduce(function (s, it) { return s + (it.bytes || 0); }, 0);
    var dev = dlDeviceClass(), cap = DL_CAP[dev] || DL_CAP.desktop;
    var over = dlTotal > cap;
    b.q('k-dl-title').textContent = 'Download ' + items.length + ' ' + label;
    b.q('k-dl-size').textContent = 'Total: ' + fmtBytes(dlTotal) +
      (over ? '  (too large to bundle on this device, ~' + fmtBytes(cap) + ' max)' : '');
    b.q('k-dl-zip').disabled = over;
    b.q('k-dl-indiv').style.display = dev === 'ios' ? 'none' : '';
    if (dev === 'ios' && over) {
      b.q('k-dl-size').textContent = 'Total: ' + fmtBytes(dlTotal) +
        ' is too large to bundle here. Download files one at a time using the ' +
        'Download button on each one.';
    }
    b.q('k-dl-choice').style.display = '';
    b.q('k-dl-prog').style.display = 'none';
    b.root.classList.add('on');
  }

  var dlToast = function () {};
  async function dlStart(mode) {
    var b = dlEnsure();
    b.q('k-dl-choice').style.display = 'none';
    b.q('k-dl-prog').style.display = '';
    b.q('k-dl-ptitle').textContent = mode === 'zip' ? 'Building your .zip...' : 'Downloading...';
    b.q('k-dl-fill').style.width = '0%';
    b.q('k-dl-rate').textContent = '';
    dlStartMs = Date.now();
    dlSetProgress(0);
    dlAbort = new AbortController();
    dlActive = true;
    try {
      if (mode === 'zip') await dlZip(); else await dlIndividual();
      dlToast(mode === 'zip' ? 'Bundle ready' : 'Downloads done');
    } catch (e) {
      if (e && e.name === 'AbortError') dlToast('Download cancelled');
      else dlToast('Download failed' + (e && e.message ? ': ' + e.message : ''));
    } finally {
      dlActive = false;
      dlClose();
    }
  }
  function dlClose() {
    var b = dlEnsure();
    b.root.classList.remove('on');
    b.q('k-dl-choice').style.display = '';
    b.q('k-dl-prog').style.display = 'none';
    b.q('k-dl-fill').style.width = '0%';
    b.q('k-dl-rate').textContent = '';
  }
  window.addEventListener('beforeunload', function (e) {
    if (dlActive) { e.preventDefault(); e.returnValue = ''; }
  });

  return {
    DEST: DEST,
    nav: nav,
    setActive: setActive,
    conn: conn,
    sig: sig,
    gate: gate,
    player: player,
    download: dlBegin,
    onToast: function (fn) { dlToast = fn; },
    fmtBytes: fmtBytes,
    fmtTime: fmtTime,
    esc: esc,
  };
})();
