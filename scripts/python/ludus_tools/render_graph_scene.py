"""Scene bundle ports and checked projections of one immutable scene generation."""

PARTS = ("geometry", "materials", "lights")
TYPES = {part: "Scene" + part.title() for part in PARTS}
ALL_DATA = (1 << len(PARTS)) - 1


def ports(inputs):
    result = dict(inputs)
    for identity, kind in inputs.items():
        if kind == "Scene":
            result.update({identity + "_" + part: TYPES[part] for part in PARTS})
    return result


def check_connections(node, inputs, where, graph):
    """Check aggregate/member exclusion even in an unexecuted draft."""
    for port in node['inputs']:
        if port['type'] == 'Scene':
            identity = port['id']
            members = any(identity + '_' + part in inputs for part in PARTS)
            graph._require(not (identity in inputs and members), "MultipleWriters", where,
                           "Use either the full Scene connection or its expanded members, not both.")


def resolve(node, inputs, where, graph):
    """Resolve either a full bundle or compatible members; geometry is mandatory."""
    result = dict(inputs)
    for port in node["inputs"]:
        if port["type"] != "Scene":
            continue
        identity = port["id"]
        members = {part: inputs[identity + "_" + part] for part in PARTS if identity + "_" + part in inputs}
        graph._require(not (identity in inputs and members), "MultipleWriters", where,
                       "Use either the full Scene connection or its expanded members, not both.")
        if not members:
            continue
        graph._require("geometry" in members, "UnboundInput", where,
                       "Expanded Scene requires Geometry (vertices, indices and transforms) for rendering.")
        origins = {value.origin for value in members.values()}
        graph._require(len(origins) == 1, "SceneBundle", where,
                       "Scene members must come from the same Scene generation; material/instance indices cannot be mixed.")
        result[identity] = graph._Value("Scene", origin=members["geometry"].origin,
                                       scene_data=sum(1 << PARTS.index(part) for part in members))
    return result
