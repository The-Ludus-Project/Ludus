"""Portable paired-compiler cook for opt-in Ludus::Behavior projects.

Thanks to Julien Hamaide, 'Automatic Lua Binding System', Game Programming
Gems 7, section 7.1, pp.503–516: generate inspectable bindings from an explicit
manifest. This original implementation generates SDK-only native wrappers and
strict Luau declarations; VM dispatch stays in the private generic adapter.
Only trusted project source and paired tools are accepted. No downloads or
engine-checkout imports occur. See docs/architecture/behavior-s4.md.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

MAX_DOCUMENT = 65536
IDENTIFIER = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,62}')
ASSET = re.compile(r'[0-9a-f]{16}')
KINDS = {'uint32': ('Uint32', 'uint32', 'number'), 'bool': ('Boolean', 'bool', 'boolean'),
         'EntityRef': ('Entity', 'EntityRef', 'EntityRef')}


def require(condition, message):
    if not condition: raise ValueError(message)


def closed(value, keys):
    require(type(value) is dict and set(value) == set(keys), 'unknown or missing fields')


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate JSON key')
        result[key] = value
    return result


def read_json(path):
    require(path.stat().st_size <= MAX_DOCUMENT, 'document exceeds 64 KiB')
    return json.loads(path.read_text(), object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite JSON')))


def integer(value, low=1, high=4294967295):
    require(type(value) is int and low <= value <= high, 'integer out of range')


def name(value):
    require(type(value) is str and IDENTIFIER.fullmatch(value) and '__' not in value, 'invalid or reserved identifier')
    require(not re.fullmatch(r'Arguments[0-9]+',value), 'reserved generated identifier')
    require(value not in {'end', 'function', 'return', 'local', 'if', 'then', 'else', 'for', 'while', 'do',
                         'repeat', 'until', 'true', 'false', 'nil', 'and', 'or', 'not', 'break', 'continue',
                         'class', 'struct', 'namespace', 'template', 'auto', 'bool', 'int', 'void', 'default',
                         'State', 'Config', 'Event', 'Api', 'Contract', 'Record', 'Kind', 'Value',
                         'alignas', 'alignof', 'asm', 'bitand', 'bitor', 'catch', 'char', 'char8_t', 'char16_t',
                         'char32_t', 'compl', 'concept', 'const', 'consteval', 'constexpr', 'constinit', 'const_cast',
                         'co_await', 'co_return', 'co_yield', 'decltype', 'delete', 'double', 'dynamic_cast', 'enum',
                         'explicit', 'export', 'extern', 'float', 'friend', 'goto', 'inline', 'long', 'mutable', 'new',
                         'noexcept', 'not_eq', 'nullptr', 'operator', 'or_eq', 'private', 'protected', 'public',
                         'register', 'reinterpret_cast', 'requires', 'short', 'signed', 'sizeof', 'static',
                         'static_assert', 'static_cast', 'switch', 'this', 'thread_local', 'throw', 'try', 'typedef',
                         'typeid', 'typename', 'union', 'unsigned', 'using', 'virtual', 'volatile', 'wchar_t', 'xor',
                         'xor_eq', 'in', 'elseif', 'and_eq', 'uint32', 'EntityRef', 'transaction', 'arguments',
                         'record', 'value', 'Field', 'FieldSpec', 'Operation', 'Transaction', 'Outcome', 'Services', 'Status', 'Command', 'CommandStatus', 'CommandResult', 'Diagnostic', 'LuauProvider', 'NativeHandler', 'MakeConfig', 'MakeState', 'MakeEvent', 'TryReadConfig', 'TryReadState', 'TryReadEvent', 'SCHEMA', 'Operations', 'ConfigFields', 'StateFields', 'EventFields', 'PACKAGE'}, 'reserved identifier')


def fields(values, entities):
    require(type(values) is list and len(values) <= 16, 'field capacity')
    ids, names = set(), set()
    for value in values:
        require(type(value) is dict, 'field required')
        kind = value.get('type')
        require(type(kind) is str and kind in KINDS, 'unsupported field kind')
        integer(value.get('id')); name(value.get('name'))
        require(value['id'] not in ids and value['name'] not in names, 'duplicate field identity/name')
        ids.add(value['id']); names.add(value['name'])
        if kind == 'uint32':
            closed(value, ('id', 'name', 'type', 'min', 'max', 'default'))
            integer(value['min'], 0); integer(value['max'], value['min'])
            integer(value['default'], value['min'], value['max'])
        elif kind == 'bool':
            closed(value, ('id', 'name', 'type', 'default'))
            require(type(value['default']) is bool, 'boolean default required')
        else:
            closed(value, ('id', 'name', 'type'))
            require(entities, 'entity fields are limited to events/operation arguments')


def contract(path):
    value = read_json(path)
    closed(value, ('version', 'namespace', 'config', 'state', 'event', 'operations'))
    integer(value['version'], 1, 1)
    require(type(value['namespace']) is str and len(value['namespace']) <= 128, 'namespace required')
    for part in value['namespace'].split('::'): name(part)
    fields(value['config'], False); fields(value['state'], False); fields(value['event'], True)
    require(value['state'], 'declared state required')
    require(type(value['operations']) is list and len(value['operations']) <= 16, 'operation capacity')
    ids, names = set(), set()
    for op in value['operations']:
        closed(op, ('id', 'name', 'phase', 'capability', 'arguments'))
        integer(op['id']); name(op['name']); integer(op['phase'], 1, 255)
        integer(op['capability'], 1, (1 << 64) - 1)
        require(op['id'] not in ids and op['name'] not in names, 'duplicate operation identity/name')
        ids.add(op['id']); names.add(op['name'])
        fields(op['arguments'], True)
        require(len(op['arguments']) <= 4, 'argument capacity')
    return value


def sha(data): return hashlib.sha256(data).hexdigest()


def canonical(value): return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def specs(items):
    output = []
    for item in items:
        kind = item['type']
        low, high, default = (item['min'], item['max'], item['default']) if kind == 'uint32' else (0, int(kind == 'bool'), int(item.get('default', 0)))
        output.append('{'+f'{item["id"]},"{item["name"]}",Kind::{KINDS[kind][0]},{low},{high},{default}'+'}')
    return ',\n'.join(output)


def generate(value):
    digest = sha(canonical(value))
    header = '// Generated project contract. Thanks to Julien Hamaide, Automatic Lua Binding System,\n// Game Programming Gems 7, section 7.1, pp.503-516; explicit schemas and ordinary wrappers.\n#pragma once\n#include <ludus/runtime/behavior/behavior.h>\nnamespace '+value['namespace']+' {\nusing namespace ludus::runtime::behavior;\n'
    definitions = '--!strict\ntype EntityRef = { __opaque: "EntityRef" }\ntype CommandResult = {Status: "Accepted" | "InvalidValue" | "InvalidEntity" | "InvalidPhase" | "MissingCapability" | "CapacityExceeded", Token: number}\n'
    for role in ('config', 'state', 'event'):
        items = value[role]
        title = role.capitalize()
        header += f'/// Project-generated typed {role} values.\nstruct {title} {{\n'
        definitions += 'type '+title+' = {\n'
        for item in items:
            kind, cxx, luau = KINDS[item['type']]
            default = str(item.get('default', 0)).lower() if item['type'] != 'EntityRef' else '{}'
            header += f'    {cxx} {item["name"]} = {default};\n'
            definitions += f'    {item["name"]}: {luau},\n'
        header += '};\n'
        definitions += '}\n'
        header += f'/// Copies typed {role} values into the declared record.\ninline Record Make{title}(const {title}& value) noexcept {{ (void)value; Record record; record.Count={len(items)};\n'
        for i, item in enumerate(items):
            kind = KINDS[item['type']][0]
            data = ('{Kind::Entity,0,value.'+item['name']+'}') if kind == 'Entity' else ('{Kind::'+kind+',static_cast<uint32>(value.'+item['name']+'),{}}')
            header += f'    record.Items[{i}]={{'+str(item['id'])+','+data+'};\n'
        header += '    return record; }\n'
        header += f'/// Reads an exact typed {role} record; preserves output on failure.\ninline bool TryRead{title}(const Record& record, {title}& output) noexcept {{\n'
        header += f'    if(record.Count!={len(items)}) return false;\n    {title} candidate;\n'
        for item in items:
            kind=KINDS[item['type']][0]; member=item['name']; slot='field_'+member
            header += f'    const Value* {slot}=nullptr;\n    for(uint32 i=0;i<record.Count;++i) if(record.Items[i].Id=={item["id"]}) {{ if({slot}) return false; {slot}=&record.Items[i].Data; }}\n'
            header += f'    if(!{slot} || {slot}->Type!=Kind::{kind}) return false;\n'
            if kind=='Entity':
                header += f'    if({slot}->Scalar!=0) return false;\n    candidate.{member}={slot}->Entity;\n'
            else:
                low,high=(item['min'],item['max']) if kind=='Uint32' else (0,1)
                header += f'    if({slot}->Scalar<{low}U || {slot}->Scalar>{high}U || {slot}->Entity.World || {slot}->Entity.Session || {slot}->Entity.Execution || {slot}->Entity.Slot || {slot}->Entity.Generation) return false;\n'
                header += f'    candidate.{member}='+ (f'{slot}->Scalar!=0' if kind=='Boolean' else f'{slot}->Scalar')+';\n'
        header += '    output=candidate; return true; }\n'

        header += f'inline constexpr FieldSpec {title}Fields[] = {{\n'+specs(items)+'\n};\n' if items else ''
    definitions += 'type Api = {\n'
    for index, op in enumerate(value['operations']):
        header += f'inline constexpr FieldSpec Arguments{index}[] = {{\n'+specs(op['arguments'])+'\n};\n' if op['arguments'] else ''
        signature = ','.join(KINDS[a['type']][1]+' '+a['name'] for a in op['arguments'])
        header += f'/// Stages operation {op["id"]}; shared phase/capability/reference checks apply.\ninline CommandResult {op["name"]}(Transaction& transaction'+(','+signature if signature else '')+') noexcept {\n'
        if op['arguments']:
            data=[]
            for a in op['arguments']:
                kind=KINDS[a['type']][0]
                data.append('{Kind::Entity,0,'+a['name']+'}' if kind=='Entity' else '{Kind::'+kind+',static_cast<uint32>('+a['name']+'),{}}')
            header += '    const Value arguments[]={'+','.join(data)+'};\n'
        header += f'    return transaction.Request({op["id"]},'+('arguments' if op['arguments'] else '{}')+'); }\n'
        definitions += f'    {op["name"]}: ('+','.join(KINDS[a['type']][2] for a in op['arguments'])+') -> CommandResult,\n'
    definitions += '}\n'
    if value['operations']:
        header += 'inline constexpr Operation Operations[] = {\n'
        for i, op in enumerate(value['operations']):
            header += '{'+f'{op["id"]},"{op["name"]}",{op["phase"]},{op["capability"]}ULL,'+(f'Arguments{i}' if op['arguments'] else '{}')+'},\n'
        header += '};\n'
    header += '/// Immutable generated contract; its storage has static lifetime.\ninline const Contract SCHEMA = {'+'"'+digest+'",'+','.join(role.capitalize()+'Fields' if value[role] else '{}' for role in ('config','state','event'))+','+('Operations' if value['operations'] else '{}')+'};\n}\n'
    return digest, header, definitions


# Reuse the S2 literal-import lexer: comments/strings never manufacture imports.
def tokens(text):
    """Lex comments/strings so import-looking text cannot manufacture dependencies."""
    result, index = [], 0
    while index < len(text):
        start = index
        if text[index].isspace():
            index += 1
            continue
        comment = text.startswith("--", index)
        if comment:
            index += 2
        long = re.match(r"\[(=*)\[", text[index:])
        if long:
            end = text.find("]" + long[1] + "]", index + len(long[0]))
            require(end >= 0, "unterminated long string/comment")
            index = end + len(long[1]) + 2
            if not comment: result.append(("string", text[start:index], start, index))
            continue
        if comment:
            end = text.find("\n", index)
            index = len(text) if end < 0 else end + 1
            continue
        if text[index] in "\"'":
            quote = text[index]
            index += 1
            while index < len(text) and text[index] != quote:
                index += 2 if text[index] == "\\" else 1
            require(index < len(text), "unterminated string")
            index += 1
            result.append(("string", text[start:index], start, index))
            continue
        require(text[index] != "`", "backtick interpolation is unsupported by the literal-import profile")
        word = re.match(r"[A-Za-z_][A-Za-z0-9_]*", text[index:])
        if word:
            index += len(word[0])
            result.append(("word", word[0], start, index))
        else:
            index += 1
            result.append(("symbol", text[start:index], start, index))
    return result


def imports(text):
    scanned = tokens(text)
    result = []
    for index, token in enumerate(scanned):
        if token[:2] != ("word", "require"): continue
        require(index + 1 < len(scanned) and
                (index == 0 or scanned[index-1][1] not in (".", ":")),
                "require must use one literal asset identity")
        if scanned[index+1][0] == "string":
            literal = scanned[index+1]
        else:
            require(index + 3 < len(scanned) and scanned[index+1][1] == "(" and
                    scanned[index+2][0] == "string" and scanned[index+3][1] == ")",
                    "require must use one literal asset identity")
            literal = scanned[index+2]
        lexeme = literal[1]
        opening = re.match(r"\[(=*)\[", lexeme)
        if opening:
            # Thanks to Roblox/Luau contributors, Ast/src/Lexer.cpp,
            # Lexer::fixupMultilineString: align delimiters and first-line/EOL
            # handling with the reviewed compiler, without copying its code.
            # https://github.com/luau-lang/luau/blob/1eca9fda3e4753a1592000f6cfdf659aaa778b7d/Ast/src/Lexer.cpp#L1282
            identity = lexeme[len(opening[0]):-(len(opening[1])+2)]
            identity = identity.replace("\r\n", "\n").removeprefix("\n")
        else:
            identity = lexeme[1:-1]
        require(re.fullmatch(r"[0-9a-f]{16}", identity) and int(identity, 16), "invalid import identity")
        result.append((identity, literal[2], literal[3]))
    return result


def graph_module():
    if __package__:
        from . import behavior_graph
    else:
        import behavior_graph
    return behavior_graph


def check_graph_contract(schema):
    # The first useful visual vocabulary stays deliberately bounded. Generated
    # bindings still resolve declared state by the project's stable field IDs.
    state = {f['name']: f['type'] for f in schema['state']}
    event = {f['name']: f['type'] for f in schema['event']}
    operation = next((op for op in schema['operations'] if op['id'] == 200), None)
    require(state.get('Interactions') == 'uint32' and state.get('OpenRequested') == 'bool' and
            event.get('Amount') == 'uint32' and event.get('Target') == 'EntityRef' and
            operation is not None and operation['name'] == 'RequestDoorOpen' and
            [f['type'] for f in operation['arguments']] == ['EntityRef', 'uint32'],
            'sequence vocabulary requires Interactions/OpenRequested, Target/Amount and operation 200 RequestDoorOpen')


def package(path):
    value = read_json(path)
    closed(value, ('version', 'programs')); integer(value['version'], 1, 1)
    require(type(value['programs']) is list and 1 <= len(value['programs']) <= 8, 'program capacity')
    by_id = {}
    root = path.parent.resolve()
    for program in value['programs']:
        graph = 'graph' in program
        closed(program, ('asset', 'revision', 'graph' if graph else 'source', 'entrypoint', 'imports'))
        require(type(program['asset']) is str and ASSET.fullmatch(program['asset']) and int(program['asset'],16), 'asset identity')
        require(program['asset'] not in by_id, 'duplicate asset')
        integer(program['revision'], 1, (1 << 64) - 1)
        require(type(program['entrypoint']) is bool, 'entrypoint flag')
        require(type(program['imports']) is list and len(program['imports']) <= 8 and all(type(i) is str and ASSET.fullmatch(i) for i in program['imports']) and len(set(program['imports'])) == len(program['imports']), 'import capacity/identity')
        authored = program['graph' if graph else 'source']
        require(type(authored) is str and not Path(authored).is_absolute(), 'project-relative source path required')
        source = (root / authored).resolve()
        require(source.is_relative_to(root) and source.suffix == ('.json' if graph else '.luau') and source.is_file(), 'source escapes project or is missing')
        require(source.stat().st_size <= 131072, 'source capacity')
        text = source.read_text()
        maps = []
        authored_hash = sha(text.encode())
        if graph:
            require(program['entrypoint'] and not program['imports'], 'sequence must be an import-free entrypoint')
            document = graph_module().parse(text)
            authored_hash = graph_module().executable(document)
            text, maps = graph_module().lower(document, 'Event')
        require(text.startswith('--!strict\n'), 'strict source required')
        actual = [asset for asset, _, _ in imports(text)]
        require(set(actual) == set(program['imports']), 'declared import closure differs from literal source imports')
        program = dict(program, text=text, maps=maps, authored=authored_hash)
        by_id[program['asset']] = program
    require(any(p['entrypoint'] for p in by_id.values()), 'entrypoint required')
    result, visiting, visited = [], set(), set()
    def visit(asset):
        require(type(asset) is str and asset in by_id, 'missing import')
        require(asset not in visiting, 'import cycle')
        if asset in visited: return
        visiting.add(asset)
        for dep in sorted(by_id[asset]['imports']):
            require(dep in by_id and not by_id[dep]['entrypoint'], 'imports require a module')
            visit(dep)
        visiting.remove(asset); visited.add(asset); result.append(by_id[asset])
    for asset in sorted(by_id): visit(asset)
    return result


def cook_key(schema, programs, profile):
    return sha(canonical({'schema': schema, 'programs': programs, 'profile': profile,
                         'generator': sha(Path(__file__).read_bytes()), 'graph_generator': sha(Path(graph_module().__file__).read_bytes()), 'options': 'interpreter-o1-g2'}))


def cook(contract_path, package_path, output, profile_path, compiler, analyzer):
    schema = contract(contract_path)
    programs = package(package_path)
    if any('graph' in p for p in programs): check_graph_contract(schema)
    profile = read_json(profile_path)
    closed(profile, ('version', 'profile', 'compiler', 'analyzer')); integer(profile['version'], 1, 1)
    for key in ('profile', 'compiler', 'analyzer'):
        require(type(profile[key]) is str and re.fullmatch('[0-9a-f]{64}',profile[key]), 'invalid paired tool profile')
    require(sha(compiler.read_bytes()) == profile['compiler'] and sha(analyzer.read_bytes()) == profile['analyzer'], 'stale or modified paired host tools')
    digest, header, definitions = generate(schema)
    key = cook_key(schema, programs, profile)
    output.mkdir(parents=True, exist_ok=True)
    destination = output / key
    if destination.exists():
        evidence = read_json(destination/'evidence.json')
        require(evidence['key']==key and evidence['outputs']=={p.name:sha(p.read_bytes()) for p in destination.iterdir() if p.name!='evidence.json'}, 'modified completed cook')
    else:
        with tempfile.TemporaryDirectory(dir=output) as temporary:
            directory = Path(temporary)
            body = bytearray()
            results = []
            analysis = directory / 'analysis'; analysis.mkdir()
            sources = directory / 'sources'; sources.mkdir()
            for program in programs:
                view=program['text']
                for asset, start, end in reversed(imports(view)):
                    view=view[:start]+'\"./'+asset+'\"'+view[end:]
                # Exact text on every host: newline translation would change long-string literals.
                (analysis/(program['asset']+'.luau')).write_text(definitions+view.removeprefix('--!strict\n'),newline='')
            for program in programs:
                source = sources / (program['asset']+'.luau')
                source.write_text(definitions + program['text'].removeprefix('--!strict\n'), newline='')
                # Run beside the analysis copies: relative './asset' requires resolve there, also
                # when the output lies on another Windows drive than the working directory.
                subprocess.run([str(analyzer), '--mode=strict', source.name], check=True, capture_output=True, timeout=30,
                               cwd=analysis)
                code = subprocess.run([str(compiler), '--binary','-O1','-g2',str(source)], check=True, capture_output=True, timeout=30).stdout
                require(code and code[0]!=0 and len(code)<=262144, 'paired compilation failed')
                dependency_ids = [int(i,16) for i in program['imports']]
                body.extend(struct.pack('<QQII8Q',int(program['asset'],16),program['revision'],len(code),len(dependency_ids)|(0x100 if program['entrypoint'] else 0),*(dependency_ids+[0]*(8-len(dependency_ids)))))
                body.extend(code)
                results.append({'asset':program['asset'],'source':sha(program['text'].encode()),'authored':program['authored'],'bytecode':sha(code),'bytes':len(code),'first_line':definitions.count('\n')+1,'path':program.get('graph',program.get('source')),'maps':[dict(span,compiled_line=span['line']+definitions.count('\n')-1) for span in program['maps']]})
            data = b'LUDS4PK\0'+struct.pack('<II',1,120+len(body))+bytes.fromhex(profile['profile']+digest+key)+struct.pack('<II',len(programs),0)+body
            require(len(data)<=2*1024*1024,'package capacity')
            # Intermediate compiler paths are deliberately excluded from publication.
            for folder in (analysis,sources):
                for source in folder.iterdir(): source.unlink()
                folder.rmdir()
            (directory/'contract.h').write_text(header)
            (directory/'contract.d.luau').write_text(definitions)
            (directory/'behavior.lupack').write_bytes(data)
            embedded='#pragma once\n#include <ludus/foundation/base/types.h>\nnamespace '+schema['namespace']+' {\ninline constexpr ludus::foundation::uint8 PACKAGE[] = {\n'
            embedded+='\n'.join('    '+','.join(str(b) for b in data[i:i+16])+',' for i in range(0,len(data),16))+'\n};\ninline constexpr char PACKAGE_KEY[] = \"'+key+'\";\n}\n'
            (directory/'package.h').write_text(embedded)
            maps_header='#pragma once\n#include <ludus/foundation/base/types.h>\nnamespace '+schema['namespace']+' {\nstruct SourceSpan { ludus::foundation::uint64 Asset; ludus::foundation::int32 Line; const char* Node; };\ninline constexpr SourceSpan SOURCE_SPANS[] = {\n'
            for program in results:
                for span in program['maps']:
                    maps_header+='    {0x'+program['asset']+'ULL,'+str(span['compiled_line'])+',"'+span['node']+'"},\n'
            maps_header+='    {0,0,""},\n};\n}\n'
            (directory/'debug_maps.h').write_text(maps_header)
            evidence={'version':1,'key':key,'contract':digest,'profile':profile,'programs':results,
                      'outputs':{p.name:sha(p.read_bytes()) for p in directory.iterdir()}}
            (directory/'evidence.json').write_text(json.dumps(evidence,sort_keys=True)+'\n')
            directory.rename(destination)
    # Build consumers include forwarding headers only after the cook target completes.
    for filename in ('contract.h','package.h','debug_maps.h'):
        with tempfile.NamedTemporaryFile(mode='w', dir=output, delete=False) as file:
            forward=Path(file.name)
            try:
                file.write('#pragma once\n#include \"'+key+'/'+filename+'\"\n')
                file.close(); forward.replace(output/filename)
            finally: forward.unlink(missing_ok=True)
    # Runtime readers acquire a completed immutable directory through this atomic pointer.
    with tempfile.NamedTemporaryFile(mode='w', dir=output, delete=False) as file:
        pointer=Path(file.name)
        try:
            file.write(json.dumps({'version':1,'key':key,'directory':key})+'\n')
            file.flush(); os.fsync(file.fileno()); file.close(); pointer.replace(output/'current.json')
        finally: pointer.unlink(missing_ok=True)
    return destination


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for flag in ('contract','package','output','profile','compiler','analyzer'): parser.add_argument('--'+flag,type=Path,required=True)
    args=parser.parse_args(argv)
    result=cook(args.contract,args.package,args.output,args.profile,args.compiler,args.analyzer)
    print(result)
    return result


if __name__=='__main__':
    try: main()
    except (OSError,ValueError,KeyError,TypeError,RecursionError,subprocess.SubprocessError) as error:
        detail=error.stderr.decode(errors='replace')[-8192:] if isinstance(error,subprocess.CalledProcessError) and error.stderr else ''
        print('Behavior cook failed: '+str(error)+'\n'+detail,file=__import__('sys').stderr)
        raise SystemExit(1)
