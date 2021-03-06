#!/usr/bin/env python

import os
import unittest
import gemmi
from common import full_path, get_path_for_tempfile
try:
    import numpy
except ImportError:
    numpy = None

def compare_maps(self, a, b, atol):
    #print(abs(numpy.array(a) - b).max())
    self.assertTrue(numpy.allclose(a, b, atol=atol, rtol=0))

def compare_asu_data(self, asu_data, data, f, phi):
    asu_dict = {tuple(a.hkl): a.value for a in asu_data}
    data_hkl = data.make_miller_array()
    self.assertEqual(data_hkl.dtype, 'int32')
    if type(data) is gemmi.ReflnBlock:
        data_f = data.make_float_array(f)
        data_phi = data.make_float_array(phi)
    else:
        data_f = data.column_with_label(f).array
        data_phi = data.column_with_label(phi).array
    self.assertTrue(len(data_f) > 100)
    asu_val = numpy.array([asu_dict[tuple(hkl)] for hkl in data_hkl])
    self.assertTrue(numpy.allclose(data_f, abs(asu_val), atol=5e-5, rtol=0))
    x180 = abs(180 - abs(data_phi - numpy.angle(asu_val, deg=True)))
    self.assertTrue(numpy.allclose(x180, 180, atol=2e-2, rtol=0.))

def fft_test(self, data, f, phi, size, order=gemmi.AxisOrder.XYZ):
    if numpy is None:
        return
    self.assertTrue(data.data_fits_into(size))
    grid_full = data.get_f_phi_on_grid(f, phi, size, half_l=False, order=order)
    self.assertEqual(grid_full.axis_order, order)
    array_full = numpy.array(grid_full, copy=False)
    map1 = gemmi.transform_f_phi_grid_to_map(grid_full)
    self.assertEqual(map1.axis_order, order)
    map2 = numpy.fft.ifftn(array_full.conj())
    map2 = numpy.real(map2) * (map2.size / grid_full.unit_cell.volume)
    compare_maps(self, map1, map2, atol=6e-7)
    map3 = data.transform_f_phi_to_map(f, phi, size, order=order)
    compare_maps(self, map1, map3, atol=6e-7)

    grid2 = gemmi.transform_map_to_f_phi(map1, half_l=False)
    self.assertFalse(grid2.half_l)
    self.assertEqual(grid2.axis_order, order)
    compare_maps(self, grid2, array_full, atol=1e-4)
    if grid2.axis_order != gemmi.AxisOrder.ZYX:
        compare_asu_data(self, grid2.prepare_asu_data(), data, f, phi)
    grid_half = data.get_f_phi_on_grid(f, phi, size, half_l=True, order=order)
    if order == gemmi.AxisOrder.ZYX:  # half_l+ZYX not supported yet
        return
    grid3 = gemmi.transform_map_to_f_phi(map1, half_l=True)
    self.assertTrue(grid3.half_l)
    self.assertEqual(grid3.axis_order, order)
    compare_maps(self, grid3, grid_half, atol=1e-4)
    compare_asu_data(self, grid3.prepare_asu_data(), data, f, phi)


class TestMtz(unittest.TestCase):
    def test_read_write(self):
        path = full_path('5e5z.mtz')
        mtz = gemmi.read_mtz_file(path)
        self.assertEqual(mtz.spacegroup.hm, 'P 1 21 1')
        out_name = get_path_for_tempfile()
        mtz.write_to_file(out_name)
        mtz2 = gemmi.read_mtz_file(out_name)
        os.remove(out_name)
        self.assertEqual(mtz2.spacegroup.hm, 'P 1 21 1')

    def test_f_phi_grid(self):
        path = full_path('5wkd_phases.mtz.gz')
        mtz = gemmi.read_mtz_file(path)
        size = mtz.get_size_for_hkl()
        for half_l in (False, True):
            grid1 = mtz.get_f_phi_on_grid('FWT', 'PHWT', size, half_l=half_l)
            grid2 = mtz.get_f_phi_on_grid('FWT', 'PHWT', size, half_l=half_l,
                                          order=gemmi.AxisOrder.ZYX)
            if numpy is None:
                continue
            array1 = numpy.array(grid1, copy=False)
            array2 = numpy.array(grid2, copy=False)
            self.assertTrue((array2 == array1.transpose(2,1,0)).all())

        fft_test(self, mtz, 'FWT', 'PHWT', size)
        fft_test(self, mtz, 'FWT', 'PHWT', size, order=gemmi.AxisOrder.ZYX)

    def test_value_grid(self):
        #path = full_path('5wkd_phases.mtz.gz')
        path = full_path('5e5z.mtz')
        mtz = gemmi.read_mtz_file(path)
        size = mtz.get_size_for_hkl()
        if numpy is None:
            return
        asu = gemmi.ReciprocalAsu(mtz.spacegroup)
        mtz_data = numpy.array(mtz, copy=False)
        fp_idx = mtz.column_labels().index('FP')
        fp_map = {}
        for row in mtz_data:
            fp_map[tuple(row[0:3])] = row[fp_idx]
        for order in (gemmi.AxisOrder.XYZ, gemmi.AxisOrder.ZYX):
            for half_l in (True, False):
                grid = mtz.get_value_on_grid('FP', size,
                                             half_l=half_l, order=order)
                counter = 0
                for point in grid:
                    hkl = grid.to_hkl(point)
                    value = fp_map.get(tuple(hkl))
                    if asu.is_in(hkl):
                        if value is not None:
                            self.assertTrue(point.value == value
                                            or (numpy.isnan(point.value)
                                                and numpy.isnan(value)))
                            counter += 1
                        else:
                            self.assertEqual(point.value, 0.)
                    else:
                        self.assertIsNone(value)
                self.assertEqual(counter, mtz_data.shape[0])


class TestSfMmcif(unittest.TestCase):
    def test_reading(self):
        doc = gemmi.cif.read(full_path('r5wkdsf.ent'))
        rblock = gemmi.as_refln_blocks(doc)[0]
        self.assertEqual(rblock.spacegroup.hm, 'C 1 2 1')

        size = rblock.get_size_for_hkl()
        for order in (gemmi.AxisOrder.XYZ, gemmi.AxisOrder.ZYX):
            fft_test(self, rblock, 'pdbx_FWT', 'pdbx_PHWT', size)

if __name__ == '__main__':
    unittest.main()
