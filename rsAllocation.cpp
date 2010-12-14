/*
 * Copyright (C) 2009 The Android Open Source Project
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
#ifndef ANDROID_RS_BUILD_FOR_HOST
#include "rsContext.h"

#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <GLES/glext.h>
#else
#include "rsContextHostStub.h"

#include <OpenGL/gl.h>
#include <OpenGl/glext.h>
#endif

#include "utils/StopWatch.h"

using namespace android;
using namespace android::renderscript;

Allocation::Allocation(Context *rsc, const Type *type, uint32_t usages) : ObjectBase(rsc) {
    init(rsc, type);

    mUsageFlags = usages;

    mPtr = malloc(mType->getSizeBytes());
    if (mType->getElement()->getHasReferences()) {
        memset(mPtr, 0, mType->getSizeBytes());
    }
    if (!mPtr) {
        LOGE("Allocation::Allocation, alloc failure");
    }
}


void Allocation::init(Context *rsc, const Type *type) {
    mPtr = NULL;

    mCpuWrite = false;
    mCpuRead = false;
    mGpuWrite = false;
    mGpuRead = false;

    mReadWriteRatio = 0;
    mUpdateSize = 0;
    mUsageFlags = 0;
    mMipmapControl = RS_ALLOCATION_MIPMAP_NONE;

    mTextureID = 0;
    mBufferID = 0;
    mUploadDefered = false;

    mUserBitmapCallback = NULL;
    mUserBitmapCallbackData = NULL;

    mType.set(type);
    rsAssert(type);

    mPtr = NULL;
}

Allocation::~Allocation() {
    if (mUserBitmapCallback != NULL) {
        mUserBitmapCallback(mUserBitmapCallbackData);
    } else {
        free(mPtr);
    }
    mPtr = NULL;

    if (mBufferID) {
        // Causes a SW crash....
        //LOGV(" mBufferID %i", mBufferID);
        //glDeleteBuffers(1, &mBufferID);
        //mBufferID = 0;
    }
    if (mTextureID) {
        glDeleteTextures(1, &mTextureID);
        mTextureID = 0;
    }
}

void Allocation::setCpuWritable(bool) {
}

void Allocation::setGpuWritable(bool) {
}

void Allocation::setCpuReadable(bool) {
}

void Allocation::setGpuReadable(bool) {
}

bool Allocation::fixAllocation() {
    return false;
}

void Allocation::deferedUploadToTexture(const Context *rsc, bool genMipmap, uint32_t lodOffset) {
    rsAssert(lodOffset < mType->getLODCount());
    mUsageFlags |= RS_ALLOCATION_USAGE_GRAPHICS_TEXTURE;
    mTextureLOD = lodOffset;
    mUploadDefered = true;
    mTextureGenMipmap = !mType->getDimLOD() && genMipmap;
}

uint32_t Allocation::getGLTarget() const {
    if (getIsTexture()) {
        if (mType->getDimFaces()) {
            return GL_TEXTURE_CUBE_MAP;
        } else {
            return GL_TEXTURE_2D;
        }
    }
    if (getIsBufferObject()) {
        return GL_ARRAY_BUFFER;
    }
    return 0;
}

void Allocation::syncAll(Context *rsc, RsAllocationUsageType src) {
    rsAssert(src == RS_ALLOCATION_USAGE_SCRIPT);

    if (getIsTexture()) {
        uploadToTexture(rsc);
    }
    if (getIsBufferObject()) {
        uploadToBufferObject(rsc);
    }

    mUploadDefered = false;
}

void Allocation::uploadToTexture(const Context *rsc) {

    mUsageFlags |= RS_ALLOCATION_USAGE_GRAPHICS_TEXTURE;
    GLenum type = mType->getElement()->getComponent().getGLType();
    GLenum format = mType->getElement()->getComponent().getGLFormat();

    if (!type || !format) {
        return;
    }

    bool isFirstUpload = false;

    if (!mTextureID) {
        glGenTextures(1, &mTextureID);

        if (!mTextureID) {
            // This should not happen, however, its likely the cause of the
            // white sqare bug.
            // Force a crash to 1: restart the app, 2: make sure we get a bugreport.
            LOGE("Upload to texture failed to gen mTextureID");
            rsc->dumpDebug();
            mUploadDefered = true;
            return;
        }
        isFirstUpload = true;
    }

    GLenum target = (GLenum)getGLTarget();
    glBindTexture(target, mTextureID);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (target == GL_TEXTURE_2D) {
        upload2DTexture(isFirstUpload);
    } else if (target == GL_TEXTURE_CUBE_MAP) {
        uploadCubeTexture(isFirstUpload);
    }

    if (mTextureGenMipmap) {
#ifndef ANDROID_RS_BUILD_FOR_HOST
        glGenerateMipmap(target);
#endif //ANDROID_RS_BUILD_FOR_HOST
    }

    rsc->checkError("Allocation::uploadToTexture");
}

void Allocation::upload2DTexture(bool isFirstUpload) {
    GLenum type = mType->getElement()->getComponent().getGLType();
    GLenum format = mType->getElement()->getComponent().getGLFormat();

    Adapter2D adapt(getContext(), this);
    for (uint32_t lod = 0; (lod + mTextureLOD) < mType->getLODCount(); lod++) {
        adapt.setLOD(lod+mTextureLOD);

        uint16_t * ptr = static_cast<uint16_t *>(adapt.getElement(0,0));
        if (isFirstUpload) {
            glTexImage2D(GL_TEXTURE_2D, lod, format,
                         adapt.getDimX(), adapt.getDimY(),
                         0, format, type, ptr);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, lod, 0, 0,
                            adapt.getDimX(), adapt.getDimY(),
                            format, type, ptr);
        }
    }
}

void Allocation::uploadCubeTexture(bool isFirstUpload) {
    GLenum type = mType->getElement()->getComponent().getGLType();
    GLenum format = mType->getElement()->getComponent().getGLFormat();

    GLenum faceOrder[] = {
        GL_TEXTURE_CUBE_MAP_POSITIVE_X,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
    };

    Adapter2D adapt(getContext(), this);
    for (uint32_t face = 0; face < 6; face ++) {
        adapt.setFace(face);

        for (uint32_t lod = 0; (lod + mTextureLOD) < mType->getLODCount(); lod++) {
            adapt.setLOD(lod+mTextureLOD);

            uint16_t * ptr = static_cast<uint16_t *>(adapt.getElement(0,0));

            if (isFirstUpload) {
                glTexImage2D(faceOrder[face], lod, format,
                             adapt.getDimX(), adapt.getDimY(),
                             0, format, type, ptr);
            } else {
                glTexSubImage2D(faceOrder[face], lod, 0, 0,
                                adapt.getDimX(), adapt.getDimY(),
                                format, type, ptr);
            }
        }
    }
}

void Allocation::deferedUploadToBufferObject(const Context *rsc) {
    mUsageFlags |= RS_ALLOCATION_USAGE_GRAPHICS_VERTEX;
    mUploadDefered = true;
}

void Allocation::uploadToBufferObject(const Context *rsc) {
    rsAssert(!mType->getDimY());
    rsAssert(!mType->getDimZ());

    mUsageFlags |= RS_ALLOCATION_USAGE_GRAPHICS_VERTEX;

    if (!mBufferID) {
        glGenBuffers(1, &mBufferID);
    }
    if (!mBufferID) {
        LOGE("Upload to buffer object failed");
        mUploadDefered = true;
        return;
    }
    GLenum target = (GLenum)getGLTarget();
    glBindBuffer(target, mBufferID);
    glBufferData(target, mType->getSizeBytes(), getPtr(), GL_DYNAMIC_DRAW);
    glBindBuffer(target, 0);
    rsc->checkError("Allocation::uploadToBufferObject");
}

void Allocation::uploadCheck(Context *rsc) {
    if (mUploadDefered) {
        syncAll(rsc, RS_ALLOCATION_USAGE_SCRIPT);
    }
}


void Allocation::data(Context *rsc, const void *data, uint32_t sizeBytes) {
    uint32_t size = mType->getSizeBytes();
    if (size != sizeBytes) {
        LOGE("Allocation::data called with mismatched size expected %i, got %i", size, sizeBytes);
        return;
    }

    if (mType->getElement()->getHasReferences()) {
        incRefs(data, sizeBytes / mType->getElement()->getSizeBytes());
        decRefs(mPtr, sizeBytes / mType->getElement()->getSizeBytes());
    }

    memcpy(mPtr, data, size);
    sendDirty();
    mUploadDefered = true;
}

void Allocation::read(void *data) {
    memcpy(data, mPtr, mType->getSizeBytes());
}

void Allocation::subData(Context *rsc, uint32_t xoff, uint32_t count, const void *data, uint32_t sizeBytes) {
    uint32_t eSize = mType->getElementSizeBytes();
    uint8_t * ptr = static_cast<uint8_t *>(mPtr);
    ptr += eSize * xoff;
    uint32_t size = count * eSize;

    if (size != sizeBytes) {
        LOGE("Allocation::subData called with mismatched size expected %i, got %i", size, sizeBytes);
        mType->dumpLOGV("type info");
        return;
    }

    if (mType->getElement()->getHasReferences()) {
        incRefs(data, count);
        decRefs(ptr, count);
    }

    memcpy(ptr, data, size);
    sendDirty();
    mUploadDefered = true;
}

void Allocation::subData(Context *rsc, uint32_t xoff, uint32_t yoff,
             uint32_t w, uint32_t h, const void *data, uint32_t sizeBytes) {
    uint32_t eSize = mType->getElementSizeBytes();
    uint32_t lineSize = eSize * w;
    uint32_t destW = mType->getDimX();

    const uint8_t *src = static_cast<const uint8_t *>(data);
    uint8_t *dst = static_cast<uint8_t *>(mPtr);
    dst += eSize * (xoff + yoff * destW);

    if ((lineSize * eSize * h) != sizeBytes) {
        rsAssert(!"Allocation::subData called with mismatched size");
        return;
    }

    for (uint32_t line=yoff; line < (yoff+h); line++) {
        if (mType->getElement()->getHasReferences()) {
            incRefs(src, w);
            decRefs(dst, w);
        }
        memcpy(dst, src, lineSize);
        src += lineSize;
        dst += destW * eSize;
    }
    sendDirty();
    mUploadDefered = true;
}

void Allocation::subData(Context *rsc, uint32_t xoff, uint32_t yoff, uint32_t zoff,
             uint32_t w, uint32_t h, uint32_t d, const void *data, uint32_t sizeBytes) {
}

void Allocation::subElementData(Context *rsc, uint32_t x, const void *data,
                                uint32_t cIdx, uint32_t sizeBytes) {
    uint32_t eSize = mType->getElementSizeBytes();
    uint8_t * ptr = static_cast<uint8_t *>(mPtr);
    ptr += eSize * x;

    if (cIdx >= mType->getElement()->getFieldCount()) {
        LOGE("Error Allocation::subElementData component %i out of range.", cIdx);
        rsc->setError(RS_ERROR_BAD_VALUE, "subElementData component out of range.");
        return;
    }

    if (x >= mType->getDimX()) {
        LOGE("Error Allocation::subElementData X offset %i out of range.", x);
        rsc->setError(RS_ERROR_BAD_VALUE, "subElementData X offset out of range.");
        return;
    }

    const Element * e = mType->getElement()->getField(cIdx);
    ptr += mType->getElement()->getFieldOffsetBytes(cIdx);

    if (sizeBytes != e->getSizeBytes()) {
        LOGE("Error Allocation::subElementData data size %i does not match field size %i.", sizeBytes, e->getSizeBytes());
        rsc->setError(RS_ERROR_BAD_VALUE, "subElementData bad size.");
        return;
    }

    if (e->getHasReferences()) {
        e->incRefs(data);
        e->decRefs(ptr);
    }

    memcpy(ptr, data, sizeBytes);
    sendDirty();
    mUploadDefered = true;
}

void Allocation::subElementData(Context *rsc, uint32_t x, uint32_t y,
                                const void *data, uint32_t cIdx, uint32_t sizeBytes) {
    uint32_t eSize = mType->getElementSizeBytes();
    uint8_t * ptr = static_cast<uint8_t *>(mPtr);
    ptr += eSize * (x + y * mType->getDimX());

    if (x >= mType->getDimX()) {
        LOGE("Error Allocation::subElementData X offset %i out of range.", x);
        rsc->setError(RS_ERROR_BAD_VALUE, "subElementData X offset out of range.");
        return;
    }

    if (y >= mType->getDimY()) {
        LOGE("Error Allocation::subElementData X offset %i out of range.", x);
        rsc->setError(RS_ERROR_BAD_VALUE, "subElementData X offset out of range.");
        return;
    }

    if (cIdx >= mType->getElement()->getFieldCount()) {
        LOGE("Error Allocation::subElementData component %i out of range.", cIdx);
        rsc->setError(RS_ERROR_BAD_VALUE, "subElementData component out of range.");
        return;
    }

    const Element * e = mType->getElement()->getField(cIdx);
    ptr += mType->getElement()->getFieldOffsetBytes(cIdx);

    if (sizeBytes != e->getSizeBytes()) {
        LOGE("Error Allocation::subElementData data size %i does not match field size %i.", sizeBytes, e->getSizeBytes());
        rsc->setError(RS_ERROR_BAD_VALUE, "subElementData bad size.");
        return;
    }

    if (e->getHasReferences()) {
        e->incRefs(data);
        e->decRefs(ptr);
    }

    memcpy(ptr, data, sizeBytes);
    sendDirty();
    mUploadDefered = true;
}

void Allocation::addProgramToDirty(const Program *p) {
    mToDirtyList.push(p);
}

void Allocation::removeProgramToDirty(const Program *p) {
    for (size_t ct=0; ct < mToDirtyList.size(); ct++) {
        if (mToDirtyList[ct] == p) {
            mToDirtyList.removeAt(ct);
            return;
        }
    }
    rsAssert(0);
}

void Allocation::dumpLOGV(const char *prefix) const {
    ObjectBase::dumpLOGV(prefix);

    String8 s(prefix);
    s.append(" type ");
    if (mType.get()) {
        mType->dumpLOGV(s.string());
    }

    LOGV("%s allocation ptr=%p mCpuWrite=%i, mCpuRead=%i, mGpuWrite=%i, mGpuRead=%i",
          prefix, mPtr, mCpuWrite, mCpuRead, mGpuWrite, mGpuRead);

    LOGV("%s allocation mUsageFlags=0x04%x, mMipmapControl=0x%04x, mTextureID=%i, mBufferID=%i",
          prefix, mUsageFlags, mMipmapControl, mTextureID, mBufferID);
}

void Allocation::serialize(OStream *stream) const {
    // Need to identify ourselves
    stream->addU32((uint32_t)getClassId());

    String8 name(getName());
    stream->addString(&name);

    // First thing we need to serialize is the type object since it will be needed
    // to initialize the class
    mType->serialize(stream);

    uint32_t dataSize = mType->getSizeBytes();
    // Write how much data we are storing
    stream->addU32(dataSize);
    // Now write the data
    stream->addByteArray(mPtr, dataSize);
}

Allocation *Allocation::createFromStream(Context *rsc, IStream *stream) {
    // First make sure we are reading the correct object
    RsA3DClassID classID = (RsA3DClassID)stream->loadU32();
    if (classID != RS_A3D_CLASS_ID_ALLOCATION) {
        LOGE("allocation loading skipped due to invalid class id\n");
        return NULL;
    }

    String8 name;
    stream->loadString(&name);

    Type *type = Type::createFromStream(rsc, stream);
    if (!type) {
        return NULL;
    }
    type->compute();

    // Number of bytes we wrote out for this allocation
    uint32_t dataSize = stream->loadU32();
    if (dataSize != type->getSizeBytes()) {
        LOGE("failed to read allocation because numbytes written is not the same loaded type wants\n");
        ObjectBase::checkDelete(type);
        return NULL;
    }

    Allocation *alloc = new Allocation(rsc, type, RS_ALLOCATION_USAGE_SCRIPT);
    alloc->setName(name.string(), name.size());

    // Read in all of our allocation data
    alloc->data(rsc, stream->getPtr() + stream->getPos(), dataSize);
    stream->reset(stream->getPos() + dataSize);

    return alloc;
}

void Allocation::sendDirty() const {
    for (size_t ct=0; ct < mToDirtyList.size(); ct++) {
        mToDirtyList[ct]->forceDirty();
    }
}

void Allocation::incRefs(const void *ptr, size_t ct, size_t startOff) const {
    const uint8_t *p = static_cast<const uint8_t *>(ptr);
    const Element *e = mType->getElement();
    uint32_t stride = e->getSizeBytes();

    p += stride * startOff;
    while (ct > 0) {
        e->incRefs(p);
        ct --;
        p += stride;
    }
}

void Allocation::decRefs(const void *ptr, size_t ct, size_t startOff) const {
    const uint8_t *p = static_cast<const uint8_t *>(ptr);
    const Element *e = mType->getElement();
    uint32_t stride = e->getSizeBytes();

    p += stride * startOff;
    while (ct > 0) {
        e->decRefs(p);
        ct --;
        p += stride;
    }
}

void Allocation::copyRange1D(Context *rsc, const Allocation *src, int32_t srcOff, int32_t destOff, int32_t len) {
}

void Allocation::resize1D(Context *rsc, uint32_t dimX) {
    Type *t = mType->cloneAndResize1D(rsc, dimX);

    uint32_t oldDimX = mType->getDimX();
    if (dimX == oldDimX) {
        return;
    }

    if (dimX < oldDimX) {
        decRefs(mPtr, oldDimX - dimX, dimX);
    }
    mPtr = realloc(mPtr, t->getSizeBytes());

    if (dimX > oldDimX) {
        const Element *e = mType->getElement();
        uint32_t stride = e->getSizeBytes();
        memset(((uint8_t *)mPtr) + stride * oldDimX, 0, stride * (dimX - oldDimX));
    }
    mType.set(t);
}

void Allocation::resize2D(Context *rsc, uint32_t dimX, uint32_t dimY) {
    LOGE("not implemented");
}

/////////////////
//


namespace android {
namespace renderscript {

void rsi_AllocationUploadToTexture(Context *rsc, RsAllocation va, bool genmip, uint32_t baseMipLevel) {
    Allocation *alloc = static_cast<Allocation *>(va);
    alloc->deferedUploadToTexture(rsc, genmip, baseMipLevel);
}

void rsi_AllocationUploadToBufferObject(Context *rsc, RsAllocation va) {
    Allocation *alloc = static_cast<Allocation *>(va);
    alloc->deferedUploadToBufferObject(rsc);
}

static void mip565(const Adapter2D &out, const Adapter2D &in) {
    uint32_t w = out.getDimX();
    uint32_t h = out.getDimY();

    for (uint32_t y=0; y < h; y++) {
        uint16_t *oPtr = static_cast<uint16_t *>(out.getElement(0, y));
        const uint16_t *i1 = static_cast<uint16_t *>(in.getElement(0, y*2));
        const uint16_t *i2 = static_cast<uint16_t *>(in.getElement(0, y*2+1));

        for (uint32_t x=0; x < w; x++) {
            *oPtr = rsBoxFilter565(i1[0], i1[1], i2[0], i2[1]);
            oPtr ++;
            i1 += 2;
            i2 += 2;
        }
    }
}

static void mip8888(const Adapter2D &out, const Adapter2D &in) {
    uint32_t w = out.getDimX();
    uint32_t h = out.getDimY();

    for (uint32_t y=0; y < h; y++) {
        uint32_t *oPtr = static_cast<uint32_t *>(out.getElement(0, y));
        const uint32_t *i1 = static_cast<uint32_t *>(in.getElement(0, y*2));
        const uint32_t *i2 = static_cast<uint32_t *>(in.getElement(0, y*2+1));

        for (uint32_t x=0; x < w; x++) {
            *oPtr = rsBoxFilter8888(i1[0], i1[1], i2[0], i2[1]);
            oPtr ++;
            i1 += 2;
            i2 += 2;
        }
    }
}

static void mip8(const Adapter2D &out, const Adapter2D &in) {
    uint32_t w = out.getDimX();
    uint32_t h = out.getDimY();

    for (uint32_t y=0; y < h; y++) {
        uint8_t *oPtr = static_cast<uint8_t *>(out.getElement(0, y));
        const uint8_t *i1 = static_cast<uint8_t *>(in.getElement(0, y*2));
        const uint8_t *i2 = static_cast<uint8_t *>(in.getElement(0, y*2+1));

        for (uint32_t x=0; x < w; x++) {
            *oPtr = (uint8_t)(((uint32_t)i1[0] + i1[1] + i2[0] + i2[1]) * 0.25f);
            oPtr ++;
            i1 += 2;
            i2 += 2;
        }
    }
}

static void mip(const Adapter2D &out, const Adapter2D &in) {
    switch (out.getBaseType()->getElement()->getSizeBits()) {
    case 32:
        mip8888(out, in);
        break;
    case 16:
        mip565(out, in);
        break;
    case 8:
        mip8(out, in);
        break;
    }
}

#ifndef ANDROID_RS_BUILD_FOR_HOST

void rsi_AllocationSyncAll(Context *rsc, RsAllocation va, RsAllocationUsageType src) {
    Allocation *a = static_cast<Allocation *>(va);
    a->syncAll(rsc, src);
}

void rsi_AllocationCopyFromBitmap(Context *rsc, RsAllocation va, const void *data, size_t dataLen) {
    Allocation *texAlloc = static_cast<Allocation *>(va);
    const Type * t = texAlloc->getType();

    uint32_t w = t->getDimX();
    uint32_t h = t->getDimY();
    bool genMips = t->getDimLOD();
    size_t s = w * h * t->getElementSizeBytes();
    if (s != dataLen) {
        rsc->setError(RS_ERROR_BAD_VALUE, "Bitmap size didn't match allocation size");
        return;
    }

    memcpy(texAlloc->getPtr(), data, s);
    if (genMips) {
        Adapter2D adapt(rsc, texAlloc);
        Adapter2D adapt2(rsc, texAlloc);
        for (uint32_t lod=0; lod < (texAlloc->getType()->getLODCount() -1); lod++) {
            adapt.setLOD(lod);
            adapt2.setLOD(lod + 1);
            mip(adapt2, adapt);
        }
    }
}

void rsi_AllocationCopyToBitmap(Context *rsc, RsAllocation va, void *data, size_t dataLen) {
    Allocation *texAlloc = static_cast<Allocation *>(va);
    const Type * t = texAlloc->getType();

    size_t s = t->getDimX() * t->getDimY() * t->getElementSizeBytes();
    if (s != dataLen) {
        rsc->setError(RS_ERROR_BAD_VALUE, "Bitmap size didn't match allocation size");
        return;
    }

    memcpy(data, texAlloc->getPtr(), s);
}

void rsi_AllocationData(Context *rsc, RsAllocation va, const void *data, uint32_t sizeBytes) {
    Allocation *a = static_cast<Allocation *>(va);
    a->data(rsc, data, sizeBytes);
}

void rsi_Allocation1DSubData(Context *rsc, RsAllocation va, uint32_t xoff, uint32_t count, const void *data, uint32_t sizeBytes) {
    Allocation *a = static_cast<Allocation *>(va);
    a->subData(rsc, xoff, count, data, sizeBytes);
}

void rsi_Allocation2DSubElementData(Context *rsc, RsAllocation va, uint32_t x, uint32_t y, const void *data, uint32_t eoff, uint32_t sizeBytes) {
    Allocation *a = static_cast<Allocation *>(va);
    a->subElementData(rsc, x, y, data, eoff, sizeBytes);
}

void rsi_Allocation1DSubElementData(Context *rsc, RsAllocation va, uint32_t x, const void *data, uint32_t eoff, uint32_t sizeBytes) {
    Allocation *a = static_cast<Allocation *>(va);
    a->subElementData(rsc, x, data, eoff, sizeBytes);
}

void rsi_Allocation2DSubData(Context *rsc, RsAllocation va, uint32_t xoff, uint32_t yoff, uint32_t w, uint32_t h, const void *data, uint32_t sizeBytes) {
    Allocation *a = static_cast<Allocation *>(va);
    a->subData(rsc, xoff, yoff, w, h, data, sizeBytes);
}

void rsi_AllocationRead(Context *rsc, RsAllocation va, void *data) {
    Allocation *a = static_cast<Allocation *>(va);
    a->read(data);
}

void rsi_AllocationResize1D(Context *rsc, RsAllocation va, uint32_t dimX) {
    Allocation *a = static_cast<Allocation *>(va);
    a->resize1D(rsc, dimX);
}

void rsi_AllocationResize2D(Context *rsc, RsAllocation va, uint32_t dimX, uint32_t dimY) {
    Allocation *a = static_cast<Allocation *>(va);
    a->resize2D(rsc, dimX, dimY);
}

#endif //ANDROID_RS_BUILD_FOR_HOST

}
}

const void * rsaAllocationGetType(RsContext con, RsAllocation va) {
    Allocation *a = static_cast<Allocation *>(va);
    a->getType()->incUserRef();

    return a->getType();
}

RsAllocation rsaAllocationCreateTyped(RsContext con, RsType vtype,
                                      RsAllocationMipmapControl mips,
                                      uint32_t usages) {
    Context *rsc = static_cast<Context *>(con);
    Allocation * alloc = new Allocation(rsc, static_cast<Type *>(vtype), usages);
    alloc->incUserRef();
    return alloc;
}


RsAllocation rsaAllocationCreateFromBitmap(RsContext con, RsType vtype,
                                           RsAllocationMipmapControl mips,
                                           const void *data, uint32_t usages) {
    Context *rsc = static_cast<Context *>(con);
    Type *t = static_cast<Type *>(vtype);

    RsAllocation vTexAlloc = rsaAllocationCreateTyped(rsc, vtype, mips, usages);
    Allocation *texAlloc = static_cast<Allocation *>(vTexAlloc);
    if (texAlloc == NULL) {
        LOGE("Memory allocation failure");
        return NULL;
    }

    memcpy(texAlloc->getPtr(), data, t->getDimX() * t->getDimY() * t->getElementSizeBytes());
    if (mips == RS_ALLOCATION_MIPMAP_FULL) {
        Adapter2D adapt(rsc, texAlloc);
        Adapter2D adapt2(rsc, texAlloc);
        for (uint32_t lod=0; lod < (texAlloc->getType()->getLODCount() -1); lod++) {
            adapt.setLOD(lod);
            adapt2.setLOD(lod + 1);
            mip(adapt2, adapt);
        }
    }

    texAlloc->deferedUploadToTexture(rsc, false, 0);
    return texAlloc;
}

RsAllocation rsaAllocationCubeCreateFromBitmap(RsContext con, RsType vtype,
                                               RsAllocationMipmapControl mips,
                                               const void *data, uint32_t usages) {
    Context *rsc = static_cast<Context *>(con);
    Type *t = static_cast<Type *>(vtype);

    // Cubemap allocation's faces should be Width by Width each.
    // Source data should have 6 * Width by Width pixels
    // Error checking is done in the java layer
    RsAllocation vTexAlloc = rsaAllocationCreateTyped(rsc, t, mips, usages);
    Allocation *texAlloc = static_cast<Allocation *>(vTexAlloc);
    if (texAlloc == NULL) {
        LOGE("Memory allocation failure");
        return NULL;
    }

    uint8_t *sourcePtr = (uint8_t*)data;
    for (uint32_t face = 0; face < 6; face ++) {
        Adapter2D faceAdapter(rsc, texAlloc);
        faceAdapter.setFace(face);

        size_t cpySize = t->getDimX() * t->getDimX() * t->getElementSizeBytes();
        memcpy(faceAdapter.getElement(0, 0), sourcePtr, cpySize);

        // Move the data pointer to the next cube face
        sourcePtr += cpySize;

        if (mips == RS_ALLOCATION_MIPMAP_FULL) {
            Adapter2D adapt(rsc, texAlloc);
            Adapter2D adapt2(rsc, texAlloc);
            adapt.setFace(face);
            adapt2.setFace(face);
            for (uint32_t lod=0; lod < (texAlloc->getType()->getLODCount() -1); lod++) {
                adapt.setLOD(lod);
                adapt2.setLOD(lod + 1);
                mip(adapt2, adapt);
            }
        }
    }

    texAlloc->deferedUploadToTexture(rsc, false, 0);
    return texAlloc;
}
