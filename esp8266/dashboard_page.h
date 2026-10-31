#pragma once
#include <Arduino.h>

// Read-only: the browser never sends flash commands or flash contents.
static const char dashboard_page[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SPI NOR activity</title>
<style>
  body{font:16px system-ui;margin:2rem auto;max-width:48rem;padding:0 1rem;color:#dde5f0;background:#16202d}
  section{background:#253448;padding:1rem;border-radius:.5rem;margin:1rem 0}
  progress{width:100%;height:1.3rem}.muted{color:#b3c5d8}.warn{color:#ffcf75}
  li{overflow-wrap:anywhere;margin:.25rem 0}
</style>
<h1>SPI NOR activity <small id="connection">Connecting</small></h1>
<p id="chip">Waiting for JEDEC ID</p><p class="warn" id="gap"></p>
<section id="coverage" hidden><h2>Observed chip coverage</h2>
<label>Erase sectors (4 KiB) <span id="erase-text">0 / 2048</span><progress id="erase" max="2048" value="0"></progress></label>
<label>Program pages (256 B) <span id="program-text">0 / 32768</span><progress id="program" max="32768" value="0"></progress></label>
<label>Read pages (256 B) <span id="read-text">0 / 32768</span><progress id="read" max="32768" value="0"></progress></label>
</section>
<section><h2>Latest transaction</h2><p id="last">None</p><p id="busy">WIP unknown</p></section>
<section><h2>Recent milestones</h2><ul id="history"></ul></section>
<p class="warn">Observed bus activity, not flashrom completion or verification. Bars show distinct addressed units, not completed writes.</p>
<script>
  const $ = id => document.getElementById(id);
  const kinds = new Set(['ID','UNSUP','CE','SE','BE32','BE64','PP','RD','BUSY','READY','IDLE']);
  const numeric = [1,2,4,5,6,7,8,9];
  let sid = 0, lastSeq = 0, lastLost = 0, supported = false, lastKind = '';
  function renderFlash(text) {
    const v = text.split(',');
    if (v.length !== 10 || v[0] !== 'F1' || !kinds.has(v[3])) return;
    for (const i of numeric) if (!/^[0-9]+$/.test(v[i]) ||
        !Number.isSafeInteger(Number(v[i])) || Number(v[i]) > 4294967295) return;
    const [session, seq, address, length, erased, programmed, read, lost] = numeric.map(i => Number(v[i]));
    if (!session || !seq || erased > 2048 || programmed > 32768 || read > 32768) return;
    if (session !== sid || ((v[3] === 'ID' || v[3] === 'UNSUP') && seq === 1 && lastSeq > 1)) {
      sid = session; lastSeq = 0; lastLost = 0; supported = false; lastKind = '';
      $('history').replaceChildren(); $('gap').textContent = '';
      $('coverage').hidden = true;
      $('chip').textContent = 'Waiting for JEDEC ID';
      for (const name of ['erase','program','read']) {
        $(name).value = 0; $(name + '-text').textContent = '0 / ' + $(name).max;
      }
    }
    if (seq <= lastSeq) return;
    if (seq > lastSeq + 1 && lastSeq) $('gap').textContent = 'Telemetry sequence gap';
    if (lost > lastLost) $('gap').textContent = 'Milestones lost: ' + lost;
    lastSeq = seq; lastLost = lost;
    if (v[3] === 'ID' || v[3] === 'UNSUP') {
      const hex = address.toString(16).toUpperCase().padStart(6, '0');
      supported = v[3] === 'ID' && address === 12722199;
      $('coverage').hidden = !supported;
      $('chip').textContent = supported ? 'MX25L6406E/MX25L6408E (JEDEC C2 20 17)' : 'Unsupported JEDEC ' + hex;
    }
    if (supported) {
      for (const [name, count, max] of [['erase',erased,2048],['program',programmed,32768],['read',read,32768]]) {
        $(name).value = count; $(name + '-text').textContent = count + ' / ' + max;
      }
    }
    if (v[3] === 'BUSY') $('busy').textContent = 'WIP observed set';
    if (v[3] === 'READY') $('busy').textContent = 'WIP observed clear (not verified)';
    $('last').textContent = v[3] + ' address 0x' + address.toString(16).toUpperCase() + ' length ' + length;
    if (v[3] !== lastKind && v[3] !== 'BUSY' && v[3] !== 'READY') {
      const li = document.createElement('li'); li.textContent = $('last').textContent;
      $('history').prepend(li);
      while ($('history').children.length > 16) $('history').lastElementChild.remove();
    }
    lastKind = v[3];
  }
  const url = new URL(location.href);
  url.port = String(Number(url.port || (url.protocol === 'https:' ? 443 : 80)) + 1);
  url.pathname = '/events'; url.search = ''; url.hash = '';
  const stream = new EventSource(url.href);
  stream.addEventListener('flash', ev => renderFlash(ev.data));
  stream.onerror = () => { $('connection').textContent = 'Reconnecting'; };
  stream.onopen = () => { $('connection').textContent = 'Live'; };
</script></html>)HTML";
