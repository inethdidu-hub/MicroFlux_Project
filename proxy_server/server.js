const http = require('http');
const https = require('https');

const FIREBASE_HOST = 'microplux-anti-theft-app-2026-default-rtdb.firebaseio.com';
const PORT = process.env.PORT || 8080;

const server = http.createServer((req, res) => {
  // CORS Headers
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PATCH, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  console.log(`[PROXY] Incoming: ${req.method} ${req.url}`);
  
  // Clone incoming headers and override host for Firebase
  const headers = { ...req.headers };
  headers['host'] = FIREBASE_HOST;
  delete headers['accept-encoding']; // Avoid compression issues

  const options = {
    hostname: FIREBASE_HOST,
    port: 443,
    path: req.url,
    method: req.method,
    headers: headers
  };

  const firebaseReq = https.request(options, (firebaseRes) => {
    console.log(`[PROXY] Firebase Response: ${firebaseRes.statusCode}`);
    
    res.writeHead(firebaseRes.statusCode, {
      'Content-Type': firebaseRes.headers['content-type'] || 'application/json',
      'Access-Control-Allow-Origin': '*'
    });
    firebaseRes.pipe(res);
  });

  firebaseReq.on('error', (err) => {
    console.error(`[PROXY CONNECTION ERROR] URL: ${req.url}, Error: ${err.message}`);
    res.writeHead(500, { 'Content-Type': 'text/plain' });
    res.end(`Proxy Error: ${err.message}`);
  });

  // Pipe request body from client to Firebase
  req.pipe(firebaseReq);
});

server.listen(PORT, () => {
  console.log(`HTTP-to-HTTPS Proxy running on port ${PORT}`);
});
