/*
Copyright (c) 2015 - present Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "CUDA2DPP.h"

// Maps CUDA header names to DPP header names
const std::map <llvm::StringRef, dppCounter> CUDA_INCLUDE_MAP {
  // CUDA includes
  {"cuda.h",                                                {"hip/hip_runtime.h",                                     "",                                                               CONV_INCLUDE_CUDA_MAIN_H,    API_DRIVER, 0}},
  {"cuda_runtime.h",                                        {"hip/hip_runtime.h",                                     "",                                                               CONV_INCLUDE_CUDA_MAIN_H,    API_RUNTIME, 0}},
};

const std::map<llvm::StringRef, dppCounter> &CUDA_RENAMES_MAP() {
  static std::map<llvm::StringRef, dppCounter> ret;
  if (!ret.empty())
    return ret;
  // First run, so compute the union map.
  // TODO: We will keep on adding to this union map
  ret.insert(CUDA_RUNTIME_FUNCTION_MAP.begin(), CUDA_RUNTIME_FUNCTION_MAP.end());
  return ret;
};
