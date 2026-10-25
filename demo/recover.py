"""Launch or reconnect to a live host; never recreates a missing session."""
import argparse
import json
from pathlib import Path
import subprocess
from reconnect_client import ReconnectClient


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=Path(__file__).resolve().parents[1] / 'build/v06/distinct-browser.exe')
    parser.add_argument('--endpoint', type=Path)
    parser.add_argument('--url', default='about:blank', help='Only used when creating a new host')
    options = parser.parse_args()
    process, connection = None, None
    try:
        if options.endpoint is None:
            process = subprocess.Popen([str(options.executable.resolve()), '--reconnect'], stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
            line = process.stdout.readline()
            if not line: raise ConnectionError(process.stderr.read().decode('utf-8', errors='replace'))
            options.endpoint = Path(json.loads(line)['endpoint'])
            print('Endpoint:', options.endpoint)
        connection = ReconnectClient(options.endpoint, options.executable)
        if process:
            browser = connection.call('create', label='Session recovery')
            token = connection.call('acquire', browser_id=browser['browser_id'], client_id='demo')['token']
            connection.call('observe', browser_id=browser['browser_id'], token=token)
            connection.call('act', browser_id=browser['browser_id'], token=token, action={'type': 'navigate', 'url': options.url})
        else:
            print(json.dumps(connection.call('snapshot'), ensure_ascii=False, indent=2))
            choice = input('Click hand back in the original window, then Enter to observe (q to disconnect): ')
            if choice.lower() != 'q':
                snapshot = connection.call('snapshot')
                for browser in snapshot['browsers']:
                    if browser['state'] != 'available':
                        print(browser['label'], browser['state'], '(not acquiring)'); continue
                    token = connection.call('acquire', browser_id=browser['browser_id'], client_id='demo')['token']
                    view = connection.call('observe', browser_id=browser['browser_id'], token=token)
                    print(browser['label'], view['page_id'], view['title'], view['url'])
                    connection.call('release', browser_id=browser['browser_id'], token=token)
        input('Enter to disconnect. Existing windows remain open: ')
    finally:
        if connection: connection.disconnect()
        if process:
            process.stdout.close(); process.stderr.close()


if __name__ == '__main__':
    main()
