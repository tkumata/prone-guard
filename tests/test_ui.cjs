const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

async function main() {
  const elements = new Map();
  const document = {getElementById(id) {
    if (!elements.has(id)) elements.set(id, {value: '', checked: false, textContent: '', disabled: true,
      addEventListener(event, handler) {this[event] = handler;}});
    return elements.get(id);
  }};
  let settings = {wifi_ssid: 'network', bed_id: 'bed-01', destination_url: 'https://edge/v1/measurements', key_configured: true, restart_required: false};
  let failure = false;
  let saved;
  const context = vm.createContext({document, setTimeout() {}, fetch: async (url, options) => {
    if (failure) return {ok: false, status: 503};
    if (options?.method === 'POST') {
      saved = JSON.parse(options.body);
      settings = {...settings, ...saved, key_configured: !saved.clear_device_key, restart_required: true};
    }
    return {ok: true, json: async () => structuredClone(url === '/health' ?
      {wifi: 'connected', clock: 'synchronized', camera: 'ok', depth: 'error', sender: 'not_configured'} : settings)};
  }});
  const html = fs.readFileSync(new URL('../main/index.html', `file://${__filename}`), 'utf8');
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  await new Promise(setImmediate);
  const get = id => document.getElementById(id);
  assert.equal(get('save').disabled, false);
  assert.equal(get('wifi-ssid').value, 'network');
  assert.match(get('key-status').textContent, /設定済み/);
  assert.match(get('health').textContent, /深度: error/);
  get('wifi-ssid').value = 'new-network';
  get('device-key').value = 'a'.repeat(64);
  get('wifi-password').value = 'new-password';
  await get('settings').submit({preventDefault() {}});
  assert.equal(saved.wifi_ssid, 'new-network');
  assert.equal(saved.device_key, 'a'.repeat(64));
  assert.equal(get('device-key').value, '');
  assert.equal(get('wifi-password').value, '');
  assert.match(get('settings-status').textContent, /再起動/);
  get('clear-device-key').checked = true;
  await get('settings').submit({preventDefault() {}});
  assert.equal(saved.clear_device_key, true);
  assert.match(get('key-status').textContent, /未設定/);
  failure = true;
  get('device-key').value = 'b'.repeat(64);
  await get('settings').submit({preventDefault() {}});
  assert.match(get('settings-status').textContent, /503/);
  assert.equal(get('device-key').value, 'b'.repeat(64));
  assert.equal(get('save').disabled, false);
  await context.poll();
  assert.match(get('health').textContent, /取得できません/);
  console.log('Management UI behavior tests passed');
}
main().catch(error => {console.error(error); process.exitCode = 1;});
