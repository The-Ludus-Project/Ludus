"""Regressions for per-target portable raster reflection and browser packing."""
import copy
import importlib.util
from pathlib import Path
import sys
import struct
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'cmake/shaders'))
from compile_raster import spirv_array_indexing, verify_native_metal_arrays, raster_interface, wgsl_contract, glsl_interface, glsl_varyings, verify_varyings, uniform_shape, wgsl_uniform_type, external_text_source

class RasterReflectionTests(unittest.TestCase):
    def test_native_ray_scene_is_scalar_compute_only(self):
        document = {'parameters': [{'name': 'Scene', 'binding': {'kind': 'descriptorTableSlot', 'index': 5},
                                    'type': {'kind': 'resource', 'baseShape': 'accelerationStructure'}}],
                    'entryPoints': [{'stage': 'compute', 'threadGroupSize': [64, 1, 1],
                                     'bindings': [{'name': 'Scene', 'binding': {'used': 1}}]}]}
        interface = raster_interface(document, 'compute')
        self.assertEqual(interface['entries'], [{'binding': 5, 'kind': 'RayScene', 'size': 0, 'name': 'Scene'}])
        with self.assertRaises(RuntimeError):
            raster_interface(document, 'fragment')
        document['parameters'][0]['type'] = {'kind': 'array', 'elementCount': 2,
                                           'elementType': {'kind': 'resource', 'baseShape': 'accelerationStructure'}}
        with self.assertRaises(RuntimeError):
            raster_interface(document, 'compute', native_arrays=True)

    def test_binding_slot_limit_matches_the_public_rhi_contract(self):
        import re
        from compile_raster import RASTER_BINDING_COUNT, RUNTIME_BINDING_LENGTH_SLOT, RUNTIME_BINDING_ARRAY_SLOT
        source = (ROOT/'modules/graphics/rhi/include/ludus/graphics/rhi/profile.h').read_text()
        match = re.search(r'RASTER_BINDING_COUNT\s*=\s*(\d+)', source)
        self.assertIsNotNone(match)
        self.assertEqual(RASTER_BINDING_COUNT, int(match.group(1)))
        self.assertEqual(RUNTIME_BINDING_LENGTH_SLOT, RASTER_BINDING_COUNT - 2)
        self.assertEqual(RUNTIME_BINDING_ARRAY_SLOT, RASTER_BINDING_COUNT - 1)
        self.assertIn('RUNTIME_BINDING_LENGTH_SLOT = RASTER_BINDING_COUNT - 2', source)
        self.assertIn('RUNTIME_BINDING_ARRAY_SLOT = RASTER_BINDING_COUNT - 1', source)
        document = self.fixture()
        document['parameters'][0]['binding']['index'] = RASTER_BINDING_COUNT - 1
        result = raster_interface(document, 'fragment')
        self.assertEqual(result['entries'][0]['binding'], RASTER_BINDING_COUNT - 1)

    def test_external_large_shader_preserves_exact_utf8_and_terminator(self):
        import re
        text = "// café\n" + "void Compute() {}\n" * 5000
        declaration, definition = external_text_source("large_shader", "METAL_COMPUTE", text)
        body = definition.split("= {\n", 1)[1].split("\n};", 1)[0]
        values = [int(value) for value in re.findall(r"\d+", body)]
        self.assertEqual(bytes(values), text.encode("utf-8") + b"\0")
        self.assertIn(f"[{len(values)}]", declaration)
        self.assertNotIn('R"', definition)

    def test_native_arrays_require_bounded_counts_and_preserve_target_identity(self):
        texture = {'kind': 'resource', 'baseShape': 'texture2D', 'resultType': {'kind': 'vector', 'elementType': {'scalarType': 'float32'}}}
        document = {'parameters': [{'name': 'Images', 'binding': {'kind': 'descriptorTableSlot', 'index': 1}, 'type': {'kind': 'array', 'elementCount': 72, 'elementType': texture}}],
                    'entryPoints': [{'stage': 'fragment', 'bindings': [{'name': 'Images', 'binding': {'used': 1}}]}]}
        value = raster_interface(document, 'fragment', native_arrays=True)
        self.assertEqual(value['entries'][0]['count'], 72)
        with self.assertRaisesRegex(RuntimeError, 'native binding array profile'):
            raster_interface(document, 'fragment')
        for count in (0, None, -1, 0xffffffff):
            invalid = copy.deepcopy(document)
            invalid['parameters'][0]['type']['elementCount'] = count
            with self.assertRaises(RuntimeError):
                raster_interface(invalid, 'fragment', native_arrays=True)
        for shape in ('structuredBuffer',):
            invalid = copy.deepcopy(document)
            invalid['parameters'][0]['type']['elementType']['baseShape'] = shape
            with self.assertRaises(RuntimeError):
                raster_interface(invalid, 'fragment', native_arrays=True)
        code = 'fragment float4 Main(array<texture2d<float>, 72> Images [[texture(1)]]) {}'
        verify_native_metal_arrays(code, value)
        for invalid in (code.replace('72', '71'), code.replace('texture(1)', 'texture(2)')):
            with self.assertRaises(RuntimeError):
                verify_native_metal_arrays(invalid, value)

    def test_runtime_texture_arrays_require_the_owned_length_profile(self):
        from compile_raster import RUNTIME_BINDING_ARRAY_SLOT
        texture = {'kind': 'resource', 'baseShape': 'textureCube', 'resultType': {'kind': 'vector', 'elementType': {'scalarType': 'float32'}}}
        document = {'parameters': [{'name': 'Images', 'binding': {'kind': 'descriptorTableSlot', 'index': RUNTIME_BINDING_ARRAY_SLOT}, 'type': {'kind': 'array', 'elementCount': 0, 'elementType': texture}}],
                    'entryPoints': [{'stage': 'fragment', 'bindings': [{'name': 'Images', 'binding': {'used': 1}}]}]}
        result = raster_interface(document, 'fragment', native_arrays=True, runtime_arrays=True)
        self.assertEqual(result['entries'][0]['kind'], 'TextureCube')
        self.assertEqual(result['entries'][0]['count'], 1)
        self.assertTrue(result['entries'][0]['runtime_sized'])
        with self.assertRaises(RuntimeError):
            raster_interface(document, 'fragment', native_arrays=True)
        with self.assertRaises(RuntimeError):
            raster_interface(document, 'fragment', metal=True, native_arrays=True, runtime_arrays=True)
        document['parameters'][0]['binding']['index'] = 3
        with self.assertRaises(RuntimeError):
            raster_interface(document, 'fragment', native_arrays=True, runtime_arrays=True)

    def test_runtime_bounds_use_the_owned_uint4_abi_and_first_component(self):
        from compile_raster import RUNTIME_BINDING_ARRAY_SLOT, RUNTIME_BINDING_LENGTH_SLOT
        def instruction(operation, *values):
            return [(len(values) + 1) << 16 | operation, *values]
        def target(offset=0, width=4, component=0, subtract=1, bound=True):
            words = [0x07230203, 0x00010300, 0, 1000, 0]
            words += instruction(21, 2, 32, 0)
            words += instruction(23, 3, 2, width)
            words += instruction(30, 4, 3)
            words += instruction(72, 4, 0, 35, offset)
            words += instruction(32, 5, 2, 4)
            words += instruction(32, 6, 2, 2)
            words += instruction(32, 20, 0, 21)
            words += instruction(59, 5, 10, 2)
            words += instruction(59, 20, 1, 0)
            words += instruction(71, 10, 33, RUNTIME_BINDING_LENGTH_SLOT)
            words += instruction(71, 1, 33, RUNTIME_BINDING_ARRAY_SLOT)
            words += instruction(43, 2, 11, 0)
            words += instruction(43, 2, 12, subtract)
            words += instruction(43, 2, 19, component)
            words += instruction(65, 6, 13, 10, 11, 19)
            words += instruction(61, 2, 14, 13)
            words += instruction(130, 2, 15, 14, 12)
            name = b'GLSL.std.450\0'
            name += b'\0' * (-len(name) % 4)
            words += instruction(11, 30, *struct.unpack('<' + 'I' * (len(name) // 4), name))
            words += instruction(12, 2, 16, 30, 38, 999, 15)
            words += instruction(65, 40, 18, 1, 16 if bound else 999)
            return struct.pack('<' + 'I' * len(words), *words)
        def reflection():
            return {'entries': [{'binding': RUNTIME_BINDING_LENGTH_SLOT, 'kind': 'UniformBuffer', 'size': 16},
                                {'binding': RUNTIME_BINDING_ARRAY_SLOT, 'kind': 'Texture2D', 'count': 1, 'runtime_sized': True}]}
        result = reflection()
        spirv_array_indexing(target(), result)
        self.assertTrue(result['entries'][1]['verified_bounds'])
        for changes in ({'offset': 4}, {'width': 3}, {'component': 1}, {'subtract': 0}, {'bound': False}):
            with self.assertRaises(RuntimeError):
                spirv_array_indexing(target(**changes), reflection())

    def test_native_sampler_and_volume_arrays_verify_emitted_metal_registers(self):
        for kind, declaration, register in (('Sampler', 'sampler', 'sampler'), ('Texture3D', 'texture3d<float>', 'texture'), ('Texture2DArray', 'texture2d_array<float>', 'texture'), ('TextureCube', 'texturecube<float>', 'texture')):
            interface = {'entries': [{'binding': 2, 'count': 4, 'kind': kind}]}
            code = f'fragment float4 Main(array<{declaration}, 4> Values [[{register}(2)]]) {{}}'
            verify_native_metal_arrays(code, interface)
            with self.assertRaises(RuntimeError):
                verify_native_metal_arrays(code.replace(f'{register}(2)', f'{register}(3)'), interface)

    def test_native_indexing_is_derived_and_unbounded_access_rejects(self):
        def instruction(operation, *values):
            return [(len(values) + 1) << 16 | operation, *values]
        def target(index=5, upper=3, non_uniform=False, bounded=True):
            words = [0x07230203, 0x00010300, 0, 1000, 0]
            words += instruction(71, 1, 33, 1)  # Images binding 1
            words += instruction(21, 2, 32, 0)  # uint
            words += instruction(43, 2, 4, upper)
            name = b'GLSL.std.450\0'
            name += b'\0' * (-len(name) % 4)
            words += instruction(11, 3, *struct.unpack('<' + 'I' * (len(name) // 4), name))
            if bounded:
                words += instruction(12, 2, 5, 3, 38, 999, 4)  # uint min(index, upper)
            words += instruction(65, 10, 9, 1, index)
            if non_uniform:
                words += instruction(71, 9, 5300)
            return struct.pack('<' + 'I' * len(words), *words)
        for index, bounded, non_uniform, expected in ((4, False, False, 'Constant'), (5, True, False, 'Uniform'), (5, True, True, 'NonUniform')):
            reflected = {'entries': [{'binding': 1, 'count': 4}]}
            spirv_array_indexing(target(index=index, bounded=bounded, non_uniform=non_uniform), reflected)
            self.assertEqual(reflected['entries'][0]['indexing'], expected)
            self.assertTrue(reflected['entries'][0]['verified_bounds'])
        for data in (target(upper=4), target(bounded=False), b'broken'):
            with self.assertRaises(RuntimeError):
                spirv_array_indexing(data, {'entries': [{'binding': 1, 'count': 4}]})

    def test_whole_array_load_cannot_hide_an_unproved_local_access(self):
        def instruction(operation, *values):
            return [(len(values) + 1) << 16 | operation, *values]
        # A valid direct constant access cannot excuse an additional whole-array
        # load/copy whose local descriptor lookup escaped the global access scan.
        words = [0x07230203, 0x00010300, 0, 100, 0]
        words += instruction(71, 1, 33, 1)
        words += instruction(21, 2, 32, 0)
        words += instruction(43, 2, 4, 0)
        words += instruction(32, 10, 0, 11)
        words += instruction(59, 10, 1, 0)
        words += instruction(65, 20, 9, 1, 4)
        words += instruction(61, 11, 12, 1)
        data = struct.pack('<' + 'I' * len(words), *words)
        with self.assertRaisesRegex(RuntimeError, 'pointer forwarding'):
            spirv_array_indexing(data, {'entries': [{'binding': 1, 'count': 4}]})

    def test_cluster_light_arrays_obey_uniform_byte_budget(self):
        vector = {'kind':'vector','elementCount':4,'elementType':{'kind':'scalar','scalarType':'float32'}}
        for count in (32,512,1024):
            self.assertEqual(uniform_shape({'kind':'array','elementCount':count,'elementType':vector}),'Float4Array')
        self.assertEqual(uniform_shape({'kind':'array','elementCount':1025,'elementType':vector}),'Opaque')
        matrix = {'kind':'matrix','rowCount':4,'columnCount':4,'elementType':{'kind':'scalar','scalarType':'float32'}}
        self.assertEqual(uniform_shape({'kind':'array','elementCount':256,'elementType':matrix}),'Float4x4Array')
        self.assertEqual(uniform_shape({'kind':'array','elementCount':257,'elementType':matrix}),'Opaque')
        self.assertEqual(wgsl_uniform_type('', 'array<vec4<f32>,512>'), (8192,16))
        with self.assertRaises(RuntimeError):
            wgsl_uniform_type('', 'array<mat4x4<f32>,257>')

    def test_fragment_varyings_may_be_a_typed_subset(self):
        verify_varyings([(0,'vec2'),(1,'vec3')],[(0,'vec2')])
        verify_varyings([(0,'vec2')],[])
        for fragment in [[(0,'vec3')],[(2,'vec2')],[(0,'vec2'),(0,'vec2')]]:
            with self.assertRaises(RuntimeError):
                verify_varyings([(0,'vec2'),(1,'vec3')],fragment)

    def fixture(self):
        scalar={'kind':'scalar','scalarType':'float32'}
        return {'parameters':[{'name':'settings','binding':{'kind':'descriptorTableSlot','index':0},
                  'type':{'kind':'constantBuffer','elementType':{'fields':[{'name':'color','binding':{'offset':0}}]},
                          'elementVarLayout':{'binding':{'size':16}}}}],
                'entryPoints':[{'stage':'fragment','bindings':[{'name':'settings','binding':{'used':1}}]}]}
    def test_r9_texture_dimensions_are_verified_per_emitted_target(self):
        for base, array, kind, wgsl, glsl in [('texture2D',False,'Texture2D','2d','sampler2D'),('texture2D',True,'Texture2DArray','2d_array','sampler2DArray'),('textureCube',False,'TextureCube','cube','samplerCube'),('texture3D',False,'Texture3D','3d','sampler3D')]:
            data={'parameters':[{'name':'image','binding':{'kind':'descriptorTableSlot','index':1},'type':{'kind':'resource','baseShape':base,'array':array,'resultType':{'kind':'vector','elementType':{'scalarType':'float32'}}}}], 'entryPoints':[{'stage':'fragment','bindings':[{'name':'image','binding':{'used':1}}]}]}
            value=raster_interface(data,'fragment')
            self.assertEqual(value['entries'][0]['kind'],kind)
            code=f'@binding(1) @group(0) var image_0 : texture_{wgsl}<f32>;'
            self.assertEqual(wgsl_contract(code,value,'fragment'),code)
            with self.assertRaisesRegex(RuntimeError,'dimension differs'):
                wgsl_contract(code.replace(f'texture_{wgsl}', 'texture_1d'),value,'fragment')
            cross={'separate_images':[{'name':'image','binding':1}],'separate_samplers':[{'name':'sampler','binding':2}]}
            emitted=f'#version 300 es\nuniform highp {glsl} SPIRV_Cross_Combinedimagesampler; void main() {{}}'
            self.assertEqual(glsl_interface(emitted,value,cross)[1][1]['sampler'],2)
            with self.assertRaisesRegex(RuntimeError,'dimension differs'):
                glsl_interface(emitted.replace(glsl,'samplerCube' if glsl!='samplerCube' else 'sampler2D'),value,cross)
            data['parameters'][0]['type']['multisample']=True
            with self.assertRaises(RuntimeError): raster_interface(data,'fragment')

    def test_r10_scalar_aspects_verify_type_and_reject_filtered_sampling(self):
        for scalar, kind, wgsl, sampler in [('float32','TextureDepth2D','f32','sampler2D'),('uint32','TextureStencil2D','u32','usampler2D')]:
            data={'parameters':[{'name':'image','binding':{'kind':'descriptorTableSlot','index':2},'type':{'kind':'resource','baseShape':'texture2D','resultType':{'kind':'scalar','scalarType':scalar}}}], 'entryPoints':[{'stage':'fragment','bindings':[{'name':'image','binding':{'used':1}}]}]}
            value=raster_interface(data,'fragment')
            self.assertEqual(value['entries'][0]['kind'],kind)
            code=f'@binding(2) @group(0) var image_0 : texture_2d<{wgsl}>;'
            self.assertEqual(wgsl_contract(code,value,'fragment'),code)
            with self.assertRaisesRegex(RuntimeError,'dimension differs'):
                wgsl_contract(code.replace(wgsl,'i32'),value,'fragment')
            with self.assertRaisesRegex(RuntimeError,'unfiltered loads'):
                wgsl_contract(code+' textureSampleLevel(image_0, sampler_0, uv, 0);',value,'fragment')
            cross={'separate_images':[{'name':'image','binding':2}]}
            emitted=f'#version 300 es\nuniform highp {sampler} SPIRV_Cross_CombinedimageSPIRV_Cross_DummySampler; void main() {{}}'
            self.assertEqual(glsl_interface(emitted,value,cross)[1][2]['sampler'],0xffffffff)
            data['parameters'][0]['type']['array']=True
            with self.assertRaisesRegex(RuntimeError,'2D aspect view'):raster_interface(data,'fragment')

    def test_missing_stage_usage_is_rejected(self):
        data=self.fixture();data['entryPoints'][0]['bindings'][0]['binding']={}
        with self.assertRaisesRegex(RuntimeError,'compile stages separately'):raster_interface(data,'fragment')
    def test_stage_usage_and_descriptor_identity(self):
        data=self.fixture();self.assertEqual(raster_interface(data,'fragment')['entries'][0]['offsets'],{'color':0})
        for change in ({'space':1},{'index':16}):
            invalid=copy.deepcopy(data);invalid['parameters'][0]['binding'].update(change)
            with self.assertRaises(RuntimeError):raster_interface(invalid,'fragment')
        data['entryPoints'][0]['bindings'][0]['binding']['used']=0
        self.assertEqual(raster_interface(data,'fragment')['entries'],[])
    def test_equal_total_size_does_not_hide_wrong_member_offsets(self):
        value=raster_interface(self.fixture(),'fragment')
        code='@binding(0) @group(0) var<uniform> settings_0 : Tint;\nstruct Tint { @align(16) color_0 : vec4<f32>, };'
        self.assertEqual(wgsl_contract(code,value,'fragment'),code)
        wrong=copy.deepcopy(value);wrong['entries'][0]['offsets']['color']=4
        with self.assertRaisesRegex(RuntimeError,'packing differs'):wgsl_contract(code,wrong,'fragment')
        essl='#version 300 es\nlayout(std140) uniform Tint { highp vec4 color; } settings;\nvoid main() {}'
        reflection={'ubos':[{'binding':0,'name':'Tint'}]}
        self.assertEqual(glsl_interface(essl,value,reflection)[0],{0:'Tint'})
        with self.assertRaisesRegex(RuntimeError,'offsets differ'):glsl_interface(essl,wrong,reflection)
    def test_location_normalization_matches_named_input_and_leaves_outputs(self):
        value={'entries':[],'inputs':[{'name':'position','location':0,'format':'Float3'}, {'name':'uv','location':1,'format':'Float2'}]}
        code='struct Input { @location(1) position_0 : vec3<f32>, @location(0) uv_0 : vec2<f32>, };\nstruct Output { @location(3) uv_1 : vec2<f32>, };\n@vertex fn vertexMain(input : Input) -> Output {}'
        normalized=wgsl_contract(code,value,'vertex')
        self.assertIn('@location(0) position_0',normalized);self.assertIn('@location(1) uv_0',normalized)
        self.assertIn('@location(3) uv_1',normalized)
        with self.assertRaisesRegex(RuntimeError,'differs from reflection'):wgsl_contract(code.replace('uv_0','unknown_0'),value,'vertex')

    def test_essl_stage_names_are_linked_by_verified_location(self):
        vertex='out vec2 vertex_uv; void main() { vertex_uv=vec2(0); }'
        fragment='in vec2 input_uv; void main() { vec2 value=input_uv; }'
        a,iface_a=glsl_varyings(vertex,{'outputs':[{'name':'vertex.uv','type':'vec2','location':0}]},'vertex')
        b,iface_b=glsl_varyings(fragment,{'inputs':[{'name':'input.uv','type':'vec2','location':0}]},'fragment')
        self.assertEqual(iface_a,iface_b);self.assertIn('out vec2 ludusVarying0;',a);self.assertIn('in vec2 ludusVarying0;',b)
        with self.assertRaisesRegex(RuntimeError,'differs from location reflection'):
            glsl_varyings(vertex,{'outputs':[{'name':'absent','type':'vec2','location':0}]},'vertex')

    def compute_fixture(self):
        return {'parameters':[{'name':'records','binding':{'kind':'descriptorTableSlot','index':0},
                  'type':{'kind':'resource','baseShape':'structuredBuffer','access':'readWrite',
                          'resultType':{'kind':'vector','elementCount':2,
                                        'elementType':{'kind':'scalar','scalarType':'float32'}}}}],
                'entryPoints':[{'stage':'compute','threadGroupSize':[64,1,1],
                                'bindings':[{'name':'records','binding':{'used':1}}]}]}

    def test_compute_storage_access_stride_and_workgroup(self):
        data=self.compute_fixture()
        value=raster_interface(data,'compute')
        self.assertEqual(value['workgroup'],[64,1,1])
        self.assertEqual(value['entries'][0]['kind'],'StorageReadWrite')
        self.assertEqual(value['entries'][0]['size'],8)
        data['parameters'][0]['type']['access']='read'
        self.assertEqual(raster_interface(data,'compute')['entries'][0]['kind'],'StorageRead')
        for group in ([0,1,1],[257,1,1],[16,17,1],[1,1,65],[1,1],None):
            invalid=copy.deepcopy(data);invalid['entryPoints'][0]['threadGroupSize']=group
            with self.assertRaisesRegex(RuntimeError,'workgroup'):raster_interface(invalid,'compute')

    def test_fragment_storage_is_read_only_and_keeps_element_stride(self):
        data = self.compute_fixture()
        data['parameters'][0]['type']['access'] = 'read'
        data['entryPoints'][0]['stage'] = 'fragment'
        value = raster_interface(data, 'fragment')
        self.assertEqual(value['entries'][0]['kind'], 'StorageRead')
        self.assertEqual(value['entries'][0]['size'], 8)
        with self.assertRaises(RuntimeError):
            raster_interface(data, 'vertex')
        data['parameters'][0]['type']['access'] = 'readWrite'
        with self.assertRaises(RuntimeError):
            raster_interface(data, 'fragment')

    def test_compute_integer_uniforms_verify_wgsl_packing_without_widening_raster(self):
        from compile_raster import wgsl_contract, wgsl_uniform_type
        code = """struct Parameters_0 {
            @align(16) dimensions_0: vec4<u32>,
            @align(16) signed_0: vec4<i32>,
            sequence_0: u32,
            @align(16) exposure_0: vec4<f32>,
        }
        @binding(0) @group(0) var<uniform> parameters_0: Parameters_0;
        """
        reflected = {'inputs': [], 'entries': [{'binding': 0, 'kind': 'UniformBuffer',
            'size': 64, 'offsets': {'dimensions': 0, 'signed': 16, 'sequence': 32, 'exposure': 48}}]}
        self.assertEqual(wgsl_contract(code, reflected, 'compute'), code)
        with self.assertRaisesRegex(RuntimeError, 'require float'):
            wgsl_contract(code, reflected, 'fragment')
        for kind, layout in (('u32', (4,4)), ('vec2<i32>', (8,8)),
                             ('vec3<u32>', (12,16)), ('vec4<i32>', (16,16)),
                             ('array<vec4<u32>,4>', (64,16))):
            self.assertEqual(wgsl_uniform_type('', kind, compute=True), layout)
            with self.assertRaises(RuntimeError):
                wgsl_uniform_type('', kind)
        for kind in ('bool', 'f64', 'vec4<bool>', 'array<vec4<u32>,0>', 'array<vec4<u32>,1025>'):
            with self.assertRaises(RuntimeError):
                wgsl_uniform_type('', kind, compute=True)
        invalid = copy.deepcopy(reflected)
        invalid['entries'][0]['offsets']['sequence'] = 36
        with self.assertRaisesRegex(RuntimeError, 'packing differs'):
            wgsl_contract(code, invalid, 'compute')

    def test_compute_rejects_unportable_elements_and_extra_storage(self):
        data=self.compute_fixture()
        for change in ({'elementCount':3},{'elementType':{'kind':'scalar','scalarType':'float64'}},
                       {'kind':'struct','fields':[]}):
            invalid=copy.deepcopy(data);invalid['parameters'][0]['type']['resultType'].update(change)
            with self.assertRaisesRegex(RuntimeError,'scalar32'):raster_interface(invalid,'compute')
        for i in range(1,5):
            resource=copy.deepcopy(data['parameters'][0]);resource['name']=f'records{i}';resource['binding']['index']=i
            data['parameters'].append(resource)
            data['entryPoints'][0]['bindings'].append({'name':resource['name'],'binding':{'used':1}})
        self.assertEqual(len(raster_interface(data,'compute')['entries']), 5)
        for i in range(5,16):
            resource=copy.deepcopy(data['parameters'][0]);resource['name']=f'records{i}';resource['binding']['index']=i
            data['parameters'].append(resource)
            data['entryPoints'][0]['bindings'].append({'name':resource['name'],'binding':{'used':1}})
        self.assertEqual(len(raster_interface(data,'compute')['entries']), 16)
        data['parameters'][-1]['binding']['index'] = 16
        with self.assertRaisesRegex(RuntimeError,'bounded profile'):raster_interface(data,'compute')

if __name__=='__main__':unittest.main()

class MaterialUniformLayoutTests(unittest.TestCase):
    def test_matrix_and_fixed_vector_array_layouts_are_independent(self):
        from compile_shader import glsl_es_layout
        wgsl='@binding(0) @group(0) var<uniform> params : Data; struct Data { transform : mat4x4<f32>, colors : array<vec4<f32>, 2>, tint : vec4<f32>, };'
        layout={'entries':[{'binding':0,'kind':'UniformBuffer','size':112,'offsets':{'transform':0,'colors':64,'tint':96}}]}
        self.assertEqual(wgsl_contract(wgsl,layout,'fragment'),wgsl)
        glsl='layout(std140) uniform Data { mat4 transform; vec4 colors[2]; vec4 tint; } params;'
        derived=glsl_es_layout(glsl)
        self.assertEqual(derived['offsets'],layout['entries'][0]['offsets']); self.assertEqual(derived['size'],112)
        wrong=copy.deepcopy(layout); wrong['entries'][0]['offsets']['tint']=80
        with self.assertRaisesRegex(RuntimeError,'packing differs'):wgsl_contract(wgsl,wrong,'fragment')
        with self.assertRaisesRegex(RuntimeError,'bounded'):wgsl_contract(wgsl.replace('array<vec4<f32>, 2>','array<f32, 2>'),layout,'fragment')

    def test_emitted_row_major_mat4_wrapper_offsets_are_independent(self):
        from compile_shader import glsl_es_layout
        layout=glsl_es_layout('struct Matrices { highp mat4 data[2]; }; layout(std140) uniform Values { layout(row_major) mat4 Transform; vec4 Colors[2]; layout(row_major) Matrices Bones; vec4 Tint; };')
        self.assertEqual(layout['offsets'],{'Transform':0,'Colors':64,'Bones':96,'Tint':224})
        self.assertEqual(layout['size'],240)
        with self.assertRaises(RuntimeError):
            glsl_es_layout('layout(std140) uniform Values { layout(row_major) vec4 Tint; };')

    def test_pinned_slang_matrix_storage_wrappers_are_verified(self):
        code='struct _MatrixStorage_float4x4_ColMajorstd140_0 { @align(16) data_0 : array<vec4<f32>, i32(4)>, }; struct _Array_std140_matrixx3Cfloatx2C4x2C4x3E2_0 { @align(16) data_1 : array<_MatrixStorage_float4x4_ColMajorstd140_0, i32(2)>, }; @binding(0) @group(0) var<uniform> params : Data; struct Data { transform : _MatrixStorage_float4x4_ColMajorstd140_0, colors : array<vec4<f32>, i32(2)>, bones : _Array_std140_matrixx3Cfloatx2C4x2C4x3E2_0, tint : vec4<f32>, };'
        layout={'entries':[{'binding':0,'kind':'UniformBuffer','size':240,'offsets':{'transform':0,'colors':64,'bones':96,'tint':224}}]}
        self.assertEqual(wgsl_contract(code,layout,'fragment'),code)
        with self.assertRaisesRegex(RuntimeError,'exactly four'):
            wgsl_contract(code.replace('i32(4)','i32(3)'),layout,'fragment')

    def test_numeric_shapes_distinguish_float4_from_equal_sized_unsigned_vectors(self):
        from compile_raster import uniform_shape
        value={'kind':'vector','elementCount':4,'elementType':{'kind':'scalar','scalarType':'float32'}}
        self.assertEqual(uniform_shape(value),'Float4')
        value['elementType']['scalarType']='uint32'
        self.assertEqual(uniform_shape(value),'Opaque')

class NativeStorageRecordTests(unittest.TestCase):
    def test_records_require_native_profile_and_match_reflected_offsets(self):
        data = RasterReflectionTests.compute_fixture(self)
        fields = [{'name': 'Weight', 'type': {'kind': 'scalar', 'scalarType': 'float32'},
                   'binding': {'kind': 'uniform', 'offset': 0, 'size': 4}},
                  {'name': 'Sample', 'type': {'kind': 'vector', 'elementCount': 4,
                                            'elementType': {'kind': 'scalar', 'scalarType': 'uint32'}},
                   'binding': {'kind': 'uniform', 'offset': 16, 'size': 16}}]
        data['parameters'][0]['type']['resultType'] = {'kind': 'struct', 'name': 'Reservoir', 'fields': fields}
        with self.assertRaises(RuntimeError):
            raster_interface(data, 'compute')
        interface = raster_interface(data, 'compute', structured_records=True)
        self.assertEqual(32, interface['entries'][0]['size'])
        self.assertEqual('uint32', interface['entries'][0]['fields'][1]['scalar'])
        fields[1]['binding']['offset'] = 4
        with self.assertRaisesRegex(RuntimeError, 'differs from std430'):
            raster_interface(data, 'compute', structured_records=True)

    def test_binary_array_stride_must_match_the_reflected_element(self):
        from compile_raster import verify_storage_record_strides
        def instruction(opcode, *operands):
            return [((len(operands) + 1) << 16) | opcode, *operands]
        def binary(stride):
            words = [0x07230203, 0x00010300, 0, 10, 0]
            words += instruction(71, 5, 33, 6)  # variable binding
            words += instruction(71, 5, 34, 0)  # group zero
            words += instruction(71, 2, 6, stride)  # runtime array stride
            words += instruction(29, 2, 1)  # runtime array of a record
            words += instruction(30, 3, 2)  # block containing the array
            words += instruction(32, 4, 12, 3)  # StorageBuffer pointer to block
            words += instruction(59, 4, 5, 12)  # resource variable
            return struct.pack('<' + 'I' * len(words), *words)
        interface = {'entries': [{'binding': 6, 'kind': 'StorageReadWrite', 'size': 32, 'fields': []}]}
        verify_storage_record_strides(binary(32), interface)
        for invalid in (binary(16), binary(32)[:-1], b''):
            with self.assertRaises(RuntimeError):
                verify_storage_record_strides(invalid, interface)
