/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HW_EMULATOR_CAMERA_ALIGNMENT_H
#define HW_EMULATOR_CAMERA_ALIGNMENT_H

namespace android {

// Align |value| to the next larger value that is divisible by |alignment|
// |alignment| has to be a power of 2.
inline int align(int value, int alignment) { return (value + alignment - 1) & (~(alignment - 1)); }

}  // namespace android

#endif  // HW_EMULATOR_CAMERA_ALIGNMENT_H
