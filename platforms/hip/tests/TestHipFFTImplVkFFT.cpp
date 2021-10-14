/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2011-2016 Stanford University and the Authors.      *
 * Portions copyright (C) 2021 Advanced Micro Devices, Inc. All Rights        *
 * Reserved.                                                                  *
 * Authors: Peter Eastman, Nicholas Curtis                                    *
 * Contributors:                                                              *
 * Portions Copyright© 2021 Advanced Micro Devices, Inc. All rights reserved  *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

/**
 * This tests the VkFFT-based implementation of FFT.
 */

#include "openmm/internal/AssertionUtilities.h"
#include "HipArray.h"
#include "HipContext.h"
#include "HipFFTImplVkFFT.h"
#include "fftpack.h"
#include "sfmt/SFMT.h"
#include "openmm/System.h"
#include <iostream>
#include <cmath>
#include <set>

using namespace OpenMM;
using namespace std;

static HipPlatform platform;

template <class Real2>
void testTransform(bool realToComplex, int xsize, int ysize, int zsize, double eps = 1) {
    System system;
    system.addParticle(0.0);
    HipPlatform::PlatformData platformData(NULL, system, "", "true", platform.getPropertyDefaultValue("HipPrecision"), "false",
            platform.getPropertyDefaultValue(HipPlatform::HipCompiler()), platform.getPropertyDefaultValue(HipPlatform::HipTempDirectory()),
            platform.getPropertyDefaultValue(HipPlatform::HipHostCompiler()), platform.getPropertyDefaultValue(HipPlatform::HipDisablePmeStream()), "false", true, 1, NULL);
    HipContext& context = *platformData.contexts[0];
    context.initialize();
    context.setAsCurrent();
    OpenMM_SFMT::SFMT sfmt;
    init_gen_rand(0, sfmt);
    vector<Real2> original(xsize*ysize*zsize);
    vector<t_complex> reference(original.size());
    for (int i = 0; i < (int) original.size(); i++) {
        Real2 value;
        value.x = (float) genrand_real2(sfmt);
        value.y = (float) genrand_real2(sfmt);
        original[i] = value;
        reference[i] = t_complex(value.x, value.y);
    }
    for (int i = 0; i < (int) reference.size(); i++) {
        if (realToComplex)
            reference[i] = t_complex(i%2 == 0 ? original[i/2].x : original[i/2].y, 0);
        else
            reference[i] = t_complex(original[i].x, original[i].y);
    }
    HipArray grid1(context, original.size(), sizeof(Real2), "grid1");
    HipArray grid2(context, original.size(), sizeof(Real2), "grid2");
    grid1.upload(original);
    HipFFTImplVkFFT fft(context, xsize, ysize, zsize, realToComplex, context.getCurrentStream(), grid1, grid2);

    // Perform a forward FFT, then verify the result is correct.

    fft.execFFT(true);
    vector<Real2> result;
    grid2.download(result);
    fftpack_t plan;
    fftpack_init_3d(&plan, xsize, ysize, zsize);
    fftpack_exec_3d(plan, FFTPACK_FORWARD, &reference[0], &reference[0]);
    int outputZSize = (realToComplex ? zsize/2+1 : zsize);
    for (int x = 0; x < xsize; x++)
        for (int y = 0; y < ysize; y++)
            for (int z = 0; z < outputZSize; z++) {
                int index1 = x*ysize*zsize + y*zsize + z;
                int index2 = x*ysize*outputZSize + y*outputZSize + z;
                ASSERT_EQUAL_TOL(reference[index1].re, result[index2].x, 1e-3 * eps);
                ASSERT_EQUAL_TOL(reference[index1].im, result[index2].y, 1e-3 * eps);
            }
    fftpack_destroy(plan);

    // Perform a backward transform and see if we get the original values.

    fft.execFFT(false);
    grid1.download(result);
    double scale = 1.0/(xsize*ysize*zsize);
    int valuesToCheck = (realToComplex ? original.size()/2 : original.size());
    for (int i = 0; i < valuesToCheck; ++i) {
        ASSERT_EQUAL_TOL(original[i].x, scale*result[i].x, 1e-4 * eps);
        ASSERT_EQUAL_TOL(original[i].y, scale*result[i].y, 1e-4 * eps);
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc > 1)
            platform.setPropertyDefaultValue("HipPrecision", string(argv[1]));
        if (platform.getPropertyDefaultValue("HipPrecision") == "double") {
            testTransform<double2>(false, 28, 25, 30);
            testTransform<double2>(true, 28, 25, 25);
            testTransform<double2>(true, 25, 28, 25);
            testTransform<double2>(true, 25, 25, 28);
            testTransform<double2>(true, 21, 25, 27);
            testTransform<double2>(true, 49, 98, 14);
            testTransform<double2>(true, 7, 21, 98);
            testTransform<double2>(true, 98, 21, 21);
            testTransform<double2>(true, 18, 98, 6);
            testTransform<double2>(true, 98, 98, 98, 1e+1);
        }
        else {
            testTransform<float2>(false, 28, 25, 30);
            testTransform<float2>(true, 28, 25, 25);
            testTransform<float2>(true, 25, 28, 25);
            testTransform<float2>(true, 25, 25, 28);
            testTransform<float2>(true, 21, 25, 27);
            testTransform<float2>(true, 49, 98, 14);
            testTransform<float2>(true, 7, 21, 98);
            testTransform<float2>(true, 98, 21, 21);
            testTransform<float2>(true, 18, 98, 6);
            testTransform<float2>(true, 98, 98, 98, 1e+1);
        }
    }
    catch(const exception& e) {
        cout << "exception: " << e.what() << endl;
        return 1;
    }
    cout << "Done" << endl;
    return 0;
}
