#pragma once

const char* broadcast_py = R"py(
from flask import Flask, request, jsonify
import json
import os
from datetime import datetime
import threading
import logging
import sys

if sys.platform == "win32":
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')

app = Flask(__name__)

# Configuration
LOG_FILE = "clipboard_log.txt"
JSON_LOG_FILE = "clipboard_log.json"
MAX_CONTENT_DISPLAY = 100  # Max characters to display in console

# Fix console encoding for Windows BEFORE setting up logging
if sys.platform == 'win32':
    import codecs
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except AttributeError:
        sys.stdout = codecs.getwriter('utf-8')(sys.stdout.detach())
        sys.stderr = codecs.getwriter('utf-8')(sys.stderr.detach())

    os.environ['PYTHONIOENCODING'] = 'utf-8'

class SafeFormatter(logging.Formatter):
    def format(self, record):
        try:
            return super().format(record)
        except UnicodeEncodeError:
            record.msg = str(record.msg).encode('ascii', 'replace').decode('ascii')
            return super().format(record)

class SafeStreamHandler(logging.StreamHandler):
    def emit(self, record):
        try:
            super().emit(record)
        except UnicodeEncodeError:
            record.msg = str(record.getMessage()).encode('ascii', 'replace').decode('ascii')
            super().emit(record)

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('server.log', encoding='utf-8'),
        SafeStreamHandler()
    ]
)

class ClipboardLogger:
    def __init__(self):
        self.lock = threading.Lock()
        self.total_broadcasts = 0

    def log_clipboard_data(self, user_id, data):
        with self.lock:
            self.total_broadcasts += 1
            timestamp = datetime.now().isoformat()
            content = data.get('content', '')
            original_timestamp = data.get('timestamp', timestamp)
            try:
                with open(LOG_FILE, 'a', encoding='utf-8') as f:
                    f.write(f"\n{'='*80}\n")
                    f.write(f"Broadcast #{self.total_broadcasts}\n")
                    f.write(f"User ID: {user_id}\n")
                    f.write(f"Timestamp: {original_timestamp}\n")
                    f.write(f"Server Received: {timestamp}\n")
                    f.write(f"Content Length: {len(content)} characters\n")
                    f.write(f"Content:\n{content}\n")
            except Exception as e:
                logging.error(f"Error writing to text log: {e}")

            try:
                json_entry = {
                    'broadcast_number': self.total_broadcasts,
                    'user_id': user_id,
                    'client_timestamp': original_timestamp,
                    'server_timestamp': timestamp,
                    'content': content,
                    'content_length': len(content)
                }
                if os.path.exists(JSON_LOG_FILE):
                    with open(JSON_LOG_FILE, 'r', encoding='utf-8') as f:
                        log_data = json.load(f)
                else:
                    log_data = {'broadcasts': []}
                log_data['broadcasts'].append(json_entry)
                with open(JSON_LOG_FILE, 'w', encoding='utf-8') as f:
                    json.dump(log_data, f, indent=2, ensure_ascii=False)
            except Exception as e:
                logging.error(f"Error writing JSON log: {e}")

    def get_stats(self):
        try:
            file_size = os.path.getsize(LOG_FILE) if os.path.exists(LOG_FILE) else 0
            json_size = os.path.getsize(JSON_LOG_FILE) if os.path.exists(JSON_LOG_FILE) else 0
            return {
                'total_broadcasts': self.total_broadcasts,
                'log_file_size': file_size,
                'json_log_size': json_size,
                'uptime': datetime.now().isoformat()
            }
        except Exception:
            return {'error': 'Could not retrieve stats'}

clipboard_logger = ClipboardLogger()

@app.route('/', methods=['GET'])
def home():
    stats = clipboard_logger.get_stats()
    html = f"""
    <html>
    <head><title>spill: clipboard broadcast server</title></head>
    <body>
        <h1>Spill</h1>
        <h2>Status: Running ✓</h2>

        <h3>Statistics:</h3>
        <ul>
            <li>Total Broadcasts Received: <strong>{stats.get('total_broadcasts', 0)}</strong></li>
            <li>Log File Size: <strong>{stats.get('log_file_size', 0)} bytes</strong></li>
            <li>JSON Log Size: <strong>{stats.get('json_log_size', 0)} bytes</strong></li>
            <li>Server Started: <strong>{stats.get('uptime', 'Unknown')}</strong></li>
        </ul>

        <h3>Endpoints:</h3>
        <ul>
            <li><code>POST /&lt;user_id&gt;</code> - Receive clipboard broadcasts</li>
            <li><code>GET /stats</code> - Get server statistics (JSON)</li>
            <li><code>GET /logs/&lt;user_id&gt;</code> - Get recent logs for user (JSON)</li>
        </ul>

        <h3>Log Files:</h3>
        <ul>
            <li><strong>clipboard_log.txt</strong> - Human readable log</li>
            <li><strong>clipboard_log.json</strong> - Structured JSON log</li>
            <li><strong>server.log</strong> - Server activity log</li>
        </ul>

        <p><em>Refresh this page to see updated statistics.</em></p>
    </body>
    </html>
    """
    return html

@app.route('/<user_id>', methods=['POST'])
def receive_clipboard(user_id):
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'No JSON data received'}), 400
        content = data.get('content', '')
        timestamp = data.get('timestamp', datetime.now().isoformat())
        clipboard_logger.log_clipboard_data(user_id, data)
        content_preview = content[:MAX_CONTENT_DISPLAY]
        if len(content) > MAX_CONTENT_DISPLAY:
            content_preview += "..."
        try:
            log_msg = f"[CLIPBOARD] [{user_id}] Received ({len(content)} chars): {repr(content_preview)}"
            logging.info(log_msg)
        except Exception as e:
            logging.info(f"[CLIPBOARD] [{user_id}] Received ({len(content)} chars) [display error: {e}]")
        return jsonify({
            'status': 'success',
            'message': 'Clipboard data received and logged',
            'user_id': user_id,
            'content_length': len(content),
            'broadcast_number': clipboard_logger.total_broadcasts
        }), 200
    except Exception as e:
        logging.error(f"Error processing clipboard from {user_id}: {e}")
        return jsonify({'error': str(e)}), 500

@app.route('/stats', methods=['GET'])
def get_stats():
    return jsonify(clipboard_logger.get_stats())

@app.route('/logs/<user_id>', methods=['GET'])
def get_user_logs(user_id):
    try:
        if not os.path.exists(JSON_LOG_FILE):
            return jsonify({'logs': [], 'message': 'No logs found'})
        with open(JSON_LOG_FILE, 'r', encoding='utf-8') as f:
            log_data = json.load(f)
        user_logs = [
            log for log in log_data.get('broadcasts', [])
            if log.get('user_id') == user_id
        ]
        recent_logs = user_logs[-50:] if len(user_logs) > 50 else user_logs
        return jsonify({
            'user_id': user_id,
            'total_logs': len(user_logs),
            'recent_logs': recent_logs
        })
    except Exception as e:
        logging.error(f"Error retrieving logs for {user_id}: {e}")
        return jsonify({'error': str(e)}), 500

@app.route('/clear-logs', methods=['POST'])
def clear_logs():
    try:
        files_cleared = []
        for log_file in [LOG_FILE, JSON_LOG_FILE]:
            if os.path.exists(log_file):
                os.remove(log_file)
                files_cleared.append(log_file)
        clipboard_logger.total_broadcasts = 0
        logging.info("Log files cleared by admin request")
        return jsonify({
            'status': 'success',
            'message': 'Log files cleared',
            'files_cleared': files_cleared
        })
    except Exception as e:
        logging.error(f"Error clearing logs: {e}")
        return jsonify({'error': str(e)}), 500

if __name__ == '__main__':
    print("=" * 60)
    print("Clipboard Broadcast Server")
    print("=" * 60)
    print(f"Server will log clipboard data to:")
    print(f"  • {LOG_FILE} (human readable)")
    print(f"  • {JSON_LOG_FILE} (structured data)")
    print(f"  • server.log (server activity)")
    print("\nEndpoints:")
    print(f"  • POST /<user_id> - Receive clipboard broadcasts")
    print(f"  • GET / - Server status and stats")
    print(f"  • GET /stats - Statistics (JSON)")
    print(f"  • GET /logs/<user_id> - User logs (JSON)")
    print("=" * 60)
    app.run(host='0.0.0.0', port=8000, debug=False, threaded=True)
)py";
