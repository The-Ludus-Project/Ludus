#!/usr/bin/env python3
"""Scripted framed-stdio LSP peer for the generic language client (code editor M4).

Options shape the server a test needs:
  --capabilities JSON   extra ServerCapabilities, merged over the defaults
  --sync KIND           textDocumentSync change kind (0 none, 1 full, 2 incremental)
  --encoding NAME       positionEncoding to choose, or 'omit' to leave it out
  --record PATH         append one JSON line per received message, plus the
                        document text after each change
  --hold METHOD         hold the first METHOD reply until the next request arrives
  --error METHOD        answer METHOD with an internal error
  --crash-starts PATH:N exit after initialize on the first N starts, counted in PATH
  --diagnose WORD       publish an error for every WORD after each text change
  --stale-diagnostics   publish them with the previous document version
  --target WORD         answer definition-like requests and workspace/symbol with
                        the first WORD in the document
  --tokens WORD         answer semanticTokens/full with a token of legend type 0
                        for every WORD in the document
  --folding-ranges JSON answer foldingRange with this list of ranges
  --hints WORD          answer inlayHint with a parameter hint "name:" before every
                        WORD inside the requested range
  --refresh-hints       answer the first inlayHint with no hints, then send
                        workspace/inlayHint/refresh, as slangd does after
                        reading its configuration
  --snippet TEXT        answer completion with one snippet item, "Call", that
                        inserts TEXT (InsertTextFormat.Snippet)
  --resolve             answer completionItem/resolve with the item plus a
                        detail and documentation naming its label
  --rename WORD         answer prepareRename on a WORD with its range, and rename
                        with a workspace edit replacing every WORD
  --stale-rename        send that edit as documentChanges for an older version
"""
import argparse
import json
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--capabilities', default='{}')
parser.add_argument('--sync', type=int, default=2)
parser.add_argument('--encoding', default='utf-16')
parser.add_argument('--record')
parser.add_argument('--hold', action='append', default=[])
parser.add_argument('--error', action='append', default=[])
parser.add_argument('--crash-starts')
parser.add_argument('--diagnose')
parser.add_argument('--stale-diagnostics', action='store_true')
parser.add_argument('--target')
parser.add_argument('--tokens')
parser.add_argument('--folding-ranges')
parser.add_argument('--hints')
parser.add_argument('--refresh-hints', action='store_true')
parser.add_argument('--snippet')
parser.add_argument('--resolve', action='store_true')
parser.add_argument('--rename')
parser.add_argument('--stale-rename', action='store_true')
options = parser.parse_args()

text = ''
uri = ''
version = 0
held = []


def record(value):
    if options.record:
        with open(options.record, 'a', encoding='utf-8') as output:
            output.write(json.dumps(value) + '\n')


def send(value):
    body = json.dumps(dict(jsonrpc='2.0', **value)).encode()
    sys.stdout.buffer.write(b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
    sys.stdout.buffer.flush()


def offset(position):
    """The str index of a protocol position in the chosen encoding."""
    lines = text.split('\n')
    before = sum(len(line) + 1 for line in lines[:position['line']])
    line = lines[position['line']]
    if options.encoding == 'utf-8':
        return before + len(line.encode('utf-8')[:position['character']].decode('utf-8'))
    units = line.encode('utf-16-le')[:position['character'] * 2]
    return before + len(units.decode('utf-16-le'))


def position(index):
    """The protocol position of a str index in the chosen encoding."""
    before = text[:index]
    line = before.count('\n')
    column = before.rsplit('\n', 1)[-1]
    if options.encoding == 'utf-8':
        return {'line': line, 'character': len(column.encode('utf-8'))}
    return {'line': line, 'character': len(column.encode('utf-16-le')) // 2}


def publish(uri, version):
    if not options.diagnose:
        return
    diagnostics = []
    start = text.find(options.diagnose)
    while start >= 0:
        end = start + len(options.diagnose)
        diagnostics.append({'range': {'start': position(start), 'end': position(end)}, 'severity': 1,
                            'message': 'unknown name ' + options.diagnose})
        start = text.find(options.diagnose, end)
    if options.stale_diagnostics:
        version -= 1
    send({'method': 'textDocument/publishDiagnostics',
          'params': {'uri': uri, 'version': version, 'diagnostics': diagnostics}})


def should_crash():
    if not options.crash_starts:
        return False
    path, limit = options.crash_starts.rsplit(':', 1)
    try:
        with open(path, encoding='utf-8') as counter:
            starts = int(counter.read() or '0')
    except FileNotFoundError:
        starts = 0
    with open(path, 'w', encoding='utf-8') as counter:
        counter.write(str(starts + 1))
    return starts < int(limit)


while True:
    header = sys.stdin.buffer.readline()
    if not header:
        break
    length = int(header.split(b':')[1])
    sys.stdin.buffer.readline()
    message = json.loads(sys.stdin.buffer.read(length))
    method = message.get('method')
    record({'method': method, 'id': message.get('id'), 'params': message.get('params')})
    if method == 'initialize':
        capabilities = {'textDocumentSync': {'openClose': True, 'change': options.sync}}
        if options.encoding != 'omit':
            capabilities['positionEncoding'] = options.encoding
        capabilities.update(json.loads(options.capabilities))
        send({'id': message['id'], 'result': {'capabilities': capabilities}})
        if should_crash():
            sys.exit(3)
        continue
    if method == 'textDocument/didOpen':
        text = message['params']['textDocument']['text']
        uri = message['params']['textDocument']['uri']
        version = message['params']['textDocument']['version']
        record({'text': text})
        publish(message['params']['textDocument']['uri'], message['params']['textDocument']['version'])
        continue
    if method == 'textDocument/didChange':
        version = message['params']['textDocument']['version']
        for change in message['params']['contentChanges']:
            if 'range' in change:
                start = offset(change['range']['start'])
                end = offset(change['range']['end'])
                text = text[:start] + change['text'] + text[end:]
            else:
                text = change['text']
        record({'text': text})
        publish(message['params']['textDocument']['uri'], message['params']['textDocument']['version'])
        continue
    if 'id' not in message or method is None:
        continue
    # Any request releases replies held for an earlier one, so a test can
    # deliver a stale answer after a newer request.
    for response in held:
        send(response)
    held = []
    if method in options.error:
        send({'id': message['id'], 'error': {'code': -32603, 'message': 'scripted failure'}})
        continue
    params = message.get('params') or {}
    result = None
    if method == 'textDocument/completion' and options.snippet is not None:
        result = [{'label': 'Call', 'insertText': options.snippet, 'insertTextFormat': 2, 'data': 7}]
    elif method == 'textDocument/completion':
        result = [{'label': 'Item%d' % message['id'], 'position': params.get('position')}]
        if options.resolve:
            result.append({'label': 'Other%d' % message['id']})
    elif method in ('textDocument/prepareRename', 'textDocument/rename') and options.rename:
        occurrences = []
        start = text.find(options.rename)
        while start >= 0:
            occurrences.append({'start': position(start), 'end': position(start + len(options.rename))})
            start = text.find(options.rename, start + len(options.rename))
        caret = offset(params['position'])
        found = [r for r in occurrences if offset(r['start']) <= caret <= offset(r['end'])]
        if method == 'textDocument/prepareRename':
            result = {'range': found[0], 'placeholder': options.rename} if found else None
        elif options.stale_rename:
            edits = [{'range': r, 'newText': params['newName']} for r in occurrences]
            result = {'documentChanges': [{'textDocument': {'uri': uri, 'version': version - 1}, 'edits': edits}]}
        else:
            result = {'changes': {uri: [{'range': r, 'newText': params['newName']} for r in occurrences]}}
    elif method == 'completionItem/resolve' and options.resolve:
        result = dict(params)
        result['detail'] = 'resolved detail'
        result['documentation'] = {'kind': 'plaintext', 'value': 'Documentation for ' + params['label']}
    elif method == 'textDocument/hover':
        result = {'contents': {'kind': 'plaintext', 'value': 'hover %d' % message['id']}}
    elif method == 'textDocument/signatureHelp':
        result = {'signatures': [{'label': 'Call(int value)', 'parameters': [{'label': [5, 14]}]}]}
    elif options.target and options.target in text and method in (
            'textDocument/definition', 'textDocument/declaration', 'textDocument/typeDefinition',
            'textDocument/implementation', 'workspace/symbol'):
        start = text.find(options.target)
        location = {'uri': uri, 'range': {'start': position(start), 'end': position(start + len(options.target))}}
        if method != 'workspace/symbol':
            result = [location]
        elif params.get('query', '') in options.target:
            result = [{'name': options.target, 'kind': 12, 'location': location}]
        else:
            result = []
    elif method == 'textDocument/semanticTokens/full' and options.tokens:
        data = []
        previous = {'line': 0, 'character': 0}
        start = text.find(options.tokens)
        while start >= 0:
            begin = position(start)
            length = position(start + len(options.tokens))['character'] - begin['character']
            delta_line = begin['line'] - previous['line']
            delta_start = begin['character'] - (previous['character'] if delta_line == 0 else 0)
            data += [delta_line, delta_start, length, 0, 0]
            previous = begin
            start = text.find(options.tokens, start + len(options.tokens))
        result = {'data': data}
    elif method == 'textDocument/inlayHint' and options.refresh_hints:
        options.refresh_hints = False
        send({'id': message['id'], 'result': []})
        send({'id': 'refresh-1', 'method': 'workspace/inlayHint/refresh'})
        continue
    elif method == 'textDocument/inlayHint' and options.hints:
        first = offset(params['range']['start'])
        last = offset(params['range']['end'])
        result = []
        start = text.find(options.hints, first)
        while 0 <= start and start + len(options.hints) <= last:
            result.append({'position': position(start), 'label': [{'value': 'name'}, {'value': ':'}], 'kind': 2,
                           'paddingRight': True})
            start = text.find(options.hints, start + len(options.hints))
    elif method == 'textDocument/foldingRange' and options.folding_ranges:
        result = json.loads(options.folding_ranges)
    elif method in ('textDocument/documentSymbol', 'textDocument/inlayHint', 'textDocument/foldingRange',
                    'workspace/symbol', 'textDocument/references', 'textDocument/definition',
                    'textDocument/declaration', 'textDocument/typeDefinition', 'textDocument/implementation'):
        result = []
    elif method == 'textDocument/semanticTokens/full':
        result = {'data': []}
    response = {'id': message['id'], 'result': result}
    if method in options.hold:
        options.hold.remove(method)
        held.append(response)
    else:
        send(response)
