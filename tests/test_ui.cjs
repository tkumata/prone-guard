const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

async function main() {
  const elements = new Map();
  const document = {getElementById(id) {
    if (!elements.has(id)) elements.set(id, {style: {}, textContent: '', addEventListener() {}});
    return elements.get(id);
  }};
  const face = {status: 'ok', detected: true, x0: 80, y0: 30, x1: 240, y1: 210};
  let health = {mode: 'jev', jev_status: 'ok', monitor_status: 'ng', jev_probability: 0.7, face_box: face};
  let disconnected = false;
  const context = vm.createContext({document, location: {hostname: 'camera'}, setTimeout() {},
    fetch: async url => {
      if (disconnected) throw Error('offline');
      return {ok: true, json: async () => url === '/settings' ?
        {mode: 'jev', threshold: 0.5, key_configured: true} : structuredClone(health)};
    }});
  const html = fs.readFileSync(new URL('../main/index.html', `file://${__filename}`), 'utf8');
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  await new Promise(setImmediate);
  const box = elements.get('face-box');
  const error = elements.get('face-error');
  const status = elements.get('status');
  assert.equal(box.style.display, 'block');
  assert.equal(box.style.left, '80px');
  assert.equal(box.style.top, '30px');
  assert.equal(box.style.width, '160px');
  assert.equal(box.style.height, '180px');

  health.face_box = {...face, status: 'invalid_response', detected: false};
  await context.poll();
  assert.equal(box.style.display, 'none');
  assert.match(error.textContent, /顔枠エラー/);
  assert.equal(status.textContent, 'NG: うつ伏せ');
  health.face_box = {...face, detected: false};
  await context.poll();
  assert.equal(error.textContent, '');
  assert.equal(box.style.display, 'none');

  health.face_box = face;
  health.jev_status = 'unjudgeable';
  health.monitor_status = 'unknown';
  await context.poll();
  assert.equal(box.style.display, 'block');
  assert.equal(error.textContent, '');
  assert.match(status.textContent, /UNKNOWN/);
  for (const failure of ['timeout', 'network_error', 'camera_error']) {
    health.face_box = {status: failure, detected: false};
    health.jev_status = failure;
    await context.poll();
    assert.equal(box.style.display, 'none');
    assert.match(error.textContent, /顔枠エラー/);
  }
  health = {...health, mode: 'local', monitor_status: 'ok', inference: 'ok', face_box: face};
  await context.poll();
  assert.equal(box.style.display, 'block');
  assert.equal(error.textContent, '');
  assert.equal(status.textContent, 'OK: 顔検出');
  health = {...health, mode: 'jev', monitor_status: 'unknown', jev_status: 'pending',
    face_box: {status: 'pending', detected: false}};
  await context.poll();
  assert.equal(box.style.display, 'none');
  assert.equal(error.textContent, '');
  disconnected = true;
  await context.poll();
  assert.equal(box.style.display, 'none');
  assert.match(status.textContent, /状態を取得できません/);
  console.log('UI face box tests passed');
}

main().catch(error => {console.error(error); process.exitCode = 1;});
