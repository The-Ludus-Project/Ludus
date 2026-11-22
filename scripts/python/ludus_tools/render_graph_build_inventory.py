"""Attribute a graph build's shader artifacts and pipeline recipes to authored choices.

The inventory is report data for ``build-report.json``. It links each root
configuration's qualified graph node to its effective technique, stage programs,
target artifact keys and portable recipe keys, and it lists manifest techniques
cooked without a root route and registered programs that emit nothing.

Artifact bytes are charged once per unique key. A stage request is a fresh
compile (``miss``), a hit in the local artifact cache (``hit``), or a key that an
earlier request in the same build already resolved (``reuse``). Compile time is
recorded only for work this build performed. None of this data, nor any host
path, enters package or stage identity, and the inventory sets no budget.

Device pipeline preparation is outside this report: package recipes are portable
descriptions, and their bytes are not a measure of driver residency. See
docs/architecture/render-graph-authoring.md#attribute-variant-cost-before-optimizing-proposed.
"""

STAGE_ROLES = ('vertex', 'fragment', 'compute', 'native_pipeline')
FORWARD_ROLE_PREFIX = 'forward/'


def recipe_stage_keys(recipe):
    """Return {stage role: artifact key} for the stages one recipe record names."""
    keys = {role: recipe[role] for role in STAGE_ROLES if isinstance(recipe.get(role), str)}
    for label, key in recipe.get('forward_stages', {}).items():
        keys[FORWARD_ROLE_PREFIX + label] = key
    return keys


def choice_reference(choice):
    return {'configuration': choice['configuration'], 'node': choice['node']}


def directory_bytes(path):
    return sum(item.stat().st_size for item in path.rglob('*') if item.is_file())


def describe(*, plans, routes, techniques, recipes, entries, requests, compile_seconds, compiled, program_ids,
             artifact_directory):
    """Return the inventory of one build.

    plans are the root configuration plans and routes the admitted raster routes
    computed by the build. entries maps (program, profile) to the artifact key;
    requests maps the same pairs to 'miss', 'hit' or 'reuse'. compile_seconds maps
    a freshly compiled key to its compile duration, excluding a wait for a worker
    lane. compiled maps (program, profile) to its stage descriptor. program_ids
    names every registered program. artifact_directory(key) is the directory that
    holds a key's published files. Ordering is deterministic.
    """
    raster_recipe_ids = {(route['configuration'], route['node']): set(route['recipe_ids']) for route in routes}
    recipes_by_technique = {}
    for recipe in recipes:
        recipes_by_technique.setdefault(recipe['technique'], []).append(recipe)

    def profiles_of(selected):
        profiles = {}
        for recipe in selected:
            profile = profiles.setdefault(recipe['profile'], {'stages': {}, 'recipes': []})
            profile['stages'].update(recipe_stage_keys(recipe))
            if recipe['key'] not in profile['recipes']:
                profile['recipes'].append(recipe['key'])
        for profile in profiles.values():
            profile['recipes'].sort()
        return dict(sorted(profiles.items()))

    choices = []
    for plan in plans:
        for operation in plan['passes']:
            if 'technique' not in operation:
                continue
            reference = (plan['configuration'], operation['id'])
            selected = [recipe for recipe in recipes_by_technique.get(operation['technique'], [])
                        if plan['configuration'] in recipe.get('configurations', (plan['configuration'],))]
            choice = {'configuration': plan['configuration'], 'node': operation['id'], 'kind': operation['kind'],
                      'technique': operation['technique']}
            if reference in raster_recipe_ids:
                identifiers = raster_recipe_ids[reference]
                choice['recipe_ids'] = sorted(identifiers)
                selected = [recipe for recipe in selected if recipe['state'].get('id') in identifiers]
            choice['profiles'] = profiles_of(selected)
            choices.append(choice)
    choices.sort(key=lambda choice: (choice['configuration'], choice['node'], choice['technique']))

    technique_records = []
    for technique in sorted(techniques):
        users = [choice_reference(choice) for choice in choices if choice['technique'] == technique]
        technique_records.append({'id': technique, 'routed': bool(users), 'choices': users,
                                  'profiles': profiles_of(recipes_by_technique.get(technique, []))})

    programs_by_key = {}
    for (program, _), key in entries.items():
        programs_by_key.setdefault(key, set()).add(program)
    users_by_key = {}
    for choice in choices:
        for profile in choice['profiles'].values():
            for key in profile['stages'].values():
                users = users_by_key.setdefault(key, [])
                if choice_reference(choice) not in users:
                    users.append(choice_reference(choice))
    techniques_by_key = {}
    for record in technique_records:
        for profile in record['profiles'].values():
            for key in profile['stages'].values():
                techniques_by_key.setdefault(key, set()).add(record['id'])

    artifacts = {}
    for (program, profile), key in sorted(entries.items()):
        artifact = artifacts.get(key)
        if artifact is None:
            # The first request in build order decides whether this build compiled the key.
            artifact = artifacts[key] = {'key': key, 'profile': profile, 'stage': compiled[(program, profile)]['stage'],
                                         'programs': sorted(programs_by_key[key]), 'cache': requests[(program, profile)],
                                         'requests': 0, 'bytes': directory_bytes(artifact_directory(key)),
                                         'techniques': sorted(techniques_by_key.get(key, ())),
                                         'choices': users_by_key.get(key, [])}
            if key in compile_seconds:
                artifact['compile_seconds'] = compile_seconds[key]
        artifact['requests'] += 1

    recipe_records = {}
    for recipe in recipes:
        record = recipe_records.setdefault(recipe['key'], {
            'key': recipe['key'], 'technique': recipe['technique'], 'profile': recipe['profile'],
            'recipe': recipe['state'].get('id'), 'stages': recipe_stage_keys(recipe), 'choices': []})
        for choice in choices:
            if recipe['key'] in choice['profiles'].get(recipe['profile'], {}).get('recipes', ()):
                if choice_reference(choice) not in record['choices']:
                    record['choices'].append(choice_reference(choice))

    emitted = {program for program, _ in entries}
    outcomes = list(requests.values())
    counts = {
        'authored_choices': len(choices),
        'techniques': len(technique_records),
        'unrouted_techniques': sum(1 for record in technique_records if record['routed'] is False),
        'stage_requests': len(entries),
        'unique_artifacts': len(artifacts),
        'compiled': outcomes.count('miss'),
        'cache_hits': outcomes.count('hit'),
        'build_reuses': outcomes.count('reuse'),
        'recipe_keys': len(recipe_records),
        'artifact_bytes': sum(artifact['bytes'] for artifact in artifacts.values()),
    }
    return {'counts': counts, 'choices': choices, 'techniques': technique_records,
            'artifacts': [artifacts[key] for key in sorted(artifacts)],
            'recipes': [recipe_records[key] for key in sorted(recipe_records)],
            'programs_without_artifacts': sorted(set(program_ids) - emitted)}
