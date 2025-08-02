#pragma once

const char* clipboard_py = R"py(
import win32clipboard
import win32con
import win32gui
import win32api
import threading
import time
import requests
import json
import hashlib
from datetime import datetime
import sys
import os

if sys.platform == "win32":
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')

# Define WM_CLIPBOARDUPDATE if not available
WM_CLIPBOARDUPDATE = 0x031D

class ClipboardMonitor:
    def __init__(self, server_url, user_id):
        self.server_url = server_url.rstrip('/')
        self.user_id = user_id
        self.endpoint = f"{self.server_url}/{self.user_id}"
        self.last_clipboard_hash = None
        self.running = False
        self.window_handle = None
        self.polling_mode = False
        
    def get_clipboard_text(self):
        """Get text from Windows clipboard"""
        try:
            win32clipboard.OpenClipboard()
            if win32clipboard.IsClipboardFormatAvailable(win32con.CF_UNICODETEXT):
                data = win32clipboard.GetClipboardData(win32con.CF_UNICODETEXT)
                win32clipboard.CloseClipboard()
                return data
            else:
                win32clipboard.CloseClipboard()
                return None
        except Exception as e:
            print(f"Error reading clipboard: {e}")
            try:
                win32clipboard.CloseClipboard()
            except:
                pass
            return None
    
    def hash_content(self, content):
        """Create hash of content to detect changes"""
        if content is None:
            return None
        return hashlib.md5(content.encode('utf-8')).hexdigest()
    
    def broadcast_clipboard(self, content):
        """Send clipboard content to server"""
        try:
            payload = {
                'content': content,
                'timestamp': datetime.now().isoformat(),
                'user_id': self.user_id
            }
            
            headers = {
                'Content-Type': 'application/json'
            }
            
            response = requests.post(
                self.endpoint, 
                data=json.dumps(payload), 
                headers=headers,
                timeout=5
            )
            
            if response.status_code == 200:
                print(f"✓ Broadcasted clipboard content (length: {len(content)} chars)")
            else:
                print(f"✗ Server responded with status: {response.status_code}")
                
        except requests.exceptions.RequestException as e:
            print(f"✗ Network error: {e}")
        except Exception as e:
            print(f"✗ Broadcast error: {e}")
    
    def clipboard_wndproc(self, hwnd, msg, wparam, lparam):
        """Windows message handler for clipboard changes"""
        try:
            if msg == WM_CLIPBOARDUPDATE:
                self.on_clipboard_change()
            elif msg == win32con.WM_DRAWCLIPBOARD:
                # Fallback for older systems
                self.on_clipboard_change()
                # Pass message to next viewer in chain
                if wparam:
                    win32gui.SendMessage(wparam, msg, wparam, lparam)
            elif msg == win32con.WM_CHANGECBCHAIN:
                # Handle chain changes
                if wparam:
                    win32gui.SendMessage(wparam, msg, wparam, lparam)
            return 0
        except Exception as e:
            print(f"Error in window procedure: {e}")
            return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)
    
    def on_clipboard_change(self):
        """Handle clipboard change event"""
        content = self.get_clipboard_text()
        if content is not None:
            content_hash = self.hash_content(content)
            
            # Only broadcast if content actually changed
            if content_hash != self.last_clipboard_hash:
                self.last_clipboard_hash = content_hash
                print(f"Clipboard changed: {content[:50]}{'...' if len(content) > 50 else ''}")
                
                # Broadcast in separate thread to avoid blocking
                threading.Thread(
                    target=self.broadcast_clipboard, 
                    args=(content,), 
                    daemon=True
                ).start()
    
    def create_window(self):
        """Create invisible window for receiving clipboard messages"""
        # Register window class
        wc = win32gui.WNDCLASS()
        wc.lpfnWndProc = self.clipboard_wndproc
        wc.lpszClassName = "ClipboardMonitor"
        wc.hInstance = win32api.GetModuleHandle()
        
        class_atom = win32gui.RegisterClass(wc)
        
        # Create window
        self.window_handle = win32gui.CreateWindow(
            class_atom,
            "ClipboardMonitor",
            0, 0, 0, 0, 0, 0, 0,
            wc.hInstance,
            None
        )
        
        return self.window_handle
    
    def start_monitoring(self):
        """Start monitoring clipboard changes"""
        print(f"Starting clipboard monitor...")
        print(f"Broadcasting to: {self.endpoint}")
        print(f"User ID: {self.user_id}")
        print("Press Ctrl+C to stop\n")
        
        # Try modern approach first, fallback to polling if needed
        try:
            self._start_event_monitoring()
        except Exception as e:
            print(f"Event monitoring failed: {e}")
            print("Falling back to polling mode...")
            self._start_polling_monitoring()
    
    def _start_event_monitoring(self):
        """Try to use Windows clipboard events"""
        try:
            # Create window for clipboard notifications
            if not self.create_window():
                raise Exception("Failed to create window")
            
            # Try to add clipboard format listener (Windows Vista+)
            try:
                import ctypes
                from ctypes import wintypes
                
                user32 = ctypes.windll.user32
                if hasattr(user32, 'AddClipboardFormatListener'):
                    success = user32.AddClipboardFormatListener(self.window_handle)
                    if not success:
                        raise Exception("AddClipboardFormatListener failed")
                    print("✓ Using modern clipboard listener")
                else:
                    raise Exception("AddClipboardFormatListener not available")
                    
            except Exception:
                # Fallback to old clipboard viewer chain
                print("✓ Using legacy clipboard viewer chain")
                win32clipboard.SetClipboardViewer(self.window_handle)
            
            self.running = True
            
            # Get initial clipboard content
            initial_content = self.get_clipboard_text()
            if initial_content:
                self.last_clipboard_hash = self.hash_content(initial_content)
                print(f"Initial clipboard: {initial_content[:50]}{'...' if len(initial_content) > 50 else ''}")
            
            # Message loop
            while self.running:
                try:
                    bRet = win32gui.GetMessage(None, 0, 0)
                    if bRet == 0 or bRet == -1:  # WM_QUIT or error
                        break
                    win32gui.TranslateMessage(bRet)
                    win32gui.DispatchMessage(bRet)
                except KeyboardInterrupt:
                    break
                    
        except KeyboardInterrupt:
            print("\nStopping clipboard monitor...")
        except Exception as e:
            print(f"Event monitoring error: {e}")
            raise
        finally:
            self.stop_monitoring()
    
    def _start_polling_monitoring(self):
        """Fallback polling method"""
        self.polling_mode = True
        self.running = True
        
        print("✓ Using polling mode (checking every 0.5 seconds)")
        
        # Get initial clipboard content
        initial_content = self.get_clipboard_text()
        if initial_content:
            self.last_clipboard_hash = self.hash_content(initial_content)
            print(f"Initial clipboard: {initial_content[:50]}{'...' if len(initial_content) > 50 else ''}")
        
        try:
            while self.running:
                try:
                    current_content = self.get_clipboard_text()
                    if current_content is not None:
                        current_hash = self.hash_content(current_content)
                        
                        if current_hash != self.last_clipboard_hash:
                            self.last_clipboard_hash = current_hash
                            print(f"Clipboard changed: {current_content[:50]}{'...' if len(current_content) > 50 else ''}")
                            
                            # Broadcast in separate thread
                            threading.Thread(
                                target=self.broadcast_clipboard, 
                                args=(current_content,), 
                                daemon=True
                            ).start()
                    
                    time.sleep(0.5)  # Check every 500ms
                    
                except KeyboardInterrupt:
                    break
                except Exception as e:
                    print(f"Polling error: {e}")
                    time.sleep(1)  # Wait longer on error
                    
        except KeyboardInterrupt:
            print("\nStopping clipboard monitor...")
        finally:
            self.running = False
            print("Clipboard monitoring stopped.")
    
    def stop_monitoring(self):
        """Stop monitoring and cleanup"""
        self.running = False
        if not self.polling_mode and self.window_handle:
            try:
                # Try to remove from clipboard listener
                import ctypes
                user32 = ctypes.windll.user32
                if hasattr(user32, 'RemoveClipboardFormatListener'):
                    user32.RemoveClipboardFormatListener(self.window_handle)
                else:
                    # Remove from clipboard viewer chain
                    win32clipboard.ChangeClipboardChain(self.window_handle, 0)
                
                win32gui.DestroyWindow(self.window_handle)
            except Exception as e:
                print(f"Cleanup warning: {e}")
        
        if not self.polling_mode:
            print("Clipboard monitoring stopped.")

def main():
    # Configuration - modify these values
    SERVER_URL = "http://localhost:8000"  # Change to your server URL
    USER_ID = "user123"  # Change to your user ID
    
    # Parse command line arguments if provided
    if len(sys.argv) >= 2:
        SERVER_URL = sys.argv[1]
    if len(sys.argv) >= 3:
        USER_ID = sys.argv[2]
    
    print("=" * 60)
    print("Windows Clipboard Monitor & Broadcaster")
    print("=" * 60)
    
    # Test server connectivity
    try:
        test_url = SERVER_URL.rstrip('/')
        response = requests.get(test_url, timeout=5)
        print(f"✓ Server accessible at {SERVER_URL}")
    except requests.exceptions.RequestException:
        print(f"⚠ Warning: Cannot reach server at {SERVER_URL}")
        print("  The app will still monitor clipboard but broadcasts may fail.")
    
    # Create and start monitor
    monitor = ClipboardMonitor(SERVER_URL, USER_ID)
    
    try:
        monitor.start_monitoring()
    except KeyboardInterrupt:
        print("\nShutting down...")
    except Exception as e:
        print(f"Fatal error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
)py";