import sys
import unittest
from dynd import nd, ndt

class TestFields(unittest.TestCase):
    def test_simple(self):
        a = nd.ndobject([
                (1, 2, 'a', 'b'),
                (3, 4, 'ab', 'cd'),
                (5, 6, 'def', 'ghi')],
                udtype='{x: int32; y: int32; z: string; w: string}')
        # Selecting a single field
        b = nd.fields(a, 'x')
        self.assertEqual(b.udtype, ndt.make_struct_dtype(
                        [ndt.int32],
                        ['x']))
        self.assertEqual(nd.as_py(b.x), nd.as_py(a.x))
        # Selecting two fields
        b = nd.fields(a, 'z', 'y')
        self.assertEqual(b.udtype, ndt.make_struct_dtype(
                        [ndt.string, ndt.int32],
                        ['z', 'y']))
        self.assertEqual(nd.as_py(b.z), nd.as_py(a.z))
        self.assertEqual(nd.as_py(b.y), nd.as_py(a.y))
        # Selecting three fields
        b = nd.fields(a, 'w', 'y', 'z')
        self.assertEqual(b.udtype, ndt.make_struct_dtype(
                        [ndt.string, ndt.int32, ndt.string],
                        ['w', 'y', 'z']))
        self.assertEqual(nd.as_py(b.w), nd.as_py(a.w))
        self.assertEqual(nd.as_py(b.y), nd.as_py(a.y))
        self.assertEqual(nd.as_py(b.z), nd.as_py(a.z))
        # Reordering all four fields
        b = nd.fields(a, 'w', 'y', 'x', 'z')
        self.assertEqual(b.udtype, ndt.make_struct_dtype(
                        [ndt.string, ndt.int32, ndt.int32, ndt.string],
                        ['w', 'y', 'x', 'z']))
        self.assertEqual(nd.as_py(b.w), nd.as_py(a.w))
        self.assertEqual(nd.as_py(b.y), nd.as_py(a.y))
        self.assertEqual(nd.as_py(b.x), nd.as_py(a.x))
        self.assertEqual(nd.as_py(b.z), nd.as_py(a.z))

    def test_bad_field_name(self):
        a = nd.ndobject([
                (1, 2, 'a', 'b'),
                (3, 4, 'ab', 'cd'),
                (5, 6, 'def', 'ghi')],
                udtype='{x: int32; y: int32; z: string; w: string}')
        self.assertRaises(RuntimeError, nd.fields, a, 'y', 'v')

if __name__ == '__main__':
    unittest.main()