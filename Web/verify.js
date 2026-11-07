const puppeteer = require('puppeteer-core');

const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await puppeteer.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage']
  });
  const page = await browser.newPage();
  await page.setViewport({ width: 1000, height: 1180 });

  page.on('console', m => console.log('[console]', m.type(), m.text()));
  page.on('pageerror', e => console.log('[pageerror]', e.message));

  await page.goto('http://127.0.0.1:8899/index.html', { waitUntil: 'load' });

  await page.type('#username', 'admin');
  await page.type('#password', 'admin123');
  await page.click('#loginArea button');

  console.log('--- 登录后等待 13 秒收数据 ---');
  await sleep(13000);

  const readAll = () => page.evaluate(() => {
    const ids = ['temp', 'humi', 'light', 'solitemp', 'knob1', 'knob2', 'mode', 'fan_status'];
    const o = {};
    ids.forEach(i => o[i] = (document.getElementById(i) || {}).innerText);
    o.status = document.getElementById('status').innerText;
    o.statusColor = document.getElementById('status').style.color;
    o.rfidRows = document.querySelectorAll('#rfidTable tbody tr').length;
    return o;
  });

  let v = await readAll();
  console.log('VALUES_1', JSON.stringify(v, null, 0));
  await page.screenshot({ path: 'shot_1_data.png', fullPage: true });

  console.log('--- 下发 启动风扇 ---');
  await page.evaluate(() => controlFan(1));
  await sleep(9000);
  v = await readAll();
  console.log('VALUES_2', JSON.stringify(v, null, 0));
  await page.screenshot({ path: 'shot_2_fan_on.png', fullPage: true });

  console.log('--- 下发 停止风扇 ---');
  await page.evaluate(() => controlFan(0));
  await sleep(9000);
  v = await readAll();
  console.log('VALUES_3', JSON.stringify(v, null, 0));

  await browser.close();
})().catch(e => { console.error('FAIL', e); process.exit(1); });
