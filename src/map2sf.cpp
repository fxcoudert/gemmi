// Copyright 2019 Global Phasing Ltd.
//
// Transform CCP4 map to map coefficient columns in MTZ file.

#include <stdio.h>
#include <cctype>             // for toupper
#include <cstdlib>            // for strtod
#ifndef GEMMI_ALL_IN_ONE
# define GEMMI_WRITE_IMPLEMENTATION 1
#endif
#include <gemmi/mtz.hpp>      // for Mtz
#include <gemmi/ccp4.hpp>     // for Ccp4
#include <gemmi/fourier.hpp>  // for transform_map_to_sf
#include <gemmi/gzread.hpp>   // for read_cif_gz
#include <gemmi/refln.hpp>    // for ReflnBlock
#include <gemmi/util.hpp>     // for fail, iends_with
#include <gemmi/gz.hpp>       // for MaybeGzipped

#define GEMMI_PROG map2sf
#include "options.h"

using gemmi::Mtz;
using options_type = std::vector<option::Option>;

enum OptionIndex { Base=4, Section, DMin, FType, PhiType };

static const option::Descriptor Usage[] = {
  { NoOp, 0, "", "", Arg::None,
    "Usage:\n  " EXE_NAME " [options] MAP_FILE OUTPUT_FILE COL_F COL_PH\n\n"
    "Writes map coefficients (amplitude and phase) of a map to OUTPUT_FILE.\n"
    "The output is MTZ if it has mtz extension, otherwise it is mmCIF.\n"
    "\nOptions:"},
  CommonUsage[Help],
  CommonUsage[Version],
  CommonUsage[Verbose],
  { Base, 0, "b", "base", Arg::Required,
    "  -b, --base=PATH  \tAdd new columns to the data from this file." },
  { Section, 0, "", "section", Arg::Required,
    "  --section=NAME  \tAdd new columns to this MTZ dataset or CIF block." },
  { DMin, 0, "", "dmin", Arg::Float,
    "  --dmin=D_MIN  \tResolution limit." },
  { FType, 0, "", "ftype", Arg::Char,
    "  --ftype=TYPE   \tMTZ amplitude column type (default: F)." },
  { PhiType, 0, "", "phitype", Arg::Char,
    "  --phitype=TYPE  \tMTZ phase column type (default: P)." },
  { 0, 0, 0, 0, 0, 0 }
};

inline float get_phase_for_mtz(std::complex<float> v) {
  float angle = (float) gemmi::deg(std::arg(v));
  return angle >= 0.f ? angle : angle + 360.f;
}

static void transform_map_to_sf(OptParser& p) {
  bool verbose = p.options[Verbose];
  const char* map_path = p.nonOption(0);
  const char* output_path = p.nonOption(1);
  const char* f_col = p.nonOption(2);
  const char* phi_col = p.nonOption(3);
  char f_type = p.options[FType] ? std::toupper(p.options[FType].arg[0]) : 'F';
  char phi_type = p.options[PhiType] ? std::toupper(p.options[PhiType].arg[0])
                                     : 'P';
  double min_1_d2 = 0;
  if (p.options[DMin]) {
    double dmin = std::strtod(p.options[DMin].arg, nullptr);
    min_1_d2 = 1. / (dmin * dmin);
  }
  if (verbose)
    fprintf(stderr, "Reading %s ...\n", map_path);
  gemmi::Ccp4<float> map;
  map.read_ccp4(gemmi::MaybeGzipped(map_path));
  map.setup(gemmi::GridSetup::Full, NAN);
  if (std::any_of(map.grid.data.begin(), map.grid.data.end(),
                  [](float x) { return std::isnan(x); }))
    gemmi::fail("Map does not cover all the ASU");
  if (verbose)
    fprintf(stderr, "Fourier transform of grid %d x %d x %d...\n",
            map.grid.nu, map.grid.nv, map.grid.nw);
  auto hkl = gemmi::transform_map_to_f_phi(map.grid, /*half_l=*/true);
  if (gemmi::iends_with(output_path, ".mtz")) {
    gemmi::Mtz mtz;
    if (p.options[Base]) {
      if (verbose)
        fprintf(stderr, "Reading %s ...\n", p.options[Base].arg);
      mtz = gemmi::read_mtz_file(p.options[Base].arg);
      int dataset_id = -1;
      if (p.options[Section]) {
        const char* ds_name = p.options[Section].arg;
        if (Mtz::Dataset* ds = mtz.dataset_with_name(ds_name))
          dataset_id = ds->id;
        else
          mtz.add_dataset(ds_name);
      }
      if (verbose)
        fprintf(stderr, "Copied columns: %s\n",
                gemmi::join_str(mtz.columns, ", ",
                                [](const Mtz::Column& c) { return c.label; }
                               ).c_str());
      mtz.add_column(f_col, f_type, dataset_id);
      int f_idx = mtz.columns.back().idx;
      mtz.add_column(phi_col, phi_type, dataset_id);
      mtz.expand_data_rows(2);
      size_t ncol = mtz.columns.size();
      for (int i = 0; i != mtz.nreflections; ++i) {
        int h = (int) mtz.data[i * ncol + 0];
        int k = (int) mtz.data[i * ncol + 1];
        int l = (int) mtz.data[i * ncol + 2];
        std::complex<float> v = hkl.get_value(h, k, l);
        mtz.data[i * ncol + f_idx] = (float) std::abs(v);
        mtz.data[i * ncol + f_idx + 1] = (float) gemmi::phase_in_angles(v);
      }
    } else {
      mtz.cell = map.grid.unit_cell;
      mtz.spacegroup = map.grid.spacegroup;
      mtz.sort_order = {{1, 2, 3, 0, 0}};
      mtz.add_dataset("HKL_base");
      mtz.add_column("H", 'H');
      mtz.add_column("K", 'H');
      mtz.add_column("L", 'H');
      mtz.add_dataset(p.options[Section] ? p.options[Section].arg : "unknown");
      mtz.add_column(f_col, f_type);
      mtz.add_column(phi_col, phi_type);
      int max_h = (hkl.nu - 1) / 2;
      int max_k = (hkl.nv - 1) / 2;
      int max_l = hkl.nw - 1;
      gemmi::ReciprocalAsuChecker asu(mtz.spacegroup);
      for (int h = -max_h; h < max_h + 1; ++h)
        for (int k = -max_k; k < max_k + 1; ++k)
          for (int l = 0; l < max_l + 1; ++l)
            if (asu.is_in({{h, k, l}}) &&
                (min_1_d2 == 0. ||
                 mtz.cell.calculate_1_d2(h, k, l) < min_1_d2) &&
                !(h == 0 && k == 0 && l == 0)) {
              std::complex<float> v = hkl.get_value_q(h >= 0 ? h : h + hkl.nu,
                                                      k >= 0 ? k : k + hkl.nv,
                                                      l);
              mtz.data.push_back((float) h);
              mtz.data.push_back((float) k);
              mtz.data.push_back((float) l);
              mtz.data.push_back(std::abs(v));
              mtz.data.push_back((float) gemmi::phase_in_angles(v));
              ++mtz.nreflections;
            }
    }
    if (verbose)
      fprintf(stderr, "Writing %s ...\n", output_path);
    mtz.write_to_file(output_path);
  } else {
    gemmi::fail("mmCIF support not implemented yet");
  }
}


int GEMMI_MAIN(int argc, char **argv) {
  OptParser p(EXE_NAME);
  p.simple_parse(argc, argv, Usage);
  p.require_positional_args(4);
  try {
    transform_map_to_sf(p);
  } catch (std::runtime_error& e) {
    fprintf(stderr, "ERROR: %s\n", e.what());
    return 1;
  }
  return 0;
}

// vim:sw=2:ts=2:et:path^=../include,../third_party
