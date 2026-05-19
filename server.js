const express = require('express');
const app = express();

// Middleware
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// Store the latest telemetry record in memory
let latestTelemetry = null;
let telemetryHistory = [];
const MAX_HISTORY = 100; // Keep last 100 records

// ===== TELEMETRY ENDPOINT (POST) =====
app.post('/api/tracker/telemetry', (req, res) => {
  console.log('\n┌─────────────────────────────────────────────────────────┐');
  console.log('│  📡 TELEMETRY RECEIVED FROM SMART TRACKER (POST)       │');
  console.log('└─────────────────────────────────────────────────────────┘\n');
  
  const data = req.body;
  
  // Validate required fields
  if (!data.timestamp || !data.gps || !data.lte) {
    return res.status(400).json({
      status: 'error',
      message: 'Missing required fields: timestamp, gps, lte'
    });
  }
  
  // Format timestamp
  const date = new Date(data.timestamp * 1000);
  console.log(`[${date.toISOString()}] New Telemetry Record`);
  console.log('');
  
  // GPS Data
  console.log('📍 GPS DATA:');
  console.log(`   Location  : ${data.gps.latitude.toFixed(6)}, ${data.gps.longitude.toFixed(6)}`);
  console.log(`   Altitude  : ${data.gps.altitude.toFixed(2)} m`);
  console.log(`   Accuracy  : HDOP ${data.gps.hdop.toFixed(2)}`);
  console.log(`   Satellites: ${data.gps.satellites} active`);
  console.log('');
  
  // LTE Data
  console.log('📶 LTE CELLULAR:');
  console.log(`   Carrier   : ${data.lte.carrier || 'Unknown'}`);
  console.log(`   Network   : ${data.lte.network_type}`);
  console.log(`   Signal    : ${data.lte.signal_csq}/31 (CSQ)`);
  console.log(`   MCC/MNC   : ${data.lte.mcc}/${data.lte.mnc}`);
  console.log(`   Cell ID   : ${data.lte.cell_id}`);
  console.log(`   TAC       : ${data.lte.tac}`);
  console.log('');
  
  // Store in history
  latestTelemetry = {
    ...data,
    receivedAt: new Date().toISOString(),
    id: telemetryHistory.length + 1
  };
  telemetryHistory.push(latestTelemetry);
  
  // Keep only last MAX_HISTORY records
  if (telemetryHistory.length > MAX_HISTORY) {
    telemetryHistory.shift();
  }
  
  console.log(`✓ Record saved (Total: ${telemetryHistory.length})\n`);
  
  // Return success response
  res.status(200).json({
    status: 'success',
    message: 'Telemetry received and stored',
    id: latestTelemetry.id,
    timestamp: Date.now()
  });
});

// ===== TELEMETRY ENDPOINT (GET) - Accept query parameters =====
app.get('/api/tracker/telemetry', (req, res) => {
  console.log('\n┌─────────────────────────────────────────────────────────┐');
  console.log('│  📡 TELEMETRY RECEIVED FROM SMART TRACKER (GET)        │');
  console.log('└─────────────────────────────────────────────────────────┘\n');
  
  const query = req.query;
  
  // Build telemetry object from query parameters
  const data = {
    timestamp: parseInt(query.ts) || Math.floor(Date.now() / 1000),
    rtc_datetime: new Date().toISOString(),
    gps: {
      latitude: parseFloat(query.lat) || 0,
      longitude: parseFloat(query.lon) || 0,
      altitude: parseFloat(query.alt) || 0,
      hdop: 0,
      satellites: parseInt(query.sats) || 0
    },
    lte: {
      signal_csq: parseInt(query.csq) || 0,
      carrier: query.carrier || 'Unknown',
      network_type: 'LTE',
      mcc: parseInt(query.mcc) || 0,
      mnc: parseInt(query.mnc) || 0,
      cell_id: query.cell_id || '0x0',
      tac: query.tac || '0x0'
    }
  };
  
  const date = new Date(data.timestamp * 1000);
  console.log(`[${date.toISOString()}] New Telemetry Record (GET)`);
  console.log('');
  
  console.log('📍 GPS DATA:');
  console.log(`   Location  : ${data.gps.latitude.toFixed(6)}, ${data.gps.longitude.toFixed(6)}`);
  console.log(`   Altitude  : ${data.gps.altitude.toFixed(2)} m`);
  console.log(`   Satellites: ${data.gps.satellites} active`);
  console.log('');
  
  console.log('📶 LTE CELLULAR:');
  console.log(`   Signal    : ${data.lte.signal_csq}/31 (CSQ)`);
  console.log(`   MCC/MNC   : ${data.lte.mcc}/${data.lte.mnc}`);
  console.log('');
  
  // Store in history
  latestTelemetry = {
    ...data,
    receivedAt: new Date().toISOString(),
    id: telemetryHistory.length + 1
  };
  telemetryHistory.push(latestTelemetry);
  
  // Keep only last MAX_HISTORY records
  if (telemetryHistory.length > MAX_HISTORY) {
    telemetryHistory.shift();
  }
  
  console.log(`✓ Record saved (Total: ${telemetryHistory.length})\n`);
  
  res.status(200).json({
    status: 'success',
    message: 'GET telemetry received',
    timestamp: Date.now()
  });
});

// ===== GET LATEST TELEMETRY =====
app.get('/api/tracker/telemetry/latest', (req, res) => {
  if (!latestTelemetry) {
    return res.status(404).json({
      status: 'error',
      message: 'No telemetry data received yet'
    });
  }
  
  res.status(200).json({
    status: 'success',
    data: latestTelemetry
  });
});

// ===== GET TELEMETRY HISTORY =====
app.get('/api/tracker/telemetry/history', (req, res) => {
  const limit = req.query.limit ? parseInt(req.query.limit) : 10;
  const records = telemetryHistory.slice(-limit);
  
  res.status(200).json({
    status: 'success',
    total: telemetryHistory.length,
    returned: records.length,
    data: records
  });
});

// ===== HEALTH CHECK =====
app.get('/api/health', (req, res) => {
  res.status(200).json({
    status: 'ok',
    server: 'telemetry-receiver',
    uptime: process.uptime(),
    records: telemetryHistory.length,
    timestamp: new Date().toISOString()
  });
});

// ===== DASHBOARD (Simple HTML) =====
app.get('/dashboard', (req, res) => {
  const html = `
<!DOCTYPE html>
<html>
<head>
  <title>Smart Tracker Dashboard</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 20px;
      background: #1e1e1e;
      color: #e0e0e0;
    }
    .container {
      max-width: 1200px;
      margin: 0 auto;
    }
    h1 {
      text-align: center;
      color: #4CAF50;
    }
    .card {
      background: #2d2d2d;
      border: 1px solid #444;
      border-radius: 8px;
      padding: 20px;
      margin: 10px 0;
    }
    .status {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 20px;
    }
    .metric {
      background: #363636;
      padding: 15px;
      border-radius: 5px;
      border-left: 4px solid #4CAF50;
    }
    .metric-label {
      color: #aaa;
      font-size: 0.9em;
    }
    .metric-value {
      font-size: 1.5em;
      color: #4CAF50;
      font-weight: bold;
      margin-top: 5px;
    }
    .map {
      margin-top: 20px;
      padding: 20px;
      background: #363636;
      border-radius: 5px;
      min-height: 300px;
    }
    #map {
      width: 100%;
      height: 400px;
    }
    .history {
      margin-top: 20px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      background: #363636;
    }
    th {
      background: #2d2d2d;
      padding: 10px;
      text-align: left;
      border-bottom: 2px solid #4CAF50;
    }
    td {
      padding: 10px;
      border-bottom: 1px solid #444;
    }
    tr:hover {
      background: #2d2d2d;
    }
    .status-ok {
      color: #4CAF50;
    }
    .status-waiting {
      color: #FFC107;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>📡 Smart Student Tracker Dashboard</h1>
    
    <div class="card">
      <h2>Latest Telemetry</h2>
      <div id="status" class="status">
        <div class="metric">
          <div class="metric-label">GPS Location</div>
          <div class="metric-value" id="location">--</div>
        </div>
        <div class="metric">
          <div class="metric-label">Signal Strength</div>
          <div class="metric-value" id="signal">--</div>
        </div>
        <div class="metric">
          <div class="metric-label">Altitude</div>
          <div class="metric-value" id="altitude">--</div>
        </div>
        <div class="metric">
          <div class="metric-label">Satellites</div>
          <div class="metric-value" id="satellites">--</div>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>Recent Records (Last 10)</h2>
      <table id="historyTable">
        <thead>
          <tr>
            <th>Time</th>
            <th>Latitude</th>
            <th>Longitude</th>
            <th>Signal (CSQ)</th>
            <th>Satellites</th>
            <th>Carrier</th>
          </tr>
        </thead>
        <tbody id="historyBody">
          <tr><td colspan="6" class="status-waiting">Waiting for data...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <script>
    // Auto-refresh every 5 seconds
    setInterval(updateDashboard, 5000);
    updateDashboard(); // Initial load

    async function updateDashboard() {
      try {
        // Get latest telemetry
        const latestRes = await fetch('/api/tracker/telemetry/latest');
        if (latestRes.ok) {
          const latestData = await latestRes.json();
          const rec = latestData.data;
          
          document.getElementById('location').textContent = 
            rec.gps.latitude.toFixed(6) + ', ' + rec.gps.longitude.toFixed(6);
          document.getElementById('signal').textContent = 
            rec.lte.signal_csq + '/31 (' + rec.lte.carrier + ')';
          document.getElementById('altitude').textContent = 
            rec.gps.altitude.toFixed(2) + ' m';
          document.getElementById('satellites').textContent = 
            rec.gps.satellites + ' satellites';
        }
        
        // Get history
        const historyRes = await fetch('/api/tracker/telemetry/history?limit=10');
        if (historyRes.ok) {
          const historyData = await historyRes.json();
          const body = document.getElementById('historyBody');
          body.innerHTML = '';
          
          historyData.data.reverse().forEach(rec => {
            const row = document.createElement('tr');
            const time = new Date(rec.receivedAt).toLocaleTimeString();
            row.innerHTML = \`
              <td>\${time}</td>
              <td>\${rec.gps.latitude.toFixed(6)}</td>
              <td>\${rec.gps.longitude.toFixed(6)}</td>
              <td>\${rec.lte.signal_csq}/31</td>
              <td>\${rec.gps.satellites}</td>
              <td>\${rec.lte.carrier}</td>
            \`;
            body.appendChild(row);
          });
        }
      } catch (error) {
        console.error('Dashboard update error:', error);
      }
    }
  </script>
</body>
</html>
  `;
  res.send(html);
});

// ===== 404 HANDLER =====
app.use((req, res) => {
  res.status(404).json({
    status: 'error',
    message: 'Endpoint not found',
    path: req.path,
    availableEndpoints: [
      'POST   /api/tracker/telemetry',
      'GET    /api/tracker/telemetry',
      'GET    /api/tracker/telemetry/latest',
      'GET    /api/tracker/telemetry/history',
      'GET    /api/health',
      'GET    /dashboard'
    ]
  });
});

// ===== START SERVER =====
const PORT = process.env.PORT || 3000;
const HOST = '0.0.0.0';
const RAILWAY_URL = 'https://trackerservertest-production.up.railway.app';

app.listen(PORT, HOST, () => {
  console.log('\n╔════════════════════════════════════════════════════════════╗');
  console.log('║   SMART TRACKER TELEMETRY SERVER v1.0                   ║');
  console.log('╚════════════════════════════════════════════════════════════╝\n');
  console.log(`✓ Server running on http://${HOST}:${PORT}`);
  console.log(`✓ Railway Deployment: ${RAILWAY_URL}`);
  console.log(`✓ Dashboard: ${RAILWAY_URL}/dashboard`);
  console.log(`✓ API Endpoint (POST): POST ${RAILWAY_URL}/api/tracker/telemetry`);
  console.log(`✓ API Endpoint (GET): GET ${RAILWAY_URL}/api/tracker/telemetry`);
  console.log(`✓ Latest Record: GET ${RAILWAY_URL}/api/tracker/telemetry/latest`);
  console.log(`✓ History: GET ${RAILWAY_URL}/api/tracker/telemetry/history`);
  console.log(`✓ Health: GET ${RAILWAY_URL}/api/health\n`);
  console.log('Waiting for telemetry data from tracker...\n');
});
