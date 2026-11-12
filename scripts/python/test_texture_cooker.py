"""Independent color, footprint, normal and bounded coverage mip regressions."""
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'cmake/shaders'))
from cook_texture import cook
class TextureCookerTests(unittest.TestCase):
    def test_srgb_average_is_linear_light_and_alpha_is_linear(self):
        source={'width':2,'height':2,'format':'Rgba8Srgb','semantic':'color','pixels':[255,255,255,255,0,0,0,255]*2}
        blob,levels=cook(source)
        self.assertEqual(levels[1]['offset'],16)
        self.assertEqual(blob[16:20],bytes([188,188,188,255]))
        source['format']='Rgba8Unorm'
        self.assertEqual(cook(source)[0][16:20],bytes([128,128,128,255]))
    def test_odd_edges_and_rg_padding_are_included(self):
        source={'width':3,'height':1,'format':'Rg8Unorm','semantic':'data','pixels':[0,0,0,0,255,255]}
        blob,levels=cook(source)
        self.assertEqual(blob,bytes([0,0,0,0,255,255,0,0,85,85,0,0]))
        self.assertEqual([(x['width'],x['height'],x['row_pitch']) for x in levels],[(3,1,8),(1,1,4)])
    def test_normals_are_renormalized_and_opposites_use_declared_axis(self):
        source={'width':2,'height':1,'format':'Rgba8Unorm','semantic':'normal','pixels':[255,128,128,255,128,255,128,255]}
        blob,levels=cook(source)
        self.assertTrue(216<=blob[8]<=219)
        self.assertTrue(216<=blob[9]<=219)
        self.assertEqual(blob[11],255)
        source['pixels']=[255,255,255,255,0,0,0,255]
        self.assertEqual(cook(source)[0][8:12],bytes([128,128,255,255]))
    def test_mask_coverage_uses_nearest_attainable_discrete_count(self):
        source={'width':4,'height':1,'format':'R8Unorm','semantic':'mask','cutoff':.5,'pixels':[255,255,255,0]}
        blob,levels=cook(source)
        self.assertEqual(levels[0]['next_coverage_error'],.25)
        self.assertEqual(len(blob),12)
        self.assertGreaterEqual(blob[8],128)
    def test_invalid_input_and_semantics_reject(self):
        source={'width':1,'height':1,'format':'Rgba8Srgb','semantic':'data','pixels':[1,2,3,4]}
        with self.assertRaises(ValueError):cook(source)
        source.update(format='R8Unorm',semantic='mask',pixels=[1],cutoff=0)
        with self.assertRaises(ValueError):cook(source)
        source.update(cutoff=.5,width=True)
        with self.assertRaises(ValueError):cook(source)

    def test_odd_resampling_preserves_area_and_straight_alpha_avoids_bleeding(self):
        source={'width':5,'height':1,'format':'R8Unorm','semantic':'data','pixels':[0,0,0,0,255]}
        blob,levels=cook(source)
        self.assertEqual(blob[levels[-1]['offset']],51)
        source={'width':2,'height':1,'format':'Rgba8Srgb','semantic':'color','pixels':[255,0,0,255,0,0,255,0]}
        self.assertEqual(cook(source)[0][8:12],bytes([255,0,0,128]))

    def test_complete_chain_budget_rejects_before_pixel_processing(self):
        with self.assertRaisesRegex(ValueError,'64 MiB'):
            cook({'width':4096,'height':4096,'format':'Rgba8Unorm','semantic':'color','pixels':[]})

    def test_bake_gutters_metadata_and_single_mip_are_verified(self):
        source={'width':4,'height':4,'format':'Rgba8Unorm','semantic':'data','mip_levels':1,'pixels':[128,64,32,255]*16,
                'bake':{'source_id':1,'scene_revision':2,'dependency_revision':3,'transform':[1,0,0,0,1,0,0,0,1,0,0,0],'scale':[2,2,2],'direct_light_id':9}}
        blob,levels=cook(source)
        self.assertEqual(len(levels),1)
        self.assertEqual(len(blob),64)
        source['pixels'][0]=255
        with self.assertRaisesRegex(ValueError,'gutters'):cook(source)
        source['pixels'][0]=128
        source['bake']['scene_revision']=0
        with self.assertRaisesRegex(ValueError,'uint64'):cook(source)
        source['bake']['scene_revision']=2
        source['mip_levels']=2
        with self.assertRaisesRegex(ValueError,'one clamped mip'):cook(source)

    def test_bake_gutters_check_each_edge_and_corner_of_a_nonsquare_chart(self):
        width, height, gutter = 9, 7, 2
        texel_bytes = 4
        pixels = []
        border_offsets = []
        for y in range(height):
            for x in range(width):
                interior_x = min(max(x, gutter), width - gutter - 1)
                interior_y = min(max(y, gutter), height - gutter - 1)
                if (x, y) != (interior_x, interior_y):
                    border_offsets.append(len(pixels))
                pixels.extend((interior_x * 41, interior_y * 53, 0, 255))
        source = {
            'width': width, 'height': height, 'format': 'Rgba8Unorm',
            'semantic': 'data', 'mip_levels': 1, 'pixels': pixels,
            'bake': {'source_id': 1, 'scene_revision': 2, 'dependency_revision': 3,
                     'transform': [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0],
                     'scale': [1, 1, 1], 'gutter': gutter},
        }
        for offset in border_offsets:
            with self.subTest(x=offset // texel_bytes % width, y=offset // texel_bytes // width):
                invalid_pixels = pixels.copy()
                invalid_pixels[offset] ^= 255
                with self.assertRaisesRegex(ValueError, 'gutters'):
                    cook(dict(source, pixels=invalid_pixels))
        center_offset = (height // 2 * width + width // 2) * texel_bytes
        pixels[center_offset] ^= 255
        blob, _ = cook(source)
        self.assertEqual(blob, bytes(pixels))

    def test_explicit_initialized_range_is_bounded(self):
        source={'width':4,'height':4,'format':'R8Unorm','semantic':'data','pixels':[0]*16,'mip_levels':1}
        self.assertEqual(len(cook(source)[1]),1)
        for value in (0,4,True,1.5):
            source['mip_levels']=value
            with self.assertRaises(ValueError):cook(source)

    def test_bake_numeric_boundaries_reject_before_header_generation(self):
        source={'width':4,'height':4,'format':'Rgba8Unorm','semantic':'data','mip_levels':1,'pixels':[255]*64,
                'bake':{'source_id':1,'scene_revision':1,'dependency_revision':1,'transform':[1,0,0,0,1,0,0,0,1,0,0,0],'scale':[1,1,1]}}
        for field,index in [('transform',9),('scale',0)]:
            for value in [1e-46,-1e-46,10 ** 1000]:
                source['bake'][field][index]=value
                with self.assertRaisesRegex(ValueError,'representable'):cook(source)
            source['bake'][field][index]=2 ** -149
            self.assertEqual(len(cook(source)[1]),1)
            source['bake'][field][index]=0

    def test_large_thin_image_is_cooked_without_device_clamping(self):
        source = {'width':8240,'height':1,'format':'R8Unorm','semantic':'data',
                  'mip_levels':1,'pixels':[127]*8240}
        blob, levels = cook(source)
        self.assertEqual(len(blob), 8240)
        self.assertEqual(levels[0]['width'], 8240)
        for dimension in (0, 32769, 2**32-1):
            source['width'] = dimension
            with self.assertRaisesRegex(ValueError, 'dimensions'):
                cook(source)
        source.update(width=8240, height=8200, format='Rgba8Unorm')
        with self.assertRaisesRegex(ValueError, '64 MiB'):
            cook(source)


class ExpandedUploadBudgetTests(unittest.TestCase):
    def test_explicit_budget_admits_large_footprint_before_texel_validation(self):
        source = {'width':8240,'height':8200,'format':'Rgba8Unorm','semantic':'color',
                  'mip_levels':1,'pixels':[], 'max_upload_bytes':512*1024*1024}
        with self.assertRaisesRegex(ValueError, 'complete tightly packed'):
            cook(source)
        for invalid in (0, -1, True, 512*1024*1024+1):
            source['max_upload_bytes'] = invalid
            with self.assertRaisesRegex(ValueError, 'integer in 1..512 MiB'):
                cook(source)
        source['max_upload_bytes'] = 64*1024*1024
        with self.assertRaisesRegex(ValueError, 'max_upload_bytes'):
            cook(source)
