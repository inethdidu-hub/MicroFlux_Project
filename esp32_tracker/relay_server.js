const http = require('http');
const https = require('https');

const FIREBASE_HOST = 'microplux-anti-theft-app-2026-default-rtdb.firebaseio.com';
const PORT = process.env.PORT || 8080;

const server = http.createServer((req, res) => {
  // Add CORS headers to prevent browser issues if accessed from web app
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PATCH, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  console.log(`[PROXY] Relaying request: ${req.method} ${req.url}`);

  // Create HTTPS options to forward to Firebase RTD
  const options = {
    hostname: FIREBASE_HOST,
    port: 443,
    path: req.url,
    method: req.method,
    headers: {
      'Content-Type': req.headers['content-type'] || 'application/json',
      'Host': FIREBASE_HOST
    }
  };

  const firebaseReq = https.request(options, (firebaseRes) => {
    // Forward Firebase headers and status code back to client
    res.writeHead(firebaseRes.statusCode, {
      'Content-Type': firebaseRes.headers['content-type'] || 'application/json',
      'Access-Control-Allow-Origin': '*'
    });
    firebaseRes.pipe(res);
  });

  firebaseReq.on('error', (err) => {
    console.error(`[PROXY ERROR] ${err.message}`);
    res.writeHead(500, { 'Content-Type': 'text/plain' });
    res.end(`Proxy Error: ${err.message}`);
  });

  // Pipe the request body from A9G to Firebase
  req.pipe(firebaseReq);
});

server.listen(PORT, () => {
  console.log(`Generic HTTP-to-HTTPS Firebase Proxy running on port ${PORT}`);
});
