/*
 * Copyright 2016 The Android Open Source Project
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

#define LOG_TAG "GrallocWrapper"


#include <log/log.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#include <sync/sync.h>
#pragma clang diagnostic pop

#include "Gralloc42.h"
#include "GrallocWrapper.h"

namespace android {

namespace GrallocWrapper {

static constexpr Error kTransactionError = Error::NO_RESOURCES;

Mapper::Mapper()
{
    mMapper = IMapper::getService();
    if (mMapper == nullptr || mMapper->isRemote()) {
        LOG_ALWAYS_FATAL("gralloc-mapper must be in passthrough mode");
    }
}

Error Mapper::createDescriptor(
        const V2_0::IMapper::BufferDescriptorInfo& descriptorInfo,
        V2_0::BufferDescriptor* outDescriptor) const
{
    V4_0::IMapper::BufferDescriptorInfo infoV4;
    translate(descriptorInfo, infoV4);

    Error error;
    auto ret = mMapper->createDescriptor(infoV4,
            [&](const auto& tmpError, const auto& tmpDescriptor)
            {
                error = tmpError;
                if (error != Error::NONE) {
                    return;
                }

                translate(tmpDescriptor, *outDescriptor);
            });

    return (ret.isOk()) ? error : kTransactionError;
}

Error Mapper::importBuffer(const hardware::hidl_handle& rawHandle,
        buffer_handle_t* outBufferHandle) const
{
    Error error;
    auto ret = mMapper->importBuffer(rawHandle,
            [&](const auto& tmpError, const auto& tmpBuffer)
            {
                error = tmpError;
                if (error != Error::NONE) {
                    return;
                }

                *outBufferHandle = static_cast<buffer_handle_t>(tmpBuffer);
            });

    return (ret.isOk()) ? error : kTransactionError;
}

void Mapper::freeBuffer(buffer_handle_t bufferHandle) const
{
    auto buffer = const_cast<native_handle_t*>(bufferHandle);
    auto ret = mMapper->freeBuffer(buffer);

    auto error = (ret.isOk()) ? static_cast<Error>(ret) : kTransactionError;
    ALOGE_IF(error != Error::NONE, "freeBuffer(%p) failed with %d",
            buffer, error);
}

Error Mapper::lock(buffer_handle_t bufferHandle, uint64_t usage,
        const V2_0::IMapper::Rect& accessRegion,
        int acquireFence, void** outData) const
{
    V4_0::IMapper::Rect accessRegionV4;
    translate(accessRegion, accessRegionV4);

    auto buffer = const_cast<native_handle_t*>(bufferHandle);

    // put acquireFence in a hidl_handle
    hardware::hidl_handle acquireFenceHandle;
    NATIVE_HANDLE_DECLARE_STORAGE(acquireFenceStorage, 1, 0);
    if (acquireFence >= 0) {
        auto h = native_handle_init(acquireFenceStorage, 1, 0);
        h->data[0] = acquireFence;
        acquireFenceHandle = h;
    }

    Error error;
    auto ret = mMapper->lock(buffer, usage, accessRegionV4, acquireFenceHandle,
            [&](const auto& tmpError, const auto& tmpData)
            {
                error = tmpError;
                if (error != Error::NONE) {
                    return;
                }

                *outData = tmpData;
            });

    // we own acquireFence even on errors
    if (acquireFence >= 0) {
        close(acquireFence);
    }

    return (ret.isOk()) ? error : kTransactionError;
}

Error Mapper::lock(buffer_handle_t bufferHandle, uint64_t usage,
        const V2_0::IMapper::Rect& accessRegion,
        int acquireFence, YCbCrLayout* outLayout) const
{
    auto buffer = const_cast<native_handle_t*>(bufferHandle);
    YCbCrLayout layout = {};
    void* mapped = nullptr;

    V4_0::IMapper::Rect accessRegionV4;
    translate(accessRegion, accessRegionV4);

    // put acquireFence in a hidl_handle
    hardware::hidl_handle acquireFenceHandle;
    NATIVE_HANDLE_DECLARE_STORAGE(acquireFenceStorage, 1, 0);
    if (acquireFence >= 0) {
        auto h = native_handle_init(acquireFenceStorage, 1, 0);
        h->data[0] = acquireFence;
        acquireFenceHandle = h;
    }

    Error error;
    auto ret = mMapper->lock(buffer, usage, accessRegionV4,
            acquireFenceHandle,
            [&](const auto& tmpError, const auto& tmpLayout)
            {
                error = tmpError;
                if (error != Error::NONE) {
                    return;
                }

                mapped = tmpLayout;
            });

    // TODO: IMPL
    if (mapped != nullptr) {
    }

    *outLayout = layout;

    // we own acquireFence even on errors
    if (acquireFence >= 0) {
        close(acquireFence);
    }

    return (ret.isOk()) ? error : kTransactionError;
}

int Mapper::unlock(buffer_handle_t bufferHandle) const
{
    auto buffer = const_cast<native_handle_t*>(bufferHandle);

    int releaseFence = -1;
    Error error;
    auto ret = mMapper->unlock(buffer,
            [&](const auto& tmpError, const auto& tmpReleaseFence)
            {
                error = tmpError;
                if (error != Error::NONE) {
                    return;
                }

                auto fenceHandle = tmpReleaseFence.getNativeHandle();
                if (fenceHandle && fenceHandle->numFds == 1) {
                    int fd = dup(fenceHandle->data[0]);
                    if (fd >= 0) {
                        releaseFence = fd;
                    } else {
                        ALOGD("failed to dup unlock release fence");
                        sync_wait(fenceHandle->data[0], -1);
                    }
                }
            });

    if (!ret.isOk()) {
        error = kTransactionError;
    }

    if (error != Error::NONE) {
        ALOGE("unlock(%p) failed with %d", buffer, error);
    }

    return releaseFence;
}

Allocator::Allocator(const Mapper& mapper)
    : mMapper(mapper)
{
    mAllocator = IAllocator::getService();
    if (mAllocator == nullptr) {
        LOG_ALWAYS_FATAL("gralloc-alloc is missing");
    }
}

std::string Allocator::dumpDebugInfo() const
{
    std::string debugInfo;

    return debugInfo;
}

Error Allocator::allocate(V2_0::BufferDescriptor descriptor, uint32_t count,
        uint32_t* outStride, buffer_handle_t* outBufferHandles) const
{
    V4_0::BufferDescriptor descriptorV4;
    translate(descriptor, descriptorV4);

    Error error;
    auto ret = mAllocator->allocate(descriptorV4, count,
            [&](const auto& tmpError, const auto& tmpStride,
                const auto& tmpBuffers) {
                error = tmpError;
                if (tmpError != Error::NONE) {
                    return;
                }

                // import buffers
                for (uint32_t i = 0; i < count; i++) {
                    error = mMapper.importBuffer(tmpBuffers[i],
                            &outBufferHandles[i]);
                    if (error != Error::NONE) {
                        for (uint32_t j = 0; j < i; j++) {
                            mMapper.freeBuffer(outBufferHandles[j]);
                            outBufferHandles[j] = nullptr;
                        }
                        return;
                    }
                }

                *outStride = tmpStride;
            });

    return (ret.isOk()) ? error : kTransactionError;
}

} // namespace GrallocWrapper

} // namespace android
