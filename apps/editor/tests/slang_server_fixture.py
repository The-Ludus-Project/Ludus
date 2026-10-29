#!/usr/bin/env python3
"""Deterministic framed-stdio peer for production Slang client regressions."""
import json
import re
import sys
import time

text = ''
held = None

def send(value):
    body = json.dumps(dict(jsonrpc='2.0', **value)).encode()
    frame = b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body
    # Exercise header/body fragmentation without relying on OS packet boundaries.
    sys.stdout.buffer.write(frame[:7])
    sys.stdout.buffer.flush()
    time.sleep(0.005)
    sys.stdout.buffer.write(frame[7:])
    sys.stdout.buffer.flush()

while True:
    header = sys.stdin.buffer.readline()
    if not header:
        break
    length = int(header.split(b':')[1])
    sys.stdin.buffer.readline()
    message = json.loads(sys.stdin.buffer.read(length))
    method = message.get('method')
    if method == 'initialize':
        send({'id': message['id'], 'result': {'capabilities': {
            'positionEncoding': 'utf-16', 'textDocumentSync': {'change': 2},
            'completionProvider': {}, 'signatureHelpProvider': {}}}})
    elif method == 'textDocument/didOpen':
        text = message['params']['textDocument']['text']
    elif method == 'textDocument/didChange':
        # This fixture's changes replace arbitrary UTF-16 spans, including emoji.
        for change in message['params']['contentChanges']:
            def offset(position):
                lines = text.splitlines(keepends=True)
                before = ''.join(lines[:position['line']])
                units = lines[position['line']].encode('utf-16-le')[:position['character'] * 2]
                return len(before) + len(units.decode('utf-16-le'))
            start = offset(change['range']['start'])
            end = offset(change['range']['end'])
            text = text[:start] + change['text'] + text[end:]
    elif method == 'textDocument/completion':
        position = message['params']['position']
        end = dict(position)
        line = text.splitlines()[position['line']]
        before = line.encode('utf-16-le')[:position['character'] * 2].decode('utf-16-le')
        word = re.search(r'[A-Za-z_][A-Za-z0-9_]*$', before)
        start = dict(position, character=position['character'] - (len(word[0]) if word else 0))
        edit = {'range': {'start': start, 'end': end}, 'newText': 'Version'}
        item = {'label': 'Version', 'detail': 'uint', 'textEdit': edit,
                'documentation': {'kind': 'markdown', 'value': '<b>untrusted markup</b>'}}
        if 'generated-edit' in text:
            item['additionalTextEdits'] = [{'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 0}}, 'newText': '// invalid\n'}]
        if 'additional-edit' in text:
            item['additionalTextEdits'] = [{'range': {'start': {'line': 1, 'character': 0}, 'end': {'line': 1, 'character': 0}}, 'newText': '// added\n'}]
        if 'snippet' in text:
            item['insertTextFormat'] = 2
            edit['newText'] = '${1:Version}'
        if 'crash-server' in text:
            sys.exit(7)
        if 'malformed-server' in text:
            sys.stdout.buffer.write(b'Content-Length: invalid\r\n\r\n{}')
            sys.stdout.buffer.flush()
            continue
        response = {'id': message['id'], 'result': [item]}
        if held is not None:
            send(held)
            held = None
        if 'hold-response' in text:
            held = response
        else:
            send(response)
    elif method == 'textDocument/signatureHelp':
        label = 'EvaluateMaterial(SceneContext scene, SurfaceHit hit)'
        send({'id': message['id'], 'result': {'signatures': [
            {'label': label, 'parameters': [{'label': [17, 35]}, {'label': [37, 51]}],
             'documentation': {'value': '<script>unsafe</script>\n\nDefined in /private/tmp/generated.slang(2)\n'}},
            {'label': 'EvaluateMaterial(SceneContext scene, uint index)', 'parameters': []}],
            'activeSignature': 0, 'activeParameter': 1}})
