"""
Authenticated MQTT Dashboard Lambda
Serves the pump controller dashboard with Cognito authentication and device filtering.
- Regular users only see their assigned device (derived from email username)
- Admins see all devices
- Session maintained via HTTP-only cookies (tokens stored for 1 hour)

Routes:
- /           -> Redirect to /dashboard if authenticated, else to login
- /callback   -> OAuth callback, exchange code for tokens, set cookies, redirect to /dashboard
- /dashboard  -> Serve dashboard if valid token cookie exists
- /logout     -> Clear cookies and redirect to Cognito logout
"""

import json
import base64
import urllib.request
import urllib.parse
from jose import jwt, JWTError

# =============================================================================
# CONFIGURATION
# =============================================================================

COGNITO_REGION = "us-east-1"
COGNITO_USER_POOL_ID = "us-east-1_8cW4A5S5o"
COGNITO_APP_CLIENT_ID = "1fq46jiqc4oudijf4uo6lfc00g"
COGNITO_APP_CLIENT_SECRET = "1uaon97e965hd0u673h3otttgdnnvfl3jer5f2lbo5mst6tk771j"
COGNITO_DOMAIN = "https://us-east-18cw4a5s5o.auth.us-east-1.amazoncognito.com"

# Update this to your API Gateway URL
API_BASE_URL = "https://t83h16rzm6.execute-api.us-east-1.amazonaws.com/prod"
CALLBACK_URL = f"{API_BASE_URL}/callback"

# JWKS URL for token verification
JWKS_URL = f"https://cognito-idp.{COGNITO_REGION}.amazonaws.com/{COGNITO_USER_POOL_ID}/.well-known/jwks.json"

# Admin emails - these users can see all devices
ADMIN_EMAILS = [
    "bsurya@acresofice.com",
    "jnidhin@acresofice.com",
]

# MQTT Configuration
# WebSocket URL for browser connections (via Cloudflare Tunnel)
MQTT_BROKER_URL = "wss://mqtt.iceacres.com/mqtt"
MQTT_USERNAME = "aoi"
MQTT_PASSWORD = "4201"
MQTT_TOPIC_PREFIX = "pump"


# Cache for JWKS keys
_jwks_cache = None


# =============================================================================
# COGNITO AUTHENTICATION
# =============================================================================

def get_login_url():
    """Generate Cognito hosted UI login URL."""
    params = {
        "client_id": COGNITO_APP_CLIENT_ID,
        "response_type": "code",
        "scope": "openid email profile",
        "redirect_uri": CALLBACK_URL,
    }
    return f"{COGNITO_DOMAIN}/login?{urllib.parse.urlencode(params)}"


def exchange_code_for_tokens(code):
    """Exchange authorization code for tokens."""
    token_url = f"{COGNITO_DOMAIN}/oauth2/token"

    credentials = f"{COGNITO_APP_CLIENT_ID}:{COGNITO_APP_CLIENT_SECRET}"
    encoded_credentials = base64.b64encode(credentials.encode()).decode()

    data = urllib.parse.urlencode({
        "grant_type": "authorization_code",
        "client_id": COGNITO_APP_CLIENT_ID,
        "code": code,
        "redirect_uri": CALLBACK_URL
    }).encode()

    request = urllib.request.Request(token_url, data=data, method="POST")
    request.add_header("Content-Type", "application/x-www-form-urlencoded")
    request.add_header("Authorization", f"Basic {encoded_credentials}")

    with urllib.request.urlopen(request) as response:
        return json.loads(response.read().decode())


def get_jwks():
    """Fetch and cache JWKS from Cognito."""
    global _jwks_cache
    if _jwks_cache is None:
        with urllib.request.urlopen(JWKS_URL) as response:
            _jwks_cache = json.loads(response.read().decode())
    return _jwks_cache


def validate_token(id_token, access_token=None):
    """Validate the Cognito ID token and return the claims."""
    jwks = get_jwks()

    claims = jwt.decode(
        id_token,
        jwks,
        algorithms=["RS256"],
        audience=COGNITO_APP_CLIENT_ID,
        issuer=f"https://cognito-idp.{COGNITO_REGION}.amazonaws.com/{COGNITO_USER_POOL_ID}",
        access_token=access_token,
        options={"verify_at_hash": access_token is not None}
    )

    if claims.get("token_use") != "id":
        raise JWTError("Token is not an ID token")

    return claims


def get_user_info(email):
    """
    Get user info from email.
    Returns: (device_name, is_admin)
    """
    device_name = email.split("@")[0].lower()
    is_admin = email.lower() in [e.lower() for e in ADMIN_EMAILS]
    return device_name, is_admin


# =============================================================================
# HTTP RESPONSES
# =============================================================================

def html_response(status_code, body, cookies=None):
    """Return an HTML response."""
    response = {
        "statusCode": status_code,
        "headers": {
            "Content-Type": "text/html; charset=utf-8",
            "Cache-Control": "no-cache, no-store, must-revalidate",
        },
        "body": body
    }
    if cookies:
        response["multiValueHeaders"] = {"Set-Cookie": cookies}
    return response


def redirect_response(location, cookies=None):
    """Return a redirect response."""
    response = {
        "statusCode": 302,
        "headers": {
            "Location": location,
            "Cache-Control": "no-cache, no-store, must-revalidate",
        },
        "body": ""
    }
    if cookies:
        response["multiValueHeaders"] = {"Set-Cookie": cookies}
    return response


def parse_cookies(event):
    """Parse cookies from the request."""
    cookies = {}
    cookie_header = ""

    # Handle both API Gateway v1 and v2 formats
    headers = event.get("headers", {}) or {}
    if "cookie" in headers:
        cookie_header = headers["cookie"]
    elif "Cookie" in headers:
        cookie_header = headers["Cookie"]

    if cookie_header:
        for item in cookie_header.split(";"):
            item = item.strip()
            if "=" in item:
                key, value = item.split("=", 1)
                cookies[key.strip()] = value.strip()

    return cookies


def render_error_page(title, message):
    """Render an error page."""
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>{title}</title>
    <style>
        body {{
            font-family: 'Inter', system-ui, sans-serif;
            background: #fafafa;
            display: flex;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
            margin: 0;
        }}
        .error-box {{
            background: white;
            padding: 3rem;
            border-radius: 12px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            text-align: center;
            max-width: 400px;
        }}
        h1 {{ color: #ef4444; margin-bottom: 1rem; }}
        p {{ color: #64748b; margin-bottom: 1.5rem; }}
        a {{
            display: inline-block;
            background: #3b82f6;
            color: white;
            padding: 12px 24px;
            border-radius: 8px;
            text-decoration: none;
        }}
        a:hover {{ background: #2563eb; }}
    </style>
</head>
<body>
    <div class="error-box">
        <h1>{title}</h1>
        <p>{message}</p>
        <a href="{get_login_url()}">Login</a>
    </div>
</body>
</html>"""


# =============================================================================
# DASHBOARD HTML
# =============================================================================

def render_dashboard(user_email, device_name, is_admin):
    """Render the MQTT dashboard with user-specific configuration."""

    user_config = json.dumps({
        "email": user_email,
        "device": device_name,
        "isAdmin": is_admin,
    })

    admin_badge = '<span style="background:#f59e0b;color:white;padding:4px 12px;border-radius:12px;font-size:0.75rem;margin-left:12px;">ADMIN</span>' if is_admin else ''

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Pump Controller</title>
    <script src="https://unpkg.com/mqtt/dist/mqtt.min.js"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style>
        :root {{
            --bg: #fafafa;
            --surface: #ffffff;
            --border: #e2e8f0;
            --text: #0f172a;
            --text-mute: #64748b;
            --accent: #3b82f6;
            --success: #10b981;
            --danger: #ef4444;
            --warning: #f59e0b;
            --shadow-sm: 0 1px 3px rgba(0,0,0,0.05), 0 1px 2px rgba(0,0,0,0.1);
            --shadow-md: 0 4px 6px -1px rgba(0,0,0,0.1), 0 2px 4px -1px rgba(0,0,0,0.06);
            --radius: 10px;
        }}
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: 'Inter', system-ui, sans-serif;
            background: var(--bg);
            color: var(--text);
            min-height: 100vh;
            line-height: 1.5;
            overflow-x: hidden;
        }}
        .app-header {{
            max-width: 1280px;
            margin: 0 auto;
            padding: 20px 24px;
            display: flex;
            align-items: center;
            justify-content: space-between;
            border-bottom: 1px solid var(--border);
            background: white;
        }}
        .app-title {{
            font-size: 1.25rem;
            font-weight: 600;
            letter-spacing: -0.025em;
            display: flex;
            align-items: center;
        }}
        .user-info {{
            display: flex;
            align-items: center;
            gap: 16px;
        }}
        .user-email {{
            font-size: 0.875rem;
            color: var(--text-mute);
        }}
        .logout-btn {{
            padding: 8px 16px;
            background: var(--danger);
            color: white;
            border: none;
            border-radius: 6px;
            font-size: 0.875rem;
            cursor: pointer;
        }}
        .logout-btn:hover {{ background: #dc2626; }}
        .dashboard {{
            max-width: 1280px;
            margin: 32px auto;
            padding: 0 24px;
            display: grid;
            grid-template-columns: 240px 1fr;
            gap: 32px;
        }}
        .sidebar {{
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: var(--radius);
            padding: 20px;
            height: fit-content;
            box-shadow: var(--shadow-sm);
        }}
        .firmware-update-bottom {{
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: var(--radius);
            padding: 20px;
            box-shadow: var(--shadow-sm);
            grid-column: 1 / -1;
        }}
        .sidebar-header {{
            font-size: 0.875rem;
            font-weight: 600;
            color: var(--text-mute);
            margin-bottom: 16px;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            display: flex;
            align-items: center;
            gap: 8px;
        }}

        .connection-indicator {{
            width: 10px;
            height: 10px;
            border-radius: 50%;
            flex-shrink: 0;
        }}
        .connection-indicator.connected {{
            background: var(--success);
            box-shadow: 0 0 8px var(--success);
            animation: pulse-green 2s infinite;
        }}
        .connection-indicator.disconnected {{
            background: var(--danger);
        }}
        .connection-indicator.connecting {{
            background: var(--warning);
            animation: pulse-yellow 1s infinite;
        }}
        @keyframes pulse-yellow {{
            0%, 100% {{ opacity: 1; }}
            50% {{ opacity: 0.5; }}
        }}
        .device-list {{ list-style: none; }}
        .device-item {{
            padding: 10px 14px;
            border-radius: 8px;
            cursor: pointer;
            transition: background 0.15s;
            font-weight: 500;
            color: var(--text);
            display: flex;
            justify-content: space-between;
            align-items: center;
        }}
        .device-item:hover {{ background: #f1f5f9; }}
        .device-item.active {{
            background: var(--accent);
            color: white;
        }}
        .device-name {{ flex: 1; }}
        .device-indicator {{
            width: 12px;
            height: 12px;
            border-radius: 50%;
            margin-left: 8px;
            flex-shrink: 0;
        }}
        .indicator-online {{
            background: var(--success);
            box-shadow: 0 0 8px var(--success);
            animation: pulse-green 2s infinite;
        }}
        .indicator-offline {{ background: var(--danger); }}
        .indicator-unknown {{ background: var(--warning); }}
        @keyframes pulse-green {{
            0%, 100% {{ opacity: 1; }}
            50% {{ opacity: 0.6; }}
        }}
        .main-panel {{
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: var(--radius);
            padding: 32px;
            box-shadow: var(--shadow-md);
        }}
        .connection {{
            font-size: 0.875rem;
            color: var(--text-mute);
            margin-bottom: 32px;
            padding: 12px;
            border-radius: 8px;
            background: #f8fafc;
        }}
        .connection.connected {{ background: #d1fae5; color: var(--success); }}
        .connection.disconnected {{ background: #fee; color: var(--danger); }}
        .tabs {{
            display: flex;
            gap: 8px;
            margin-bottom: 24px;
            border-bottom: 2px solid var(--border);
        }}
        .tab {{
            padding: 12px 24px;
            background: none;
            border: none;
            font-weight: 600;
            cursor: pointer;
            color: var(--text-mute);
            border-bottom: 2px solid transparent;
            margin-bottom: -2px;
            transition: all 0.2s;
        }}
        .tab:hover {{ color: var(--accent); }}
        .tab.active {{
            color: var(--accent);
            border-bottom-color: var(--accent);
        }}
        .tab-content {{ display: none; }}
        .tab-content.active {{ display: block; }}
        .control-section {{
            display: flex;
            flex-direction: column;
            align-items: center;
            gap: 32px;
            margin: 40px 0;
        }}
        .buttons {{ display: flex; gap: 24px; }}
        .control-btn {{
            width: 120px;
            height: 120px;
            border: none;
            border-radius: var(--radius);
            font-size: 1.125rem;
            font-weight: 600;
            color: white;
            cursor: pointer;
            transition: all 0.2s ease;
            box-shadow: var(--shadow-sm);
        }}
        .control-btn:hover {{
            transform: translateY(-2px);
            box-shadow: var(--shadow-md);
        }}
        .control-btn:disabled {{
            opacity: 0.5;
            cursor: not-allowed;
            transform: none;
        }}
        .btn-on {{ background: var(--success); }}
        .btn-off {{ background: var(--danger); }}
        .status-box {{ text-align: center; }}
        .pump-state {{
            font-size: 2.5rem;
            font-weight: 700;
            margin-bottom: 8px;
        }}
        .state-on {{ color: var(--success); }}
        .state-off {{ color: var(--danger); }}
        .status-label {{
            font-size: 0.95rem;
            color: var(--text-mute);
        }}
        .scheduler-section {{
            background: #f8fafc;
            border-radius: var(--radius);
            padding: 24px;
            margin-bottom: 24px;
        }}
        .section-title {{
            font-size: 1.125rem;
            font-weight: 600;
            margin-bottom: 20px;
            color: var(--text);
        }}
        .form-grid {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 20px;
        }}
        .form-group {{
            display: flex;
            flex-direction: column;
            gap: 8px;
        }}
        .form-label {{
            font-size: 0.875rem;
            font-weight: 600;
            color: var(--text);
        }}
        input[type="datetime-local"],
        input[type="number"],
        select {{
            padding: 10px 12px;
            border: 1px solid var(--border);
            border-radius: 8px;
            font-size: 0.95rem;
            font-family: inherit;
        }}
        input:focus, select:focus {{
            outline: none;
            border-color: var(--accent);
        }}
        .btn {{
            padding: 12px 24px;
            border: none;
            border-radius: 8px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.2s;
            font-size: 0.95rem;
        }}
        .btn-primary {{ background: var(--accent); color: white; }}
        .btn-primary:hover {{ background: #2563eb; }}
        .btn-success {{ background: var(--success); color: white; }}
        .btn-danger {{ background: var(--danger); color: white; }}
        .btn-danger:hover {{ background: #dc2626; }}
        .schedule-list {{ margin-top: 24px; }}
        .schedule-item {{
            background: white;
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 16px;
            margin-bottom: 12px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }}
        .schedule-info {{ flex: 1; }}
        .schedule-time {{
            font-weight: 600;
            color: var(--text);
            margin-bottom: 4px;
        }}
        .schedule-details {{
            font-size: 0.875rem;
            color: var(--text-mute);
        }}
        .schedule-status {{
            padding: 4px 12px;
            border-radius: 12px;
            font-size: 0.75rem;
            font-weight: 600;
            text-transform: uppercase;
        }}
        .status-active {{ background: #d1fae5; color: var(--success); }}
        .status-inactive {{ background: #fee; color: var(--danger); }}
        .schedule-actions {{
            display: flex;
            gap: 8px;
            margin-left: 16px;
        }}
        .btn-small {{ padding: 6px 12px; font-size: 0.875rem; }}
        .sidebar-section {{
            margin-bottom: 32px; 
        }}
        .sidebar-section:last-child {{
           margin-bottom: 0;
        }}
        .ota-section {{
            background: #f8fafc;
            border-radius: var(--radius);
            padding: 20px;
        }}
        .file-input {{
            padding: 12px;
            border: 1px solid var(--border);
            border-radius: var(--radius);
            background: var(--surface);
            cursor: pointer;
            margin-bottom: 16px;
            display: block;
            width: 100%;
            font-size: 0.875rem;
        }}
        .progress-bar {{
            height: 8px;
            background: var(--border);
            border-radius: 4px;
            overflow: hidden;
            margin: 16px 0;
        }}
        .progress-fill {{
            height: 100%;
            background: var(--success);
            width: 0%;
            transition: width 0.4s ease;
        }}
        .ota-status {{
            font-size: 0.875rem;
            min-height: 1.2em;
            margin-top: 12px;
        }}
        .ota-status.waiting {{ color: var(--warning); }}
        .ota-status.success {{ color: var(--success); }}
        .ota-status.error {{ color: var(--danger); }}
        @media (max-width: 960px) {{
        .dashboard {{ 
          grid-template-columns: 1fr;
          gap: 16px;
          }}
         .sidebar {{ 
            max-width: 100%;
          }}
         .buttons {{ 
            flex-direction: column; 
            align-items: center; 
          }}
          .form-grid {{ 
            grid-template-columns: 1fr; 
          }}
           .control-btn {{
             width: 100px;
             height: 100px;
          }}
           .tabs {{
             overflow-x: auto;
            -webkit-overflow-scrolling: touch;
           }}
          .tab {{
            white-space: nowrap;
            padding: 12px 16px;
           }}
        }}
        .status-row {{
            padding: 12px;
            border-bottom: 1px solid var(--border);
            display: flex;
            justify-content: space-between;
            align-items: center;
        }}
        .status-row:last-child {{
            border-bottom: none;
        }}
        .status-label {{
            font-weight: 600;
            color: var(--text);
            font-size: 0.95rem;
        }}
        .status-value {{
            color: var(--text-mute);
            font-size: 0.95rem;
        }}
        .status-value.highlight {{
            color: var(--accent);
            font-weight: 600;
        }}
        .status-loading {{
            text-align: center;
            padding: 40px;
            color: var(--text-mute);
        }}
        .status-loading::after {{
            content: '...';
            animation: dots 1.5s steps(4, end) infinite;
        }}
        @keyframes dots {{
            0%, 20% {{ content: '.'; }}
            40% {{ content: '..'; }}
            60%, 100% {{ content: '...'; }}
        }}
    </style>
</head>
<body>
    <header class="app-header">
        <div class="app-title">Pump Controller {admin_badge}</div>
        <div class="user-info">
            <span class="user-email">{user_email}</span>
            <button class="logout-btn" onclick="logout()">Logout</button>
        </div>
    </header>

    <div class="dashboard">
    <aside class="sidebar">
        <!-- Devices Section -->
        <div class="sidebar-section">
            <div class="sidebar-header">
                Devices
                <span class="connection-indicator connecting" id="mqttConnectionIndicator"></span>
            </div>
            <ul class="device-list" id="deviceList"></ul>
        </div>
    </aside>

        <main class="main-panel">

            <div class="tabs">
                <button class="tab active" onclick="switchTab('control')">Manual Control</button>
                <button class="tab" onclick="switchTab('status')">Status</button>
                <button class="tab" onclick="switchTab('scheduler')">Scheduler</button>
            </div>

            <div id="tab-control" class="tab-content active">
                <section class="control-section">
                    <div class="buttons">
                        <button class="control-btn btn-on" id="onBtn">ON</button>
                        <button class="control-btn btn-off" id="offBtn">OFF</button>
                    </div>
                    <div class="status-box">
                        <div class="pump-state" id="pumpStatus">Pump OFF</div>
                        <div class="status-label">Current Pump Status</div>
                    </div>
                </section>
            </div>

            <div id="tab-scheduler" class="tab-content">
                <div class="scheduler-section">
                    <div class="section-title">Create New Schedule</div>
                    <div style="background: #e0f2fe; padding: 12px; border-radius: 8px; margin-bottom: 20px; font-size: 0.875rem;">
                        <strong>Timezone:</strong> <span id="userTimezone"></span>
                    </div>
                    <div class="form-grid">
                        <div class="form-group">
                            <label class="form-label">Start Date & Time</label>
                            <input type="datetime-local" id="scheduleStart" required>
                        </div>
                        <div class="form-group">
                            <label class="form-label">Duration (minutes)</label>
                            <input type="number" id="scheduleDuration" min="1" max="1440" value="60" required>
                        </div>
                        <div class="form-group">
                            <label class="form-label">Repeat Frequency</label>
                            <select id="scheduleFrequency">
                                <option value="once">Once (One-time)</option>
                                <option value="daily" selected>Daily</option>
                                <option value="weekly">Weekly</option>
                                <option value="hourly">Every Hour</option>
                                <option value="custom">Custom (hours)</option>
                            </select>
                        </div>
                        <div class="form-group" id="customIntervalGroup" style="display: none;">
                            <label class="form-label">Custom Interval (hours)</label>
                            <input type="number" id="customInterval" min="1" max="168" value="12">
                        </div>
                    </div>
                    <button class="btn btn-primary" onclick="createSchedule()">Create Schedule</button>
                </div>
                <div class="schedule-list">
                    <div class="section-title">Active Schedules</div>
                    <div id="schedulesList"></div>
                </div>
            </div>

          
            <div id="tab-status" class="tab-content">
                <section class="ota-section">
                    <div class="section-title">Device Status Information</div>
                    <button id="fetchStatusBtn" class="btn btn-primary" style="margin-bottom: 20px;">
                    Fetch Latest Status
                    </button>
                       <div id="statusDisplay" style="background: white; border: 1px solid var(--border); border-radius: 8px; padding: 20px; min-height: 200px;">
                       <p style="color: var(--text-mute); text-align: center;">Click "Fetch Latest Status" to retrieve device information</p>
                    </div>
                </section>
            </div>
        </main>
                    <!-- Firmware Update Section - Bottom of Dashboard -->
            <div class="firmware-update-bottom">
                <div class="sidebar-header">Firmware Update</div>
                <div class="ota-section">
                    <input type="file" id="firmwareFile" accept=".bin" class="file-input">
                    <button id="uploadBtn" class="btn btn-primary" style="width: 100%; font-size: 0.875rem;" disabled>Upload</button>
                    <div class="progress-bar">
                        <div id="progressFill" class="progress-fill"></div>
                    </div>
                    <div id="otaStatus" class="ota-status"></div>
                </div>
            </div>
        </div>


    <script>
        const USER_CONFIG = {user_config};
        const brokerUrl = '{MQTT_BROKER_URL}';
        const prefix = '{MQTT_TOPIC_PREFIX}';
        const TIMEOUT_MS = 30000;
        const GRACE_PERIOD = 5000;
        const INITIAL_WAIT_TIMEOUT = 30000;
        const chunkSize = 4096;

        const KNOWN_DEVICES = ['contact', 'shey','yatoohussain786','parveezshah1983','khandilawar1441'];

        const options = {{
            clean: true,
            connectTimeout: 5000,
            clientId: 'pump-' + USER_CONFIG.device + '-' + Math.random().toString(16).substr(2, 8),
            username: '{MQTT_USERNAME}',
            password: '{MQTT_PASSWORD}',
        }};

        let client = null;
        let selectedDevice = null;
        let devices = {{}};
        let schedules = [];
        let timeoutChecker = null;
        let dashboardStartTime = Date.now();
        let dashboardConnectTime = 0;
        let statusFetchTimeout = null;
        let initialDeviceWaitTimeout = null;

        function logout() {{
            window.location.href = '{API_BASE_URL}/logout';
        }}

        function isDeviceAllowed(deviceId) {{
            if (USER_CONFIG.isAdmin) return true;
            return deviceId.toLowerCase() === USER_CONFIG.device.toLowerCase();
        }}

        function getFilteredDeviceIds() {{
            const allDevices = Object.keys(devices);
            if (USER_CONFIG.isAdmin) return allDevices;
            return allDevices.filter(id => isDeviceAllowed(id));
        }}

        function switchTab(tabName) {{
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            event.target.classList.add('active');
            document.getElementById('tab-' + tabName).classList.add('active');
        }}

        document.getElementById('scheduleFrequency')?.addEventListener('change', function() {{
            document.getElementById('customIntervalGroup').style.display = this.value === 'custom' ? 'block' : 'none';
        }});

        document.getElementById('userTimezone').textContent = Intl.DateTimeFormat().resolvedOptions().timeZone;

        function startTimeoutChecker() {{
            if (timeoutChecker) clearInterval(timeoutChecker);
            timeoutChecker = setInterval(() => {{
                const now = Date.now();
                if (now - dashboardStartTime < GRACE_PERIOD) return;

                let changed = false;
                Object.keys(devices).forEach(name => {{
                    const device = devices[name];
                    if (device.online && device.lastHeartbeat > 0 && (now - device.lastHeartbeat) > TIMEOUT_MS) {{
                        device.online = false;
                        changed = true;
                    }}
                }});
                if (changed) {{
                    updateDeviceList();
                    if (selectedDevice) updatePumpStatus();
                }}
            }}, 10000);
        }}

        function updateDeviceList() {{
            const list = document.getElementById('deviceList');
            list.innerHTML = '';
            const deviceIds = getFilteredDeviceIds();

            if (!selectedDevice && deviceIds.length > 0) {{
                selectedDevice = USER_CONFIG.isAdmin ? deviceIds[0] : USER_CONFIG.device;
            }}

            deviceIds.forEach(id => {{
                const device = devices[id];
                const li = document.createElement('li');
                li.className = 'device-item' + (id === selectedDevice ? ' active' : '');

                const nameSpan = document.createElement('span');
                nameSpan.className = 'device-name';
                nameSpan.textContent = id;

                const indicator = document.createElement('span');
                let statusClass = 'indicator-unknown';
                if (device.lastHeartbeat > 0) {{
                    statusClass = device.online ? 'indicator-online' : 'indicator-offline';
                }}else if (device.online === false) {{
                  // Device was marked offline after timeout
                   statusClass = 'indicator-offline';
                }}
                indicator.className = 'device-indicator ' + statusClass;

                li.appendChild(nameSpan);
                li.appendChild(indicator);
                li.onclick = () => selectDevice(id);
                list.appendChild(li);
            }});

            if (deviceIds.length === 0) {{
                const msg = USER_CONFIG.isAdmin
                    ? 'No devices found'
                    : `Waiting for device: <strong>${{USER_CONFIG.device}}</strong>`;
                list.innerHTML = `<p style="color: var(--text-mute); text-align: center; padding: 10px; font-size: 0.875rem;">${{msg}}</p>`;
            }}
        }}

        function selectDevice(id) {{
            if (!isDeviceAllowed(id)) return;
            selectedDevice = id;
            updateDeviceList();
            updatePumpStatus();
            loadSchedules();
        }}

        function updatePumpStatus() {{
           const el = document.getElementById('pumpStatus');
           const onBtn = document.getElementById('onBtn');
           const offBtn = document.getElementById('offBtn');

           if (devices[selectedDevice]) {{
           const device = devices[selectedDevice];
        
           // ✅ UPDATED: Check for offline state (either never connected or timed out)
           if (!device.online && device.lastHeartbeat === 0) {{
              // Device never connected
              el.textContent = 'Device Offline';
              el.className = 'pump-state state-off';
              onBtn.disabled = true;
              offBtn.disabled = true;
           }} else if (!device.online && device.lastHeartbeat > 0) {{
              // Device was online but went offline
              el.textContent = 'Device Offline';
              el.className = 'pump-state state-off';
              onBtn.disabled = true;
              offBtn.disabled = true;
           }} else if (device.lastHeartbeat === 0) {{
              // Still waiting for first heartbeat (within timeout period)
              el.textContent = 'Waiting...';
              el.className = 'pump-state state-off';
              onBtn.disabled = true;
              offBtn.disabled = true;
           }} else {{
              // Device is online
              onBtn.disabled = false;
              offBtn.disabled = false;
              el.textContent = `Pump ${{device.pump_status ? 'ON' : 'OFF'}}`;
              el.className = 'pump-state ' + (device.pump_status ? 'state-on' : 'state-off');
           }}
           }} else {{
              el.textContent = 'No Device';
              el.className = 'pump-state state-off';
              onBtn.disabled = true;
              offBtn.disabled = true;
           }}
           }}

        function createSchedule() {{
            if (!client?.connected || !selectedDevice) {{
                alert('Not connected or no device selected');
                return;
            }}

            const startTime = document.getElementById('scheduleStart').value;
            const duration = parseInt(document.getElementById('scheduleDuration').value);
            const frequency = document.getElementById('scheduleFrequency').value;
            const customInterval = parseInt(document.getElementById('customInterval').value);

            if (!startTime || !duration) {{
                alert('Please fill in all required fields');
                return;
            }}

            const startTimestamp = Math.floor(new Date(startTime).getTime() / 1000);
            let intervalSeconds = 0;
            switch(frequency) {{
                case 'hourly': intervalSeconds = 3600; break;
                case 'daily': intervalSeconds = 86400; break;
                case 'weekly': intervalSeconds = 604800; break;
                case 'custom': intervalSeconds = customInterval * 3600; break;
            }}

            const schedule = {{
                id: Date.now(),
                start: startTimestamp,
                duration: duration * 60,
                interval: intervalSeconds,
                enabled: true
            }};

            client.publish(`${{prefix}}/${{selectedDevice}}/rx`, JSON.stringify({{
                method: "schedule.add",
                params: schedule
            }}), {{ qos: 1 }});

            schedules.push(schedule);
            renderSchedules();
            document.getElementById('scheduleStart').value = '';
        }}

        function deleteSchedule(scheduleId) {{
            if (!confirm('Delete this schedule?')) return;
            client.publish(`${{prefix}}/${{selectedDevice}}/rx`, JSON.stringify({{
                method: "schedule.delete",
                params: {{ id: scheduleId }}
            }}), {{ qos: 1 }});
            schedules = schedules.filter(s => s.id !== scheduleId);
            renderSchedules();
        }}

        function toggleSchedule(scheduleId) {{
            const schedule = schedules.find(s => s.id === scheduleId);
            if (!schedule) return;
            schedule.enabled = !schedule.enabled;
            client.publish(`${{prefix}}/${{selectedDevice}}/rx`, JSON.stringify({{
                method: "schedule.toggle",
                params: {{ id: scheduleId, enabled: schedule.enabled }}
            }}), {{ qos: 1 }});
            renderSchedules();
        }}

        function loadSchedules() {{
            if (!client?.connected || !selectedDevice) return;
            client.publish(`${{prefix}}/${{selectedDevice}}/rx`, JSON.stringify({{ method: "schedule.list" }}), {{ qos: 1 }});
        }}

        function renderSchedules() {{
            const container = document.getElementById('schedulesList');
            if (schedules.length === 0) {{
                container.innerHTML = '<p style="color: var(--text-mute); text-align: center; padding: 20px;">No schedules yet</p>';
                return;
            }}

            container.innerHTML = schedules.map(s => {{
                const startDate = new Date(s.start * 1000);
                const mins = Math.floor((s.duration % 3600) / 60);
                const hrs = Math.floor(s.duration / 3600);
                const durStr = hrs > 0 ? `${{hrs}}h ${{mins}}m` : `${{mins}}m`;

                let freqStr = 'Once';
                if (s.interval === 3600) freqStr = 'Hourly';
                else if (s.interval === 86400) freqStr = 'Daily';
                else if (s.interval === 604800) freqStr = 'Weekly';
                else if (s.interval > 0) freqStr = `Every ${{s.interval / 3600}}h`;

                return `
                    <div class="schedule-item">
                        <div class="schedule-info">
                            <div class="schedule-time">${{startDate.toLocaleString()}}</div>
                            <div class="schedule-details">Duration: ${{durStr}} - ${{freqStr}}</div>
                        </div>
                        <div class="schedule-status ${{s.enabled ? 'status-active' : 'status-inactive'}}">
                            ${{s.enabled ? 'Active' : 'Disabled'}}
                        </div>
                        <div class="schedule-actions">
                            <button class="btn btn-small ${{s.enabled ? 'btn-danger' : 'btn-success'}}" onclick="toggleSchedule(${{s.id}})">
                                ${{s.enabled ? 'Disable' : 'Enable'}}
                            </button>
                            <button class="btn btn-small btn-danger" onclick="deleteSchedule(${{s.id}})">Delete</button>
                        </div>
                    </div>
                `;
            }}).join('');
        }}

                function requestDeviceStatus() {{
            if (!client?.connected || !selectedDevice) {{
                alert('Not connected or no device selected');
                return;
            }}
            
            console.log('🔍 Requesting status from device:', selectedDevice);
            
            const statusDisplay = document.getElementById('statusDisplay');
            statusDisplay.innerHTML = '<div class="status-loading">Fetching device status</div>';
            
            client.publish(`${{prefix}}/${{selectedDevice}}/rx`, 'status', {{ qos: 1 }});
            
            if (statusFetchTimeout) clearTimeout(statusFetchTimeout);
            statusFetchTimeout = setTimeout(() => {{
                const currentContent = document.getElementById('statusDisplay').innerHTML;
                if (currentContent.includes('Fetching device status')) {{
                    document.getElementById('statusDisplay').innerHTML = 
                        '<p style="color: var(--danger); text-align: center;">⚠️ No response from device. Please check if device is online.</p>';
                }}
            }}, 10000);
        }}

        function displayStatusInfo(statusData) {{
            if (statusFetchTimeout) {{
                clearTimeout(statusFetchTimeout);
                statusFetchTimeout = null;
            }}
            
            const statusDisplay = document.getElementById('statusDisplay');
            
            const statusHtml = `
                <div class="status-row">
                    <span class="status-label">Site Name</span>
                    <span class="status-value highlight">${{statusData.site_name || 'N/A'}}</span>
                </div>
                <div class="status-row">
                    <span class="status-label">Firmware Version</span>
                    <span class="status-value">${{statusData.firmware_version || 'N/A'}}</span>
                </div>
                <div class="status-row">
                    <span class="status-label">Pump Type</span>
                    <span class="status-value">${{statusData.pump_type || 'N/A'}}</span>
                </div>
                <div class="status-row">
                    <span class="status-label">Pump Status</span>
                    <span class="status-value" style="color: ${{statusData.pump_status === 'ON' ? 'var(--success)' : 'var(--danger)'}}; font-weight: 600;">
                        ${{statusData.pump_status || 'N/A'}}
                    </span>
                </div>
                <div class="status-row">
                    <span class="status-label">IMSI Number</span>
                    <span class="status-value">${{statusData.imsi || 'N/A'}}</span>
                </div>
                <div class="status-row">
                    <span class="status-label">Uptime</span>
                    <span class="status-value highlight">${{statusData.uptime || 'N/A'}}</span>
                </div>
                <div style="margin-top: 16px; padding: 12px; background: #f8fafc; border-radius: 8px; text-align: center; font-size: 0.875rem; color: var(--text-mute);">
                    Last updated: ${{new Date().toLocaleString()}}
                </div>
            `;
            
            statusDisplay.innerHTML = statusHtml;
            console.log('✅ Status display updated');
        }}

        function startInitialDeviceWaitTimeout() {{
            if (initialDeviceWaitTimeout) {{
            clearTimeout(initialDeviceWaitTimeout);
           }}
    
              initialDeviceWaitTimeout = setTimeout(() => {{
              const deviceKey = USER_CONFIG.device.toLowerCase();
              const device = devices[deviceKey];
        
              // If device still has lastHeartbeat = 0 after timeout, mark as offline
              if (device && device.lastHeartbeat === 0) {{
              console.log('⏰ Initial wait timeout - device never connected:', deviceKey);
              device.online = false;
              updateDeviceList();
              updatePumpStatus();
              }}
           }}, INITIAL_WAIT_TIMEOUT);
       }}

       function requestStatusFromAllDevices() {{
          if (!client?.connected) return;
    
          console.log('📡 Requesting status from devices on load...');
    
          if (USER_CONFIG.isAdmin) {{
          // Send status request to all known devices
          KNOWN_DEVICES.forEach(deviceId => {{
            client.publish(`${{prefix}}/${{deviceId}}/rx`, 'status', {{ qos: 1 }});
            console.log(`📤 Status request sent to: ${{deviceId}}`);
          }});
        
          // Set timeout to mark devices as offline if no response
             setTimeout(() => {{
             KNOWN_DEVICES.forEach(deviceId => {{
                const device = devices[deviceId];
                if (device && device.lastHeartbeat === 0) {{
                    console.log(`❌ No status response from: ${{deviceId}} - marking offline`);
                    device.online = false;
                    updateDeviceList();
                }}
             }});
             }}, 5000); // Wait 5 seconds for responses
          }} else {{
             // Send status request to user's device
             client.publish(`${{prefix}}/${{USER_CONFIG.device}}/rx`, 'status', {{ qos: 1 }});
             console.log(`📤 Status request sent to: ${{USER_CONFIG.device}}`);
             }}
        }}

        document.getElementById('onBtn').addEventListener('click', () => {{
            if (client?.connected && selectedDevice && isDeviceAllowed(selectedDevice)) {{
                client.publish(`${{prefix}}/${{selectedDevice}}/rx`, 'PUMP ON', {{ qos: 1 }});
            }}
        }});

        document.getElementById('offBtn').addEventListener('click', () => {{
            if (client?.connected && selectedDevice && isDeviceAllowed(selectedDevice)) {{
                client.publish(`${{prefix}}/${{selectedDevice}}/rx`, 'PUMP OFF', {{ qos: 1 }});
            }}
        }});

        document.getElementById('firmwareFile').addEventListener('change', (e) => {{
            document.getElementById('uploadBtn').disabled = !e.target.files[0];
        }});

        document.getElementById('uploadBtn').addEventListener('click', async () => {{
            const file = document.getElementById('firmwareFile').files[0];
            if (!file || !selectedDevice) return;

            const progressFill = document.getElementById('progressFill');
            const otaStatus = document.getElementById('otaStatus');
            const uploadBtn = document.getElementById('uploadBtn');

            uploadBtn.disabled = true;
            otaStatus.className = 'ota-status waiting';
            otaStatus.textContent = 'Starting upload...';
            progressFill.style.width = '0%';

            try {{
                const uint8Array = new Uint8Array(await file.arrayBuffer());
                const total = uint8Array.length;
                let offset = 0;

                while (offset < total) {{
                    const chunk = uint8Array.slice(offset, offset + chunkSize);
                    const base64Chunk = btoa(String.fromCharCode.apply(null, chunk));

                    client.publish(`${{prefix}}/${{selectedDevice}}/rx`, JSON.stringify({{
                        method: "ota.upload",
                        params: {{ offset, total, chunk: base64Chunk }}
                    }}), {{ qos: 1 }});

                    offset += chunk.length;
                    const progress = (offset / total) * 100;
                    progressFill.style.width = progress + '%';
                    otaStatus.textContent = `Uploading: ${{progress.toFixed(1)}}%`;
                    await new Promise(r => setTimeout(r, 50));
                }}

                otaStatus.className = 'ota-status success';
                otaStatus.textContent = 'Upload complete! Device rebooting...';
                setTimeout(() => {{ uploadBtn.disabled = false; }}, 5000);
            }} catch (error) {{
                otaStatus.className = 'ota-status error';
                otaStatus.textContent = 'Upload failed: ' + error.message;
                uploadBtn.disabled = false;
            }}
        }});

        function connect() {{
            client = mqtt.connect(brokerUrl, options);

            client.on('connect', () => {{
                const indicator = document.getElementById('mqttConnectionIndicator');
                indicator.className = 'connection-indicator connected';

                if (USER_CONFIG.isAdmin) {{
                // Pre-populate all known devices as offline initially
                   KNOWN_DEVICES.forEach(deviceId => {{
                   if (!devices[deviceId]) {{
                       devices[deviceId] = {{ 
                       pump_status: false, 
                       lastHeartbeat: 0, 
                       online: false 
                     }};
                    }}
                   }});
        
                   client.subscribe(`${{prefix}}/+/status`);
                   client.subscribe(`${{prefix}}/+/tx`);
        
                   // Request status from all devices after subscribing
                   setTimeout(() => {{
                      requestStatusFromAllDevices();
                    }}, 500); // Small delay to ensure subscription is complete
        
                }} else {{
                   client.subscribe(`${{prefix}}/${{USER_CONFIG.device}}/status`);
                   client.subscribe(`${{prefix}}/${{USER_CONFIG.device}}/tx`);
                   if (!devices[USER_CONFIG.device]) {{
                       devices[USER_CONFIG.device] = {{ pump_status: false, lastHeartbeat: 0, online: false }};
                   }}
                   selectedDevice = USER_CONFIG.device;

                   // Request status from user's device
                   setTimeout(() => {{
                   requestStatusFromAllDevices();
                   }}, 500);
        
                   startInitialDeviceWaitTimeout();
                  }}

            dashboardConnectTime = Date.now();
            updateDeviceList();
            updatePumpStatus();
            startTimeoutChecker();
            }});

            client.on('error', (err) => {{
                const indicator = document.getElementById('mqttConnectionIndicator');
                indicator.className = 'connection-indicator disconnected';
            }});

            client.on('close', () => {{
                const indicator = document.getElementById('mqttConnectionIndicator');
                indicator.className = 'connection-indicator disconnected';
            }});

            client.on('message', (topic, message) => {{
                const parts = topic.split('/');
                if (parts.length === 3 && parts[2] === 'status') {{
                    const id = parts[1];
                    if (!isDeviceAllowed(id)) return;

                    try {{
                        const data = JSON.parse(message.toString());
                        if (data.method === 'status.notify') {{
                            const now = Date.now();
                            if (now - dashboardConnectTime < GRACE_PERIOD) return;

                            if (!devices[id]) {{
                                devices[id] = {{ pump_status: false, lastHeartbeat: 0, online: false }};
                            }}
                            if (devices[id].lastHeartbeat === 0 && initialDeviceWaitTimeout) {{
                               console.log('✅ Device connected for first time, clearing initial timeout');
                               clearTimeout(initialDeviceWaitTimeout);
                               initialDeviceWaitTimeout = null;
                            }}
                            devices[id].pump_status = !!data.params?.pump_status;
                            devices[id].lastHeartbeat = now;
                            devices[id].online = true;

                            updateDeviceList();
                            if (id === selectedDevice) updatePumpStatus();
                        }}
                    }} catch (e) {{}}
                }}

                if (parts.length === 3 && parts[2] === 'tx') {{
                const deviceId = parts[1].toLowerCase();
                if (!isDeviceAllowed(deviceId)) return;
                
                try {{
                    const data = JSON.parse(message.toString());
                    
                    if (data.method === 'status.response') {{
                        console.log('📊 Status response:', data.params);
                        
                        if (!devices[deviceId]) {{
                            devices[deviceId] = {{ pump_status: false, lastHeartbeat: 0, online: false }};
                        }}
                        
                        devices[deviceId].pump_status = data.params.pump_status === 'ON';
                        devices[deviceId].firmware_version = data.params.firmware_version;
                        devices[deviceId].pump_type = data.params.pump_type;
                        devices[deviceId].imsi = data.params.imsi;
                        devices[deviceId].uptime = data.params.uptime;
                        devices[deviceId].lastHeartbeat = Date.now();
                        devices[deviceId].online = true;
                        
                        displayStatusInfo(data.params);
                        
                        updateDeviceList();
                        updatePumpStatus();
                    }}
                    else if (data.result && data.result.schedules) {{
                        schedules = data.result.schedules;
                        renderSchedules();
                    }}
                }} catch (e) {{
                    console.error('Parse error:', e);
                }}
            }}
            }});
                // Add button listener
            document.getElementById('fetchStatusBtn').addEventListener('click', () => {{
            requestDeviceStatus();
        }});
        }}

        connect();
    </script>
</body>
</html>"""


# =============================================================================
# LAMBDA HANDLER
# =============================================================================

def lambda_handler(event, context):
    """
    Lambda handler for authenticated MQTT dashboard.
    Handles:
    - /callback -> OAuth callback, validate token, set cookie, redirect to /dashboard
    - /dashboard -> Serve dashboard if valid token cookie exists
    - / -> Redirect to login
    """
    try:
        path = event.get("path", "") or event.get("rawPath", "") or "/"
        query_params = event.get("queryStringParameters") or {}
        cookies = parse_cookies(event)

        # Handle /dashboard - serve dashboard if valid token exists
        if path == "/dashboard":
            id_token = cookies.get("id_token")
            access_token = cookies.get("access_token")

            if not id_token:
                # No token, redirect to login
                return redirect_response(get_login_url())

            # Validate token
            try:
                claims = validate_token(id_token, access_token)
            except JWTError:
                # Invalid/expired token, redirect to login
                return redirect_response(get_login_url())

            # Get user info
            email = claims.get("email")
            if not email:
                return redirect_response(get_login_url())

            device_name, is_admin = get_user_info(email)

            # Serve dashboard
            return html_response(200, render_dashboard(email, device_name, is_admin))

        # Handle /callback - OAuth callback
        if path == "/callback":
            code = query_params.get("code")

            if not code:
                # No code but maybe we have a valid token already
                id_token = cookies.get("id_token")
                if id_token:
                    try:
                        validate_token(id_token, cookies.get("access_token"))
                        return redirect_response(f"{API_BASE_URL}/dashboard")
                    except JWTError:
                        pass
                return redirect_response(get_login_url())

            # Exchange code for tokens
            try:
                token_response = exchange_code_for_tokens(code)
            except Exception as e:
                return html_response(401, render_error_page("Authentication Failed", f"Could not exchange code: {str(e)}"))

            id_token = token_response.get("id_token")
            access_token = token_response.get("access_token")

            if not id_token:
                return html_response(400, render_error_page("Error", "Failed to get ID token"))

            # Validate token
            try:
                claims = validate_token(id_token, access_token)
            except JWTError as e:
                return html_response(401, render_error_page("Invalid Token", str(e)))

            # Get user info
            email = claims.get("email")
            if not email:
                return html_response(400, render_error_page("Error", "No email in token"))

            # Set cookies and redirect to dashboard (removes code from URL)
            # Cookie expires in 1 hour (matches typical Cognito token expiry)
            cookie_options = "Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=3600"
            cookies_to_set = [
                f"id_token={id_token}; {cookie_options}",
                f"access_token={access_token}; {cookie_options}",
            ]

            return redirect_response(f"{API_BASE_URL}/dashboard", cookies_to_set)

        # Handle /logout - clear cookies and redirect to Cognito logout
        if path == "/logout":
            # Clear cookies by setting them to expire immediately
            clear_cookies = [
                "id_token=; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=0",
                "access_token=; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=0",
            ]
            cognito_logout_url = f"{COGNITO_DOMAIN}/logout?client_id={COGNITO_APP_CLIENT_ID}&logout_uri={urllib.parse.quote(API_BASE_URL)}"
            return redirect_response(cognito_logout_url, clear_cookies)

        # Handle root / - redirect to login or dashboard
        if path == "/" or path == "":
            id_token = cookies.get("id_token")
            if id_token:
                try:
                    validate_token(id_token, cookies.get("access_token"))
                    return redirect_response(f"{API_BASE_URL}/dashboard")
                except JWTError:
                    pass
            return redirect_response(get_login_url())

        # Unknown path
        return html_response(404, render_error_page("Not Found", "Use the login link to access the dashboard."))

    except Exception as e:
        return html_response(500, render_error_page("Error", f"Internal error: {str(e)}"))
