#include "SevenZipArchiveWriter.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QDebug>
#include <QSaveFile>
#include <QStringList>
#include <QtEndian>

#include "../../common/compression/SevenZipBcj2Decoder.h"
#include "../../common/compression/SevenZipAesDecoder.h"
#include "../../common/compression/DeflateDecoder.h"
#include "../third_party/lzma/Alloc.h"
#include "../third_party/lzma/Lzma2Dec.h"
#include "../third_party/lzma/LzmaDec.h"
#include "../../opener/compression/third_party/lzma/Ppmd7.h"
#include "../third_party/lzma/LzmaEnc.h"
#include "../third_party/lzma/Lzma2Enc.h"
#include "../third_party/miniz/miniz_tinfl.h"
#include "../third_party/bzip2/bzlib.h"
#include "../third_party/bzip2/bzlib.h"
#include "../third_party/miniz/miniz_tdef.h"
#include "../../opener/compression/third_party/lzma/Ppmd7.h"
#include "../third_party/lzma/Bra.h"
#include "../third_party/lzma/Delta.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <map>

extern "C" void Ppmd7_UpdateModel(CPpmd7* model);
extern "C" BoolInt Ppmd7z_RangeDec_Init(CPpmd7_RangeDec* decoder);
extern "C" int Ppmd7z_DecodeSymbol(CPpmd7* model);

namespace peare {
namespace writer {
namespace {

const quint8 kSignature[] = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};
const qint64 kIoChunk = 1024 * 1024;
quint32 crc32Update(quint32 crc, const char* data, qint64 size);
quint32 crc32Bytes(const std::uint8_t* data, size_t size);
bool writeAll(QIODevice* output, const char* data, qint64 size);

class CompositeInput final : public QIODevice {
public:
    CompositeInput(const std::vector<QIODevice*>& inputs,
                   const std::vector<quint64>& sizes,
                   std::vector<quint32>* fileCrcs)
        : inputs_(inputs), remaining_(sizes), fileCrcs_(fileCrcs),
          groupCrc_(0xffffffffU) {}

    quint32 groupCrc() const { return groupCrc_ ^ 0xffffffffU; }

protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        if (maximum < 0 || !fileCrcs_ || fileCrcs_->size() != inputs_.size())
            return -1;
        qint64 total = 0;
        while (total < maximum && current_ < inputs_.size()) {
            if (!remaining_[current_]) {
                ++current_;
                continue;
            }
            const qint64 request = qint64(std::min<quint64>(
                remaining_[current_], quint64(maximum - total)));
            const qint64 count = inputs_[current_]->read(data + total, request);
            if (count <= 0) return total ? total : -1;
            (*fileCrcs_)[current_] = crc32Update(
                (*fileCrcs_)[current_], data + total, count);
            groupCrc_ = crc32Update(groupCrc_, data + total, count);
            remaining_[current_] -= quint64(count);
            total += count;
        }
        return total;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    const std::vector<QIODevice*>& inputs_;
    std::vector<quint64> remaining_;
    std::vector<quint32>* fileCrcs_;
    quint32 groupCrc_;
    size_t current_ = 0;
};

struct LzmaInputStream {
    ISeqInStream stream;
    QIODevice* input = nullptr;
    quint64 count = 0;
    quint32 crc = 0xffffffffU;
    bool failed = false;
};

struct LzmaOutputStream {
    ISeqOutStream stream;
    QIODevice* output = nullptr;
    quint64 count = 0;
    quint32 crc = 0xffffffffU;
    bool failed = false;
};

struct DeflateOutputStream {
    QIODevice* output = nullptr;
    quint64 count = 0;
    quint32 crc = 0xffffffffU;
    bool failed = false;
};

struct PpmdOutputStream {
    IByteOut stream;
    QIODevice* output = nullptr;
    quint64 count = 0;
    quint64 limit = 0;
    quint32 crc = 0xffffffffU;
    char buffer[65536];
    size_t used = 0;
    bool failed = false;
};

void flushPpmdOutput(PpmdOutputStream* output)
{
    if (!output->used || output->failed) return;
    const qint64 size = qint64(output->used);
    if (!writeAll(output->output, output->buffer, size)) {
        output->failed = true;
        return;
    }
    output->crc = crc32Update(output->crc, output->buffer, size);
    output->count += output->used;
    output->used = 0;
}

void writePpmdByte(IByteOutPtr stream, Byte value)
{
    PpmdOutputStream* output = reinterpret_cast<PpmdOutputStream*>(
        const_cast<IByteOut*>(stream));
    if (output->failed) return;
    if (output->count + output->used >= output->limit) {
        output->failed = true;
        return;
    }
    output->buffer[output->used++] = char(value);
    if (output->used == sizeof(output->buffer)) flushPpmdOutput(output);
}

void ppmdShiftLow(CPpmd7z_RangeEnc* coder)
{
    const UInt32 low = UInt32(coder->Low);
    if (low < 0xff000000U || UInt32(coder->Low >> 32) != 0) {
        Byte cache = coder->Cache;
        do {
            IByteOut_Write(coder->Stream, Byte(cache + Byte(coder->Low >> 32)));
            cache = 0xff;
        } while (--coder->CacheSize);
        coder->Cache = Byte(low >> 24);
        coder->CacheSize = 1;
    } else {
        ++coder->CacheSize;
    }
    coder->Low = UInt32(low << 8);
}

void ppmdNormalize(CPpmd7z_RangeEnc* coder)
{
    if (coder->Range < (UInt32(1) << 24)) {
        coder->Range <<= 8;
        ppmdShiftLow(coder);
    }
}

void ppmdEncodeInterval(CPpmd7* model, UInt32 start, UInt32 size,
                        UInt32 total, unsigned normalizeCount)
{
    CPpmd7z_RangeEnc* coder = &model->rc.enc;
    coder->Range /= total;
    coder->Low += UInt64(start) * coder->Range;
    coder->Range *= size;
    while (normalizeCount--) ppmdNormalize(coder);
}

bool ppmdSuffixContains(CPpmd7* model, Byte symbol)
{
    if (!model->MinContext->Suffix) return true;
    CPpmd7_Context* suffix = Ppmd7_GetContext(
        model, model->MinContext->Suffix);
    if (suffix->NumStats == 1)
        return Ppmd7Context_OneState(suffix)->Symbol == symbol;
    CPpmd_State* states = Ppmd7_GetStats(model, suffix);
    for (unsigned index = 0; index < suffix->NumStats; ++index)
        if (states[index].Symbol == symbol) return true;
    return false;
}

bool encodePpmd7Symbol(CPpmd7* model, Byte symbol,
                       unsigned char available[256])
{
    CPpmd7_Context* context = model->MinContext;
    if (context->NumStats != 1) {
        CPpmd_State* state = Ppmd7_GetStats(model, context);
        UInt32 start = 0;
        const UInt32 statsTotal = context->Union2.SummFreq;
        for (unsigned index = 0; index < context->NumStats; ++index) {
            if (state[index].Symbol == symbol) {
                ppmdEncodeInterval(model, start, state[index].Freq,
                                   statsTotal, 2);
                model->FoundState = &state[index];
                if (!ppmdSuffixContains(model, symbol)) return false;
                if (!index) {
                    Ppmd7_Update1_0(model);
                } else {
                    model->PrevSuccess = 0;
                    Ppmd7_Update1(model);
                }
                return true;
            }
            start += state[index].Freq;
        }
        if (start >= statsTotal) return false;
        ppmdEncodeInterval(model, start, statsTotal - start, statsTotal, 0);
        model->HiBitsFlag = PPMD7_HiBitsFlag_3(model->FoundState->Symbol);
        std::memset(available, 1, 256);
        for (unsigned index = 0; index < context->NumStats; ++index)
            available[state[index].Symbol] = 0;
        model->PrevSuccess = 0;
    } else {
        CPpmd_State* state = Ppmd7Context_OneState(model->MinContext);
        UInt16* probability = Ppmd7_GetBinSumm(model);
        const UInt32 probability0 = *probability;
        const UInt32 size0 = (model->rc.enc.Range >> 14) * probability0;
        const UInt32 updated = PPMD_UPDATE_PROB_1(probability0);
        if (state->Symbol == symbol) {
            *probability = UInt16(updated + (1 << PPMD_INT_BITS));
            model->rc.enc.Range = size0;
            ppmdNormalize(&model->rc.enc);
            const unsigned frequency = state->Freq;
            CPpmd7_Context* successor = Ppmd7_GetContext(
                model, Ppmd_GET_SUCCESSOR(state));
            model->FoundState = state;
            model->PrevSuccess = 1;
            ++model->RunLength;
            state->Freq = Byte(frequency + (frequency < 128));
            if (model->OrderFall == 0 &&
                reinterpret_cast<const Byte*>(successor) > model->Text) {
                model->MaxContext = model->MinContext = successor;
            } else {
                Ppmd7_UpdateModel(model);
            }
            return true;
        }
        *probability = UInt16(updated);
        model->InitEsc = model->ExpEscape[updated >> 10];
        model->rc.enc.Low += size0;
        model->rc.enc.Range -= size0;
        std::memset(available, 1, 256);
        available[state->Symbol] = 0;
        model->PrevSuccess = 0;
    }

    for (;;) {
        ppmdNormalize(&model->rc.enc);
        ppmdNormalize(&model->rc.enc);
        context = model->MinContext;
        const unsigned numMasked = context->NumStats;
        do {
            ++model->OrderFall;
            if (!context->Suffix) return false;
            context = Ppmd7_GetContext(model, context->Suffix);
        } while (context->NumStats == numMasked);

        CPpmd_State* states = Ppmd7_GetStats(model, context);
        model->MinContext = context;
        UInt32 availableTotal = 0;
        for (unsigned index = 0; index < context->NumStats; ++index) {
            if (available[states[index].Symbol])
                availableTotal += states[index].Freq;
        }
        UInt32 escapedTotal = 0;
        CPpmd_See* see = Ppmd7_MakeEscFreq(model, numMasked, &escapedTotal);
        const UInt32 total = availableTotal + escapedTotal;
        if (!escapedTotal || !total) return false;
        UInt32 start = 0;
        for (unsigned index = 0; index < context->NumStats; ++index) {
            if (!available[states[index].Symbol]) continue;
            if (states[index].Symbol == symbol) {
                ppmdEncodeInterval(model, start, states[index].Freq, total, 2);
                Ppmd_See_UPDATE(see)
                model->FoundState = &states[index];
                Ppmd7_Update2(model);
                return true;
            }
            start += states[index].Freq;
        }
        ppmdEncodeInterval(model, availableTotal, escapedTotal, total, 0);
        see->Summ = UInt16(see->Summ + total);
        for (unsigned index = 0; index < context->NumStats; ++index)
            available[states[index].Symbol] = 0;
    }
}

bool encodePpmd7(QIODevice* input, QIODevice* output,
                 const QByteArray& sourceProperties,
                 QByteArray* properties,
                 quint64* unpackedSize, quint32* unpackedCrc,
                 quint64* packedSize, quint32* packedCrc, QString* error)
{
    unsigned order = 16;
    UInt32 memorySize = 16U * 1024U * 1024U;
    if (!sourceProperties.isEmpty()) {
        if (sourceProperties.size() != 5) {
            if (error) *error = QStringLiteral("The source PPMd properties are invalid.");
            return false;
        }
        order = static_cast<unsigned char>(sourceProperties.at(0));
        memorySize = 0;
        for (unsigned index = 0; index < 4; ++index)
            memorySize |= UInt32(static_cast<unsigned char>(sourceProperties.at(1 + int(index))))
                         << (index * 8);
        if (order < 2 || order > 64 || memorySize < 1024U ||
            memorySize > 1024U * 1024U * 1024U) {
            if (error) *error = QStringLiteral(
                "The source PPMd properties are out of range (order %1, memory %2).")
                .arg(order).arg(memorySize);
            return false;
        }
    }
    properties->append(char(order));
    for (unsigned index = 0; index < 4; ++index)
        properties->append(char((memorySize >> (index * 8)) & 0xffU));
    PpmdOutputStream encoded = {};
    encoded.stream.Write = writePpmdByte;
    encoded.output = output;
    encoded.crc = 0xffffffffU;
    const qint64 inputSizeHint = input->size();
    if (inputSizeHint >= 0) {
        const quint64 size = quint64(inputSizeHint);
        encoded.limit = size > ((std::numeric_limits<quint64>::max)() -
                                1024 * 1024) / 64
            ? (std::numeric_limits<quint64>::max)()
            : size * 64 + 1024 * 1024;
    } else {
        encoded.limit = (std::numeric_limits<quint64>::max)();
    }
    CPpmd7 model;
    Ppmd7_Construct(&model);
    if (!Ppmd7_Alloc(&model, memorySize, &g_Alloc)) {
        if (error) *error = QStringLiteral("Could not allocate the PPMd model.");
        return false;
    }
    model.rc.enc.Stream = &encoded.stream;
    model.rc.enc.Low = 0;
    model.rc.enc.Range = 0xffffffffU;
    model.rc.enc.Cache = 0;
    model.rc.enc.CacheSize = 1;
    Ppmd7_Init(&model, order);
    quint64 inputSize = 0;
    quint32 inputCrc = 0xffffffffU;
    QByteArray buffer(int(kIoChunk), Qt::Uninitialized);
    bool success = true;
    std::memset(buffer.data(), 0, size_t(buffer.size()));
    unsigned char available[256];
    quint64 symbolOffset = 0;
    for (;;) {
        const qint64 count = input->read(buffer.data(), buffer.size());
        if (count < 0) {
            if (error) *error = input->errorString();
            success = false;
            break;
        }
        if (!count) break;
        inputCrc = crc32Update(inputCrc, buffer.constData(), count);
        inputSize += quint64(count);
        for (qint64 index = 0; index < count; ++index) {
            const Byte symbol = static_cast<unsigned char>(buffer.at(int(index)));
            if (!encodePpmd7Symbol(&model, symbol, available) ||
                encoded.failed) {
                if (error) *error = QStringLiteral("PPMd encoding failed.");
                success = false;
                break;
            }
            ++symbolOffset;
        }
        if (!success) break;
    }
    if (success) {
        for (unsigned index = 0; index < 5; ++index) ppmdShiftLow(&model.rc.enc);
        flushPpmdOutput(&encoded);
        if (encoded.failed) {
            if (error) *error = output->errorString();
            success = false;
        }
    }
    Ppmd7_Free(&model, &g_Alloc);
    if (!success) return false;
    *unpackedSize = inputSize;
    *unpackedCrc = inputCrc ^ 0xffffffffU;
    *packedSize = encoded.count;
    *packedCrc = encoded.crc ^ 0xffffffffU;
    return true;
}

SRes readLzmaInput(ISeqInStreamPtr stream, void* data, size_t* size)
{
    LzmaInputStream* input = reinterpret_cast<LzmaInputStream*>(
        const_cast<ISeqInStream*>(stream));
    const size_t requested = *size;
    const qint64 actual = input->input->read(static_cast<char*>(data),
                                             qint64(requested));
    if (actual < 0) {
        input->failed = true;
        *size = 0;
        return SZ_ERROR_READ;
    }
    *size = size_t(actual);
    if (actual) {
        input->crc = crc32Update(input->crc, static_cast<const char*>(data), actual);
        input->count += quint64(actual);
    }
    return SZ_OK;
}

size_t writeLzmaOutput(ISeqOutStreamPtr stream, const void* data, size_t size)
{
    LzmaOutputStream* output = reinterpret_cast<LzmaOutputStream*>(
        const_cast<ISeqOutStream*>(stream));
    const qint64 actual = output->output->write(static_cast<const char*>(data),
                                                qint64(size));
    if (actual != qint64(size)) {
        output->failed = true;
        return 0;
    }
    output->crc = crc32Update(output->crc, static_cast<const char*>(data), actual);
    output->count += quint64(actual);
    return size;
}

mz_bool writeDeflateOutput(const void* data, int size, void* userData)
{
    DeflateOutputStream* output = static_cast<DeflateOutputStream*>(userData);
    if (!writeAll(output->output, static_cast<const char*>(data), size)) {
        output->failed = true;
        return MZ_FALSE;
    }
    output->crc = crc32Update(output->crc, static_cast<const char*>(data), size);
    output->count += quint64(size);
    return MZ_TRUE;
}

bool encodeDeflate(QIODevice* input, QIODevice* output, int level,
                   quint64* unpackedSize, quint32* unpackedCrc,
                   quint64* packedSize, quint32* packedCrc, QString* error)
{
    std::unique_ptr<tdefl_compressor> compressor(new tdefl_compressor);
    DeflateOutputStream packed = {};
    packed.output = output;
    const mz_uint probes = level == 0 ? TDEFL_DEFAULT_MAX_PROBES
        : mz_uint(1 + (level - 1) * 511);
    const int flags = int(probes) | (level == 0 ? TDEFL_FORCE_ALL_RAW_BLOCKS : 0);
    if (tdefl_init(compressor.get(), writeDeflateOutput, &packed, flags) !=
        TDEFL_STATUS_OKAY) {
        if (error) *error = QStringLiteral("Could not initialize the Deflate encoder.");
        return false;
    }
    QByteArray buffer(int(kIoChunk), Qt::Uninitialized);
    quint64 inputSize = 0;
    quint32 inputCrc = 0xffffffffU;
    for (;;) {
        const qint64 count = input->read(buffer.data(), buffer.size());
        if (count < 0) {
            if (error) *error = input->errorString();
            return false;
        }
        if (!count) break;
        inputCrc = crc32Update(inputCrc, buffer.constData(), count);
        inputSize += quint64(count);
        const tdefl_status status = tdefl_compress_buffer(
            compressor.get(), buffer.constData(), size_t(count), TDEFL_NO_FLUSH);
        if (status != TDEFL_STATUS_OKAY || packed.failed) {
            if (error) *error = QStringLiteral("Deflate compression failed.");
            return false;
        }
    }
    const tdefl_status finishStatus = tdefl_compress_buffer(
        compressor.get(), nullptr, 0, TDEFL_FINISH);
    if ((finishStatus != TDEFL_STATUS_DONE &&
         finishStatus != TDEFL_STATUS_OKAY) || packed.failed) {
        if (error) *error = QStringLiteral("Deflate finalization failed.");
        return false;
    }
    *unpackedSize = inputSize;
    *unpackedCrc = inputCrc ^ 0xffffffffU;
    *packedSize = packed.count;
    *packedCrc = packed.crc ^ 0xffffffffU;
    return true;
}

bool encodeDeflate64Stored(QIODevice* input, QIODevice* output,
                           quint64* unpackedSize, quint32* unpackedCrc,
                           quint64* packedSize, quint32* packedCrc,
                           QString* error)
{
    QByteArray block(65535, Qt::Uninitialized);
    quint64 inputSize = 0;
    quint64 outputSize = 0;
    quint32 inputCrc = 0xffffffffU;
    quint32 outputCrc = 0xffffffffU;
    char pending = 0;
    bool hasPending = false;
    bool emittedBlock = false;
    bool finished = false;
    while (!finished) {
        qint64 blockSize = 0;
        if (hasPending) {
            block[0] = pending;
            blockSize = 1;
            hasPending = false;
        }
        bool eof = false;
        while (blockSize < block.size()) {
            const qint64 count = input->read(block.data() + blockSize,
                                             block.size() - blockSize);
            if (count < 0) {
                if (error) *error = input->errorString();
                return false;
            }
            if (!count) {
                eof = true;
                break;
            }
            blockSize += count;
        }
        if (!eof && blockSize == block.size()) {
            const qint64 count = input->read(&pending, 1);
            if (count < 0) {
                if (error) *error = input->errorString();
                return false;
            }
            if (count) hasPending = true;
            else eof = true;
        }
        if (!blockSize && emittedBlock) {
            finished = true;
            break;
        }
        const quint16 length = quint16(blockSize);
        const quint16 inverseLength = quint16(~length);
        const char header[5] = {
            char(eof ? 1 : 0), char(length & 0xff), char(length >> 8),
            char(inverseLength & 0xff), char(inverseLength >> 8)
        };
        if (!writeAll(output, header, sizeof(header)) ||
            !writeAll(output, block.constData(), blockSize)) {
            if (error) *error = output->errorString();
            return false;
        }
        outputCrc = crc32Update(outputCrc, header, sizeof(header));
        outputCrc = crc32Update(outputCrc, block.constData(), blockSize);
        inputCrc = crc32Update(inputCrc, block.constData(), blockSize);
        inputSize += quint64(blockSize);
        outputSize += quint64(sizeof(header) + blockSize);
        emittedBlock = true;
        finished = eof;
    }
    *unpackedSize = inputSize;
    *unpackedCrc = inputCrc ^ 0xffffffffU;
    *packedSize = outputSize;
    *packedCrc = outputCrc ^ 0xffffffffU;
    return true;
}

bool encodeBzip2(QIODevice* input, QIODevice* output, int level,
                 quint64* unpackedSize, quint32* unpackedCrc,
                 quint64* packedSize, quint32* packedCrc, QString* error)
{
    bz_stream stream = {};
    if (BZ2_bzCompressInit(&stream, level, 0, 30) != BZ_OK) {
        if (error) *error = QStringLiteral("Could not initialize the BZip2 encoder.");
        return false;
    }
    QByteArray inputBuffer(int(kIoChunk), Qt::Uninitialized);
    QByteArray outputBuffer(int(kIoChunk), Qt::Uninitialized);
    quint64 inputSize = 0;
    quint64 outputSize = 0;
    quint32 inputCrc = 0xffffffffU;
    quint32 outputCrc = 0xffffffffU;
    bool success = true;
    for (;;) {
        const qint64 count = input->read(inputBuffer.data(), inputBuffer.size());
        if (count < 0) {
            if (error) *error = input->errorString();
            success = false;
            break;
        }
        if (!count) break;
        inputSize += quint64(count);
        inputCrc = crc32Update(inputCrc, inputBuffer.constData(), count);
        stream.next_in = inputBuffer.data();
        stream.avail_in = unsigned(count);
        while (stream.avail_in) {
            stream.next_out = outputBuffer.data();
            stream.avail_out = unsigned(outputBuffer.size());
            const int code = BZ2_bzCompress(&stream, BZ_RUN);
            const unsigned produced = unsigned(outputBuffer.size()) - stream.avail_out;
            if (produced) {
                if (!writeAll(output, outputBuffer.constData(), produced)) {
                    if (error) *error = output->errorString();
                    success = false;
                    break;
                }
                outputSize += produced;
                outputCrc = crc32Update(outputCrc, outputBuffer.constData(), produced);
            }
            if (code != BZ_RUN_OK) {
                if (error) *error = QStringLiteral("BZip2 compression failed (%1).").arg(code);
                success = false;
                break;
            }
        }
        if (!success) break;
    }
    if (success) {
        int code = BZ_FINISH_OK;
        while (code == BZ_FINISH_OK) {
            stream.next_out = outputBuffer.data();
            stream.avail_out = unsigned(outputBuffer.size());
            code = BZ2_bzCompress(&stream, BZ_FINISH);
            const unsigned produced = unsigned(outputBuffer.size()) - stream.avail_out;
            if (produced) {
                if (!writeAll(output, outputBuffer.constData(), produced)) {
                    if (error) *error = output->errorString();
                    success = false;
                    break;
                }
                outputSize += produced;
                outputCrc = crc32Update(outputCrc, outputBuffer.constData(), produced);
            }
            if (code != BZ_FINISH_OK && code != BZ_STREAM_END) {
                if (error) *error = QStringLiteral("BZip2 finalization failed (%1).").arg(code);
                success = false;
                break;
            }
        }
    }
    BZ2_bzCompressEnd(&stream);
    if (!success) return false;
    *unpackedSize = inputSize;
    *unpackedCrc = inputCrc ^ 0xffffffffU;
    *packedSize = outputSize;
    *packedCrc = outputCrc ^ 0xffffffffU;
    return true;
}

bool encodeLzma2(QIODevice* input, QIODevice* output, int level,
                 const QByteArray& sourceProperties,
                 quint64 expectedSize, QByteArray* properties,
                 quint64* unpackedSize, quint32* unpackedCrc,
                 quint64* packedSize, quint32* packedCrc, QString* error)
{
    CLzma2EncProps encoderProperties;
    Lzma2EncProps_Init(&encoderProperties);
    encoderProperties.lzmaProps.level = level;
    encoderProperties.lzmaProps.numThreads = 1;
    if (!sourceProperties.isEmpty()) {
        if (sourceProperties.size() != 1) {
            if (error) *error = QStringLiteral("The source LZMA2 properties are invalid.");
            return false;
        }
        const unsigned property = static_cast<unsigned char>(sourceProperties.at(0));
        if (property > 40) {
            if (error) *error = QStringLiteral("The source LZMA2 dictionary is invalid.");
            return false;
        }
        encoderProperties.lzmaProps.dictSize = property == 40
            ? UInt32(0xffffffffU)
            : UInt32(2U | (property & 1U)) << ((property >> 1) + 11);
        encoderProperties.lzmaProps.reduceSize = (std::numeric_limits<UInt64>::max)();
    } else {
        encoderProperties.lzmaProps.reduceSize = expectedSize;
    }
    Lzma2EncProps_Normalize(&encoderProperties);
    CLzma2EncHandle encoder = Lzma2Enc_Create(&g_Alloc, &g_Alloc);
    if (!encoder) {
        if (error) *error = QStringLiteral("Could not allocate the LZMA2 encoder.");
        return false;
    }
    SRes result = Lzma2Enc_SetProps(encoder, &encoderProperties);
    if (result == SZ_OK) {
        properties->append(char(Lzma2Enc_WriteProperties(encoder)));
        Lzma2Enc_SetDataSize(encoder, expectedSize);
        LzmaInputStream in = {};
        in.stream.Read = readLzmaInput;
        in.input = input;
        LzmaOutputStream out = {};
        out.stream.Write = writeLzmaOutput;
        out.output = output;
        result = Lzma2Enc_Encode2(encoder, &out.stream, nullptr, nullptr,
                                  &in.stream, nullptr, 0, nullptr);
        if (result == SZ_OK && !in.failed && !out.failed &&
            in.count == expectedSize) {
            *unpackedSize = in.count;
            *unpackedCrc = in.crc ^ 0xffffffffU;
            *packedSize = out.count;
            *packedCrc = out.crc ^ 0xffffffffU;
        } else if (result == SZ_OK) {
            result = SZ_ERROR_WRITE;
        }
    }
    Lzma2Enc_Destroy(encoder);
    if (result != SZ_OK) {
        if (error) *error = QStringLiteral("LZMA2 compression failed (%1).").arg(result);
        return false;
    }
    return true;
}

bool encodeLzma(QIODevice* input, QIODevice* output, int level,
                const QByteArray& sourceProperties,
                quint64 expectedSize, QByteArray* properties,
                quint64* unpackedSize, quint32* unpackedCrc,
                quint64* packedSize, quint32* packedCrc, QString* error)
{
    CLzmaEncProps encoderProperties;
    LzmaEncProps_Init(&encoderProperties);
    encoderProperties.level = level;
    encoderProperties.numThreads = 1;
    encoderProperties.writeEndMark = 1;
    if (!sourceProperties.isEmpty()) {
        if (sourceProperties.size() != LZMA_PROPS_SIZE) {
            if (error) *error = QStringLiteral("The source LZMA properties are invalid.");
            return false;
        }
        const unsigned encoded = static_cast<unsigned char>(sourceProperties.at(0));
        encoderProperties.lc = int(encoded % 9U);
        encoderProperties.lp = int((encoded / 9U) % 5U);
        encoderProperties.pb = int(encoded / 45U);
        encoderProperties.dictSize = 0;
        for (unsigned index = 0; index < 4; ++index)
            encoderProperties.dictSize |= UInt32(
                static_cast<unsigned char>(sourceProperties.at(1 + int(index))))
                << (index * 8);
        if (encoderProperties.pb > 4 || encoderProperties.dictSize < (1U << 12)) {
            if (error) *error = QStringLiteral("The source LZMA properties are out of range.");
            return false;
        }
        encoderProperties.reduceSize = (std::numeric_limits<UInt64>::max)();
    } else {
        encoderProperties.reduceSize = expectedSize;
    }
    LzmaEncProps_Normalize(&encoderProperties);

    CLzmaEncHandle encoder = LzmaEnc_Create(&g_Alloc);
    if (!encoder) {
        if (error) *error = QStringLiteral("Could not allocate the LZMA encoder.");
        return false;
    }
    SRes result = LzmaEnc_SetProps(encoder, &encoderProperties);
    if (result == SZ_OK) {
        Byte encodedProperties[LZMA_PROPS_SIZE];
        SizeT propertiesSize = sizeof(encodedProperties);
        result = LzmaEnc_WriteProperties(encoder, encodedProperties,
                                         &propertiesSize);
        if (result == SZ_OK && propertiesSize == sizeof(encodedProperties)) {
            properties->append(reinterpret_cast<const char*>(encodedProperties),
                               int(propertiesSize));
            LzmaEnc_SetDataSize(encoder, expectedSize);
            LzmaInputStream in = {};
            in.stream.Read = readLzmaInput;
            in.input = input;
            LzmaOutputStream out = {};
            out.stream.Write = writeLzmaOutput;
            out.output = output;
            result = LzmaEnc_Encode(encoder, &out.stream, &in.stream, nullptr,
                                    &g_Alloc, &g_Alloc);
            if (result == SZ_OK && !in.failed && !out.failed &&
                in.count == expectedSize) {
                *unpackedSize = in.count;
                *unpackedCrc = in.crc ^ 0xffffffffU;
                *packedSize = out.count;
                *packedCrc = out.crc ^ 0xffffffffU;
            } else if (result == SZ_OK) {
                result = SZ_ERROR_WRITE;
            }
        }
    }
    LzmaEnc_Destroy(encoder, &g_Alloc, &g_Alloc);
    if (result != SZ_OK) {
        if (error) *error = QStringLiteral("LZMA compression failed (%1).").arg(result);
        return false;
    }
    return true;
}

quint32 crc32Update(quint32 crc, const char* data, qint64 size)
{
    for (qint64 i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc;
}

quint32 crc32(const QByteArray& data)
{
    return crc32Update(0xffffffffU, data.constData(), data.size()) ^ 0xffffffffU;
}

void append32(QByteArray* data, quint32 value)
{
    for (unsigned i = 0; i < 4; ++i)
        data->append(char((value >> (i * 8)) & 0xffU));
}

void append64(QByteArray* data, quint64 value)
{
    for (unsigned i = 0; i < 8; ++i)
        data->append(char((value >> (i * 8)) & 0xffU));
}

void appendNumber(QByteArray* data, quint64 value)
{
    for (unsigned extra = 0; extra <= 7; ++extra) {
        const unsigned payloadBits = 7 - extra;
        const quint64 limit = payloadBits == 0
            ? 0 : (quint64(1) << (payloadBits + extra * 8));
        if (payloadBits && value < limit) {
            const quint8 prefix = extra == 0 ? 0
                : quint8(((quint16(1) << extra) - 1) << (8 - extra));
            data->append(char(prefix | quint8(value >> (extra * 8))));
            for (unsigned i = 0; i < extra; ++i)
                data->append(char((value >> (i * 8)) & 0xffU));
            return;
        }
    }
    data->append(char(0xff));
    append64(data, value);
}

void appendDigestSet(QByteArray* data, const std::vector<quint32>& digests)
{
    data->append(char(1));
    for (quint32 digest : digests) append32(data, digest);
}

QByteArray bitVector(const std::vector<bool>& bits)
{
    QByteArray bytes(int((bits.size() + 7) / 8), char(0));
    for (size_t i = 0; i < bits.size(); ++i) {
        if (bits[i])
            bytes[int(i / 8)] = char(static_cast<unsigned char>(bytes.at(int(i / 8))) |
                                     (0x80U >> (i % 8)));
    }
    return bytes;
}

void appendFileProperty(QByteArray* header, quint8 property,
                        const QByteArray& value)
{
    header->append(char(property));
    appendNumber(header, quint64(value.size()));
    header->append(value);
}

bool writeAll(QIODevice* output, const char* data, qint64 size)
{
    qint64 offset = 0;
    while (offset < size) {
        const qint64 written = output->write(data + offset, size - offset);
        if (written <= 0) return false;
        offset += written;
    }
    return true;
}

QString keyForName(const QByteArray& name)
{
    return QString::fromUtf8(name);
}

bool validName(const QByteArray& name, bool directory)
{
    QString value = QString::fromUtf8(name);
    if (name.isEmpty() || value.contains(QChar(0)) || value.toUtf8() != name)
        return false;
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (value.startsWith(QLatin1Char('/')) ||
        (value.size() > 1 && value.at(1) == QLatin1Char(':'))) return false;
    const QStringList parts = value.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    for (const QString& part : parts)
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) return false;
    if (directory && !value.endsWith(QLatin1Char('/'))) value.append(QLatin1Char('/'));
    return !value.isEmpty();
}

QByteArray normalizedName(QByteArray name, bool directory)
{
    name.replace('\\', '/');
    while (name.startsWith("./")) name.remove(0, 2);
    if (directory && !name.endsWith('/')) name.append('/');
    return name;
}

QByteArray utf16Name(const QByteArray& utf8Name)
{
    const QString value = QString::fromUtf8(utf8Name);
    QByteArray encoded;
    encoded.reserve((value.size() + 1) * 2);
    for (QChar character : value) {
        const ushort unit = character.unicode();
        encoded.append(char(unit & 0xff));
        encoded.append(char(unit >> 8));
    }
    encoded.append(char(0));
    encoded.append(char(0));
    return encoded;
}

class SevenZipCursor final {
public:
    explicit SevenZipCursor(const QByteArray& data)
        : data_(reinterpret_cast<const unsigned char*>(data.constData())),
          size_(size_t(data.size())) {}

    bool byte(unsigned char* value)
    {
        if (!value || position_ >= size_) return false;
        *value = data_[position_++];
        return true;
    }

    bool number(quint64* value)
    {
        unsigned char first = 0;
        if (!value || !byte(&first)) return false;
        quint64 result = 0;
        unsigned extra = 0;
        for (; extra < 8 && (first & (0x80U >> extra)); ++extra) {}
        if (extra == 8) {
            for (unsigned index = 0; index < 8; ++index) {
                unsigned char next = 0;
                if (!byte(&next)) return false;
                result |= quint64(next) << (index * 8);
            }
            *value = result;
            return true;
        }
        const unsigned highBits = 7 - extra;
        if (highBits)
            result |= quint64(first & ((1U << highBits) - 1U)) << (extra * 8);
        for (unsigned index = 0; index < extra; ++index) {
            unsigned char next = 0;
            if (!byte(&next)) return false;
            result |= quint64(next) << (index * 8);
        }
        *value = result;
        return true;
    }

    bool skip(quint64 count)
    {
        if (count > quint64(size_ - position_)) return false;
        position_ += size_t(count);
        return true;
    }

    bool bytes(quint64 count, QByteArray* value)
    {
        if (!value || count > quint64(size_ - position_) ||
            count > quint64((std::numeric_limits<int>::max)())) return false;
        *value = QByteArray(reinterpret_cast<const char*>(data_ + position_), int(count));
        position_ += size_t(count);
        return true;
    }

    bool atEnd() const { return position_ == size_; }

private:
    const unsigned char* data_;
    size_t size_;
    size_t position_ = 0;
};

bool readSevenZipDigests(SevenZipCursor* cursor, quint64 count,
                         std::vector<quint32>* values,
                         std::vector<bool>* definedValues = nullptr)
{
    unsigned char allDefined = 0;
    if (!cursor || !cursor->byte(&allDefined) || allDefined > 1) return false;
    std::vector<bool> defined(size_t(count), allDefined != 0);
    quint64 definedCount = count;
    if (!allDefined) {
        definedCount = 0;
        unsigned char byte = 0;
        for (quint64 index = 0; index < count; ++index) {
            if ((index & 7U) == 0 && !cursor->byte(&byte)) return false;
            defined[size_t(index)] = (byte & (0x80U >> (index & 7U))) != 0;
            if (defined[size_t(index)]) ++definedCount;
        }
    }
    if (definedCount > (std::numeric_limits<quint64>::max)() / 4) return false;
    if (values) values->assign(size_t(count), 0);
    if (definedValues) *definedValues = defined;
    for (quint64 index = 0; index < count; ++index) {
        if (!defined[size_t(index)]) continue;
        quint32 value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
            unsigned char next = 0;
            if (!cursor->byte(&next)) return false;
            value |= quint32(next) << (byte * 8);
        }
        if (values) (*values)[size_t(index)] = value;
    }
    return true;
}

struct SevenZipSourceVariant {
    struct Folder {
        struct Coder {
            QByteArray method;
            QByteArray properties;
            size_t inputBase = 0;
            size_t outputBase = 0;
            size_t inputCount = 1;
            size_t outputCount = 1;
        };
        struct Bind {
            size_t inputIndex = 0;
            size_t outputIndex = 0;
        };
        std::vector<Coder> coders;
        std::vector<Bind> binds;
        std::vector<size_t> packInputIndices;
        std::vector<quint64> outputSizes;
        std::vector<quint64> packedSizes;
        std::vector<quint32> packedCrcs;
        std::vector<bool> packedCrcDefined;
        size_t finalOutputIndex = 0;
        QByteArray method;
        QByteArray properties;
        quint64 packedSize = 0;
        quint64 unpackedSize = 0;
        quint32 unpackedCrc = 0;
        bool unpackedCrcDefined = false;
        std::vector<quint64> substreamSizes;
        std::vector<quint32> substreamCrcs;
        std::vector<bool> substreamCrcDefined;
    };
    struct File {
        QString name;
        QString stagedPath;
        QByteArray method;
        QByteArray properties;
        QByteArray aesProperties;
        quint64 size = 0;
        quint64 folder = 0;
        bool directory = false;
        bool hasStream = false;
    };
    QByteArray method;
    QByteArray properties;
    QByteArray headerMethod;
    QByteArray headerProperties;
    QByteArray headerAesProperties;
    quint64 packPosition = 0;
    quint64 packCount = 0;
    quint64 folderCount = 0;
    quint64 packedSize = 0;
    quint64 unpackedSize = 0;
    quint32 packCrc = 0;
    quint32 unpackedCrc = 0;
    std::map<QString, std::vector<quint64> > sourceFoldersByPath;
    std::vector<quint64> folderSubstreamCounts;
    std::vector<bool> folderCrcDefined;
    std::vector<Folder> folders;
    std::vector<File> files;
    std::vector<quint64> packedSizes;
    std::vector<quint32> packedCrcs;
    std::vector<quint32> folderCrcs;
    std::vector<bool> packedCrcDefined;
    bool packCrcDefined = false;
    bool unpackedCrcDefined = false;
    bool headerCompressed = false;
    bool headerEncrypted = false;
    bool dataEncrypted = false;
    bool solid = false;
    bool foundStreams = false;
};

bool isSevenZipFolderFilter(const QByteArray& method)
{
    return method == QByteArray::fromHex("03") ||
        method == QByteArray::fromHex("0303011b") ||
        method == QByteArray::fromHex("020302") ||
        method == QByteArray::fromHex("020304") ||
        method == QByteArray::fromHex("03030103") ||
        method == QByteArray::fromHex("03030205") ||
        method == QByteArray::fromHex("03030401") ||
        method == QByteArray::fromHex("03030501") ||
        method == QByteArray::fromHex("03030701") ||
        method == QByteArray::fromHex("03030805") ||
        method == QByteArray::fromHex("0a") ||
        method == QByteArray::fromHex("0b");
}

bool isSevenZipBcj2(const QByteArray& method)
{
    return method == QByteArray::fromHex("0303011b");
}

bool isSevenZipEncryption(const QByteArray& method)
{
    return method == QByteArray::fromHex("06f10701");
}

bool parseSevenZipFolder(SevenZipCursor* cursor,
                         SevenZipSourceVariant::Folder* folder,
                         SevenZipSourceVariant* variant)
{
    quint64 coderCount = 0;
    if (!cursor || !folder || !variant || !cursor->number(&coderCount) ||
        !coderCount || coderCount > 16) return false;
    quint64 totalInputs = 0;
    quint64 totalOutputs = 0;
    folder->coders.clear();
    for (quint64 coderIndex = 0; coderIndex < coderCount; ++coderIndex) {
        unsigned char flags = 0;
        if (!cursor->byte(&flags) || (flags & 0xC0U) || !(flags & 0x0FU))
            return false;
        QByteArray method;
        if (!cursor->bytes(flags & 0x0FU, &method)) return false;
        quint64 inputs = 1;
        quint64 outputs = 1;
        if (flags & 0x10U) {
            if (!cursor->number(&inputs) || !cursor->number(&outputs) ||
                !inputs || !outputs) return false;
        }
        if (inputs > 64 - totalInputs || outputs > 64 - totalOutputs)
            return false;
        SevenZipSourceVariant::Folder::Coder coder;
        coder.inputBase = size_t(totalInputs);
        coder.outputBase = size_t(totalOutputs);
        coder.inputCount = size_t(inputs);
        coder.outputCount = size_t(outputs);
        coder.method = method;
        folder->coders.push_back(coder);
        totalInputs += inputs;
        totalOutputs += outputs;
        QByteArray properties;
        if (flags & 0x20U) {
            quint64 propertySize = 0;
            if (!cursor->number(&propertySize) ||
                !cursor->bytes(propertySize, &properties)) return false;
        }
        folder->coders.back().properties = properties;
        if (isSevenZipEncryption(method)) variant->dataEncrypted = true;
        if (folder->method.isEmpty() && !isSevenZipFolderFilter(method) &&
            !isSevenZipEncryption(method)) {
            folder->method = method;
            folder->properties = properties;
        }
    }
    if (folder->method.isEmpty() && variant->dataEncrypted) {
        folder->method = QByteArray(1, char(0));
        folder->properties.clear();
    }
    if (!totalInputs || !totalOutputs || totalOutputs - 1 > totalInputs)
        return false;
    for (quint64 bind = 0; bind < totalOutputs - 1; ++bind) {
        quint64 input = 0;
        quint64 output = 0;
        if (!cursor->number(&input) || !cursor->number(&output) ||
            input >= totalInputs || output >= totalOutputs) return false;
        SevenZipSourceVariant::Folder::Bind bindPair;
        bindPair.inputIndex = size_t(input);
        bindPair.outputIndex = size_t(output);
        folder->binds.push_back(bindPair);
    }
    const quint64 unboundInputs = totalInputs - (totalOutputs - 1);
    if (unboundInputs > 1) {
        for (quint64 index = 0; index < unboundInputs; ++index) {
            quint64 input = 0;
            if (!cursor->number(&input) || input >= totalInputs) return false;
            folder->packInputIndices.push_back(size_t(input));
        }
    } else {
        for (size_t input = 0; input < size_t(totalInputs); ++input) {
            const bool bound = std::any_of(folder->binds.begin(), folder->binds.end(),
                [input](const SevenZipSourceVariant::Folder::Bind& pair) {
                    return pair.inputIndex == input;
                });
            if (!bound) folder->packInputIndices.push_back(input);
        }
    }
    if (folder->packInputIndices.size() != size_t(unboundInputs)) return false;
    std::vector<bool> outputBound(size_t(totalOutputs), false);
    for (const SevenZipSourceVariant::Folder::Bind& pair : folder->binds)
        outputBound[pair.outputIndex] = true;
    for (size_t output = 0; output < outputBound.size(); ++output)
        if (!outputBound[output]) folder->finalOutputIndex = output;
    folder->outputSizes.assign(size_t(totalOutputs), 0);
    variant->method = folder->method;
    variant->properties = folder->properties;
    return true;
}

bool parseSevenZipStreamsInfo(SevenZipCursor* cursor,
                              SevenZipSourceVariant* variant)
{
    quint64 folderCount = 0;
    bool unpackInfoSeen = false;
    for (;;) {
        unsigned char property = 0;
        if (!cursor->byte(&property)) return false;
        if (property == 0) {
            if (unpackInfoSeen && variant->folderSubstreamCounts.empty()) {
                variant->folderSubstreamCounts.assign(size_t(folderCount), 1);
                for (SevenZipSourceVariant::Folder& folder : variant->folders) {
                    folder.substreamSizes.push_back(folder.unpackedSize);
                    folder.substreamCrcs.push_back(folder.unpackedCrc);
                    folder.substreamCrcDefined.push_back(folder.unpackedCrcDefined);
                }
            }
            if (unpackInfoSeen) {
                if (variant->packedSizes.size() != variant->packCount ||
                    variant->folders.size() != folderCount) return false;
                size_t packIndex = 0;
                for (SevenZipSourceVariant::Folder& folder : variant->folders) {
                    folder.packedSizes.clear();
                    folder.packedCrcs.clear();
                    folder.packedCrcDefined.clear();
                    folder.packedSize = 0;
                    for (size_t input = 0; input < folder.packInputIndices.size(); ++input) {
                        if (packIndex >= variant->packedSizes.size() ||
                            variant->packedSizes[packIndex] >
                                (std::numeric_limits<quint64>::max)() - folder.packedSize)
                            return false;
                        folder.packedSizes.push_back(variant->packedSizes[packIndex]);
                        folder.packedSize += variant->packedSizes[packIndex];
                        const bool crcDefined = packIndex <
                            variant->packedCrcDefined.size() &&
                            variant->packedCrcDefined[packIndex];
                        folder.packedCrcDefined.push_back(crcDefined);
                        folder.packedCrcs.push_back(crcDefined
                            ? variant->packedCrcs[packIndex] : 0);
                        ++packIndex;
                    }
                }
                if (packIndex != variant->packedSizes.size()) return false;
            }
            return unpackInfoSeen;
        }
        if (property == 6) {
            quint64 position = 0;
            quint64 packCount = 0;
            if (!cursor->number(&position) || !cursor->number(&packCount) ||
                packCount > 1000000) return false;
            variant->packPosition = position;
            variant->packCount = packCount;
            for (;;) {
                if (!cursor->byte(&property)) return false;
                if (property == 0) break;
                if (property == 9) {
                    variant->packedSizes.assign(size_t(packCount), 0);
                    for (quint64 index = 0; index < packCount; ++index) {
                        quint64 size = 0;
                        if (!cursor->number(&size)) return false;
                        variant->packedSizes[size_t(index)] = size;
                        if (!index) variant->packedSize = size;
                    }
                } else if (property == 10) {
                    std::vector<quint32> values;
                    if (!readSevenZipDigests(cursor, packCount, &values,
                                             &variant->packedCrcDefined)) return false;
                    variant->packedCrcs = values;
                    if (!values.empty()) {
                        variant->packCrc = values[0];
                        variant->packCrcDefined = true;
                    }
                } else {
                    return false;
                }
            }
        } else if (property == 7) {
            if (unpackInfoSeen || !cursor->byte(&property) || property != 11 ||
                !cursor->number(&folderCount) || folderCount > 1000000 ||
                !cursor->byte(&property) || property != 0) return false;
            for (quint64 folder = 0; folder < folderCount; ++folder) {
                SevenZipSourceVariant folderVariant;
                SevenZipSourceVariant::Folder sourceFolder;
                if (!parseSevenZipFolder(cursor, &sourceFolder, &folderVariant) ||
                    sourceFolder.method.isEmpty()) {
                    return false;
                }
                sourceFolder.method = folderVariant.method;
                sourceFolder.properties = folderVariant.properties;
                variant->dataEncrypted = variant->dataEncrypted ||
                    folderVariant.dataEncrypted;
                variant->folders.push_back(sourceFolder);
                if (!folder) {
                    variant->method = folderVariant.method;
                    variant->properties = folderVariant.properties;
                }
            }
            if (!cursor->byte(&property) || property != 12) return false;
            variant->folderCount = folderCount;
            for (quint64 folder = 0; folder < folderCount; ++folder) {
                SevenZipSourceVariant::Folder& sourceFolder =
                    variant->folders[size_t(folder)];
                if (sourceFolder.outputSizes.empty() ||
                    sourceFolder.finalOutputIndex >= sourceFolder.outputSizes.size())
                    return false;
                for (quint64& outputSize : sourceFolder.outputSizes)
                    if (!cursor->number(&outputSize)) return false;
                sourceFolder.unpackedSize =
                    sourceFolder.outputSizes[sourceFolder.finalOutputIndex];
                if (!folder) variant->unpackedSize = sourceFolder.unpackedSize;
            }
            for (;;) {
                if (!cursor->byte(&property)) return false;
                if (property == 0) break;
                if (property != 10) return false;
                std::vector<quint32> values;
                if (!readSevenZipDigests(cursor, folderCount, &values,
                                         &variant->folderCrcDefined)) return false;
                variant->folderCrcs = values;
                for (quint64 folder = 0; folder < folderCount; ++folder) {
                    if (size_t(folder) < variant->folderCrcDefined.size()) {
                        variant->folders[size_t(folder)].unpackedCrcDefined =
                            variant->folderCrcDefined[size_t(folder)];
                        if (variant->folders[size_t(folder)].unpackedCrcDefined)
                            variant->folders[size_t(folder)].unpackedCrc =
                                values[size_t(folder)];
                    }
                }
                if (!values.empty() && variant->folderCrcDefined[0]) {
                    variant->unpackedCrc = values[0];
                    variant->unpackedCrcDefined = true;
                }
            }
            unpackInfoSeen = true;
            variant->foundStreams = folderCount != 0;
        } else if (property == 8) {
            if (!unpackInfoSeen) return false;
            std::vector<quint64> substreams(size_t(folderCount), 1);
            for (;;) {
                if (!cursor->byte(&property)) return false;
                if (property == 0) break;
                if (property == 13) {
                    for (quint64& count : substreams)
                        if (!cursor->number(&count) || count > 1000000) return false;
                } else if (property == 9) {
                    for (size_t folder = 0; folder < substreams.size(); ++folder) {
                        const quint64 count = substreams[folder];
                        std::vector<quint64>& sizes =
                            variant->folders[folder].substreamSizes;
                        sizes.assign(size_t(count), 0);
                        quint64 known = 0;
                        for (quint64 index = 0; index + 1 < count; ++index) {
                            quint64 size = 0;
                            if (!cursor->number(&size) ||
                                size > (std::numeric_limits<quint64>::max)() - known)
                                return false;
                            sizes[size_t(index)] = size;
                            known += size;
                        }
                        if (count) {
                            if (known > variant->folders[folder].unpackedSize)
                                return false;
                            sizes[size_t(count - 1)] =
                                variant->folders[folder].unpackedSize - known;
                        }
                    }
                } else if (property == 10) {
                    quint64 digestCount = 0;
                    for (size_t folder = 0; folder < substreams.size(); ++folder) {
                        quint64 count = substreams[folder];
                        if (count == 1 && folder < variant->folderCrcDefined.size() &&
                            variant->folderCrcDefined[folder]) --count;
                        if (count > (std::numeric_limits<quint64>::max)() -
                                    digestCount) return false;
                        digestCount += count;
                    }
                    std::vector<quint32> values;
                    std::vector<bool> defined;
                    if (!readSevenZipDigests(cursor, digestCount, &values,
                                             &defined)) return false;
                    size_t digestIndex = 0;
                    for (size_t folder = 0; folder < substreams.size(); ++folder) {
                        const quint64 count = substreams[folder];
                        SevenZipSourceVariant::Folder& sourceFolder =
                            variant->folders[folder];
                        if (sourceFolder.substreamSizes.empty()) {
                            sourceFolder.substreamSizes.assign(size_t(count), 0);
                            if (count == 1)
                                sourceFolder.substreamSizes[0] = sourceFolder.unpackedSize;
                            else
                                return false;
                        }
                        sourceFolder.substreamCrcs.assign(size_t(count), 0);
                        sourceFolder.substreamCrcDefined.assign(size_t(count), false);
                        for (quint64 index = 0; index < count; ++index) {
                            if (count == 1 && sourceFolder.unpackedCrcDefined) {
                                sourceFolder.substreamCrcs[0] = sourceFolder.unpackedCrc;
                                sourceFolder.substreamCrcDefined[0] = true;
                                continue;
                            }
                            if (digestIndex >= values.size()) return false;
                            sourceFolder.substreamCrcs[size_t(index)] =
                                values[digestIndex];
                            sourceFolder.substreamCrcDefined[size_t(index)] =
                                defined[digestIndex];
                            ++digestIndex;
                        }
                    }
                    if (digestIndex != values.size()) return false;
                } else {
                    return false;
                }
            }
            variant->solid = std::any_of(substreams.begin(), substreams.end(),
                [](quint64 count) { return count > 1; });
            variant->folderSubstreamCounts = substreams;
            for (size_t folder = 0; folder < substreams.size(); ++folder) {
                SevenZipSourceVariant::Folder& sourceFolder =
                    variant->folders[folder];
                if (sourceFolder.substreamSizes.empty()) {
                    if (substreams[folder] != 1) return false;
                    sourceFolder.substreamSizes.push_back(sourceFolder.unpackedSize);
                }
                if (sourceFolder.substreamSizes.size() != substreams[folder])
                    return false;
                if (sourceFolder.substreamCrcs.empty()) {
                    sourceFolder.substreamCrcs.assign(size_t(substreams[folder]), 0);
                    sourceFolder.substreamCrcDefined.assign(
                        size_t(substreams[folder]), false);
                    if (substreams[folder] == 1 && sourceFolder.unpackedCrcDefined) {
                        sourceFolder.substreamCrcs[0] = sourceFolder.unpackedCrc;
                        sourceFolder.substreamCrcDefined[0] = true;
                    }
                }
            }
        } else {
            return false;
        }
    }
}

bool parseSevenZipMainHeader(const QByteArray& header,
                             SevenZipSourceVariant* variant)
{
    SevenZipCursor cursor(header);
    unsigned char property = 0;
    if (!variant || !cursor.byte(&property) || property != 1U) return false;
    std::vector<QString> fileNames;
    std::vector<bool> emptyStreams;
    std::vector<bool> emptyFiles;
    for (;;) {
        if (!cursor.byte(&property)) return false;
        if (property == 0) break;
        if (property == 4) {
            if (!parseSevenZipStreamsInfo(&cursor, variant)) return false;
        } else if (property == 2) {
            for (;;) {
                unsigned char fileProperty = 0;
                if (!cursor.byte(&fileProperty)) return false;
                if (fileProperty == 0) break;
                quint64 size = 0;
                if (!cursor.number(&size) || !cursor.skip(size)) return false;
            }
        } else if (property == 5) {
            quint64 count = 0;
            if (!cursor.number(&count) || count > 1000000) return false;
            fileNames.resize(size_t(count));
            emptyStreams.assign(size_t(count), false);
            emptyFiles.assign(size_t(count), false);
            bool namesSeen = false;
            for (;;) {
                unsigned char fileProperty = 0;
                if (!cursor.byte(&fileProperty)) return false;
                if (fileProperty == 0) break;
                quint64 size = 0;
                QByteArray data;
                if (!cursor.number(&size) || !cursor.bytes(size, &data))
                    return false;
                if (fileProperty == 14) {
                    SevenZipCursor bits(data);
                    unsigned char current = 0;
                    unsigned available = 0;
                    for (quint64 index = 0; index < count; ++index) {
                        if (!available) {
                            if (!bits.byte(&current)) return false;
                            available = 8;
                        }
                        emptyStreams[size_t(index)] = (current & 0x80U) != 0;
                        current = static_cast<unsigned char>(current << 1);
                        --available;
                    }
                    if (!bits.atEnd()) return false;
                } else if (fileProperty == 15) {
                    size_t emptyCount = 0;
                    for (bool emptyStream : emptyStreams)
                        if (emptyStream) ++emptyCount;
                    SevenZipCursor bits(data);
                    unsigned char current = 0;
                    unsigned available = 0;
                    size_t fileIndex = 0;
                    for (size_t index = 0; index < emptyCount; ++index) {
                        if (!available) {
                            if (!bits.byte(&current)) return false;
                            available = 8;
                        }
                        while (fileIndex < emptyStreams.size() &&
                               !emptyStreams[fileIndex]) ++fileIndex;
                        if (fileIndex >= emptyStreams.size()) return false;
                        emptyFiles[fileIndex++] = (current & 0x80U) != 0;
                        current = static_cast<unsigned char>(current << 1);
                        --available;
                    }
                    if (!bits.atEnd()) return false;
                } else if (fileProperty == 17) {
                    SevenZipCursor names(data);
                    unsigned char external = 0;
                    if (!names.byte(&external) || external != 0 || namesSeen)
                        return false;
                    namesSeen = true;
                    for (quint64 index = 0; index < count; ++index) {
                        std::vector<ushort> units;
                        for (;;) {
                            unsigned char low = 0;
                            unsigned char high = 0;
                            if (!names.byte(&low) || !names.byte(&high)) return false;
                            const ushort unit = ushort(low) | (ushort(high) << 8);
                            if (!unit) break;
                            units.push_back(unit);
                        }
                        fileNames[size_t(index)] = QString::fromUtf16(
                            units.empty() ? nullptr : units.data(),
                            int(units.size()));
                    }
                    if (!names.atEnd()) return false;
                }
            }
            if (count && !namesSeen) return false;
        } else {
            return false;
        }
    }
    if (!cursor.atEnd()) return false;
    if (variant->foundStreams) {
        if (fileNames.size() != emptyStreams.size() ||
            variant->folderSubstreamCounts.empty()) return false;
        quint64 folder = 0;
        quint64 remaining = variant->folderSubstreamCounts[0];
        for (size_t index = 0; index < fileNames.size(); ++index) {
            SevenZipSourceVariant::File file;
            file.name = fileNames[index];
            if (file.name.isEmpty()) return false;
            if (emptyStreams[index]) {
                file.directory = !emptyFiles[index];
            } else {
                while (!remaining && folder + 1 <
                       variant->folderSubstreamCounts.size()) {
                    ++folder;
                    remaining = variant->folderSubstreamCounts[size_t(folder)];
                }
                if (!remaining || folder >= variant->folders.size() ||
                    variant->folderSubstreamCounts[size_t(folder)] < remaining)
                    return false;
                const quint64 substreamIndex =
                    variant->folderSubstreamCounts[size_t(folder)] - remaining;
                if (substreamIndex >=
                    variant->folders[size_t(folder)].substreamSizes.size())
                    return false;
                file.folder = folder;
                file.method = variant->folders[size_t(folder)].method;
                file.properties = variant->folders[size_t(folder)].properties;
                for (const SevenZipSourceVariant::Folder::Coder& coder :
                     variant->folders[size_t(folder)].coders) {
                    if (isSevenZipEncryption(coder.method))
                        file.aesProperties = coder.properties;
                }
                file.size = variant->folders[size_t(folder)].substreamSizes[
                    size_t(substreamIndex)];
                file.hasStream = true;
                QString key = file.name;
                key.replace(QLatin1Char('\\'), QLatin1Char('/'));
                variant->sourceFoldersByPath[key].push_back(folder);
                --remaining;
            }
            variant->files.push_back(file);
        }
        if (remaining || folder + 1 !=
            variant->folderSubstreamCounts.size()) return false;
    } else {
        for (size_t index = 0; index < fileNames.size(); ++index) {
            if (!emptyStreams[index] || fileNames[index].isEmpty()) return false;
            SevenZipSourceVariant::File file;
            file.name = fileNames[index];
            file.directory = !emptyFiles[index];
            variant->files.push_back(file);
        }
    }
    return true;
}

bool decodeSevenZipFolder(const SevenZipSourceVariant::Folder& folder,
                          const std::vector<std::vector<std::uint8_t> >& packed,
                          const QString& password,
                          std::vector<std::uint8_t>* unpacked);

bool decodeSevenZipEncodedHeader(QFile* input, const QByteArray& encoded,
                                 quint64 nextHeaderOffset,
                                 const QString& password,
                                 SevenZipSourceVariant* variant,
                                 QByteArray* decoded, bool* locked)
{
    if (!input || !variant || !decoded || !locked) return false;
    *locked = false;
    SevenZipCursor cursor(encoded);
    SevenZipSourceVariant headerVariant;
    const bool streamsParsed = parseSevenZipStreamsInfo(&cursor, &headerVariant);
    if (!streamsParsed) qWarning() << "7z encrypted header parse streams failed";
    if (!streamsParsed || !cursor.atEnd() ||
        !headerVariant.foundStreams || headerVariant.packCount != 1 ||
        headerVariant.folderCount != 1 ||
        headerVariant.packedSize > quint64((std::numeric_limits<int>::max)()) ||
        !headerVariant.unpackedSize ||
        headerVariant.unpackedSize > 256U * 1024U * 1024U ||
        headerVariant.unpackedSize >
            quint64((std::numeric_limits<int>::max)()) ||
        headerVariant.packPosition > nextHeaderOffset ||
        headerVariant.packedSize > nextHeaderOffset -
            headerVariant.packPosition ||
        headerVariant.packPosition > quint64(input->size() - 32) ||
        headerVariant.packedSize > quint64(input->size() - 32) -
            headerVariant.packPosition ||
        !input->seek(qint64(32 + headerVariant.packPosition))) {
        qWarning() << "7z encrypted header structure rejected"
                   << cursor.atEnd() << headerVariant.foundStreams
                   << headerVariant.packCount << headerVariant.folderCount
                   << headerVariant.packedSize << headerVariant.unpackedSize
                   << headerVariant.packPosition << nextHeaderOffset
                   << input->size();
        return false;
    }

    const QByteArray packed = input->read(qint64(headerVariant.packedSize));
    if (packed.size() != int(headerVariant.packedSize) ||
        (headerVariant.packCrcDefined &&
         crc32(packed) != headerVariant.packCrc)) {
        qWarning() << "7z encrypted header packed stream rejected";
        return false;
    }
    if (headerVariant.folders.size() != 1) {
        qWarning() << "7z encrypted header folder count" << headerVariant.folders.size();
        return false;
    }
    for (const SevenZipSourceVariant::Folder::Coder& coder :
         headerVariant.folders[0].coders) {
        if (isSevenZipEncryption(coder.method)) {
            variant->headerEncrypted = true;
            variant->headerAesProperties = coder.properties;
        }
    }
    if (variant->headerEncrypted && password.isNull()) {
        qWarning() << "7z encrypted header recognized";
        *locked = true;
        return true;
    }
    std::vector<std::vector<std::uint8_t> > packedStreams(1);
    packedStreams[0].assign(packed.constBegin(), packed.constEnd());
    std::vector<std::uint8_t> output;
    if (!decodeSevenZipFolder(headerVariant.folders[0], packedStreams,
                              password, &output) ||
        output.size() != size_t(headerVariant.unpackedSize) ||
        (headerVariant.unpackedCrcDefined &&
         crc32Bytes(output.data(), output.size()) != headerVariant.unpackedCrc))
        return false;
    *decoded = QByteArray(reinterpret_cast<const char*>(output.data()),
                          int(output.size()));
    variant->headerCompressed = true;
    variant->headerMethod = headerVariant.method;
    variant->headerProperties = headerVariant.properties;
    return true;
}

bool readSevenZipSourceVariant(const QString& archivePath,
                              const QString& password,
                              SevenZipSourceVariant* variant)
{
    QFile input(archivePath);
    if (!variant || !input.open(QIODevice::ReadOnly) || input.size() < 32)
        return false;
    QByteArray start = input.read(32);
    if (start.size() != 32 ||
        std::memcmp(start.constData(), kSignature, sizeof(kSignature)) != 0 ||
        static_cast<unsigned char>(start.at(6)) != 0) {
        return false;
    }
    const quint32 startCrc = quint32(static_cast<unsigned char>(start.at(8))) |
        (quint32(static_cast<unsigned char>(start.at(9))) << 8) |
        (quint32(static_cast<unsigned char>(start.at(10))) << 16) |
        (quint32(static_cast<unsigned char>(start.at(11))) << 24);
    if (crc32(start.mid(12)) != startCrc) {
        return false;
    }
    quint64 nextOffset = 0;
    quint64 nextSize = 0;
    for (unsigned index = 0; index < 8; ++index) {
        nextOffset |= quint64(static_cast<unsigned char>(start.at(12 + int(index))))
                      << (index * 8);
        nextSize |= quint64(static_cast<unsigned char>(start.at(20 + int(index))))
                    << (index * 8);
    }
    if (nextOffset > quint64(input.size() - 32) ||
        nextSize > quint64(input.size() - 32) - nextOffset ||
        nextSize > 64U * 1024U * 1024U ||
        nextSize > quint64((std::numeric_limits<int>::max)()) ||
        !input.seek(qint64(32 + nextOffset))) {
        return false;
    }
    const QByteArray nextHeader = input.read(qint64(nextSize));
    const quint32 nextCrc = quint32(static_cast<unsigned char>(start.at(28))) |
        (quint32(static_cast<unsigned char>(start.at(29))) << 8) |
        (quint32(static_cast<unsigned char>(start.at(30))) << 16) |
        (quint32(static_cast<unsigned char>(start.at(31))) << 24);
    if (nextHeader.size() != int(nextSize) || crc32(nextHeader) != nextCrc) {
        return false;
    }
    unsigned char property = 0;
    if (nextHeader.isEmpty()) return false;
    property = static_cast<unsigned char>(nextHeader.at(0));
    QByteArray mainHeader = nextHeader;
    if (property == 0x17U) {
        bool locked = false;
        if (!decodeSevenZipEncodedHeader(&input, nextHeader.mid(1), nextOffset,
                                         password,
                                         variant,
                                         &mainHeader, &locked)) {
            qWarning() << "7z encoded header function failed";
            return false;
        }
        if (locked) return true;
    } else if (property != 1U) {
        return false;
    }
    return parseSevenZipMainHeader(mainHeader, variant);
}

quint32 crc32Bytes(const std::uint8_t* data, size_t size)
{
    quint32 crc = 0xffffffffU;
    size_t offset = 0;
    while (offset < size) {
        const size_t amount = std::min<size_t>(size - offset,
                                               size_t((std::numeric_limits<qint64>::max)()));
        crc = crc32Update(crc, reinterpret_cast<const char*>(data + offset),
                          qint64(amount));
        offset += amount;
    }
    return crc ^ 0xffffffffU;
}

bool applySevenZipFolderFilter(const QByteArray& method,
                               const QByteArray& properties,
                               std::vector<std::uint8_t>* data)
{
    if (!data) return false;
    Byte* bytes = data->empty() ? nullptr : data->data();
    const SizeT size = SizeT(data->size());
    if (method == QByteArray::fromHex("03")) {
        if (properties.size() != 1) return false;
        Byte state[DELTA_STATE_SIZE];
        Delta_Init(state);
        Delta_Decode(state, Byte(properties.at(0)) + 1, bytes, size);
    } else if (method == QByteArray::fromHex("020302") ||
               method == QByteArray::fromHex("020304")) {
        if (!properties.isEmpty()) return false;
        const size_t wordSize = method.endsWith(char(2)) ? 2 : 4;
        for (size_t offset = 0; offset + wordSize <= data->size(); offset += wordSize)
            std::reverse(data->begin() + std::ptrdiff_t(offset),
                         data->begin() + std::ptrdiff_t(offset + wordSize));
    } else if (method == QByteArray::fromHex("03030103")) {
        if (!properties.isEmpty()) return false;
        UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
        z7_BranchConvSt_X86_Dec(bytes, size, 0, &state);
    } else if (method == QByteArray::fromHex("03030205")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_PPC_Dec(bytes, size, 0);
    } else if (method == QByteArray::fromHex("03030401")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_IA64_Dec(bytes, size, 0);
    } else if (method == QByteArray::fromHex("03030501")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_ARM_Dec(bytes, size, 0);
    } else if (method == QByteArray::fromHex("03030701")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_ARMT_Dec(bytes, size, 0);
    } else if (method == QByteArray::fromHex("03030805")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_SPARC_Dec(bytes, size, 0);
    } else if (method == QByteArray::fromHex("0a") ||
               method == QByteArray::fromHex("0b")) {
        UInt32 pc = 0;
        if (properties.size() == 4)
            pc = qFromLittleEndian<UInt32>(
                reinterpret_cast<const uchar*>(properties.constData()));
        else if (!properties.isEmpty())
            return false;
        if (method == QByteArray::fromHex("0a")) {
            if (pc & 3U) return false;
            z7_BranchConv_ARM64_Dec(bytes, size, pc);
        } else {
            if (pc & 1U) return false;
            z7_BranchConv_RISCV_Dec(bytes, size, pc);
        }
    } else {
        return false;
    }
    return true;
}

void* allocateSevenZipMemory(ISzAllocPtr, size_t size)
{
    return size ? std::malloc(size) : nullptr;
}

void releaseSevenZipMemory(ISzAllocPtr, void* address)
{
    std::free(address);
}

const ISzAlloc kSevenZipAllocator = {
    allocateSevenZipMemory, releaseSevenZipMemory
};

bool decodeSevenZipLzma(const std::vector<std::uint8_t>& packed,
                        const QByteArray& properties, size_t outputSize,
                        std::vector<std::uint8_t>* output)
{
    if (!output || outputSize > (std::numeric_limits<SizeT>::max)() ||
        packed.size() > (std::numeric_limits<SizeT>::max)() ||
        properties.size() != LZMA_PROPS_SIZE) return false;
    CLzmaProps decodedProperties = {};
    if (LzmaProps_Decode(&decodedProperties,
            reinterpret_cast<const Byte*>(properties.constData()),
            unsigned(properties.size())) != SZ_OK) return false;
    std::vector<std::uint8_t> decoded;
    try {
        decoded.resize(outputSize);
    } catch (...) {
        return false;
    }
    Byte empty = 0;
    SizeT destinationSize = SizeT(decoded.size());
    SizeT sourceSize = SizeT(packed.size());
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes result = LzmaDecode(
        decoded.empty() ? &empty : decoded.data(), &destinationSize,
        packed.empty() ? &empty : packed.data(), &sourceSize,
        reinterpret_cast<const Byte*>(properties.constData()),
        unsigned(properties.size()), LZMA_FINISH_END, &status,
        &kSevenZipAllocator);
    if (result != SZ_OK || destinationSize != outputSize ||
        sourceSize != packed.size()) return false;
    output->swap(decoded);
    return true;
}

bool decodeSevenZipLzma2(const std::vector<std::uint8_t>& packed,
                         const QByteArray& properties, size_t outputSize,
                         std::vector<std::uint8_t>* output)
{
    if (!output || properties.size() != 1 ||
        static_cast<unsigned char>(properties.at(0)) > 40 ||
        outputSize > (std::numeric_limits<SizeT>::max)() ||
        packed.size() > (std::numeric_limits<SizeT>::max)()) return false;
    std::vector<std::uint8_t> decoded;
    try {
        decoded.resize(outputSize);
    } catch (...) {
        return false;
    }
    Byte empty = 0;
    SizeT destinationSize = SizeT(decoded.size());
    SizeT sourceSize = SizeT(packed.size());
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes result = Lzma2Decode(
        decoded.empty() ? &empty : decoded.data(), &destinationSize,
        packed.empty() ? &empty : packed.data(), &sourceSize,
        static_cast<Byte>(properties.at(0)), LZMA_FINISH_END, &status,
        &kSevenZipAllocator);
    if (result != SZ_OK || destinationSize != outputSize ||
        sourceSize != packed.size() || status != LZMA_STATUS_FINISHED_WITH_MARK)
        return false;
    output->swap(decoded);
    return true;
}

struct SevenZipPpmdInput {
    IByteIn byteInput;
    const Byte* data;
    size_t size;
    size_t position;
    bool exhausted;
};

Byte readSevenZipPpmdByte(IByteInPtr byteInput)
{
    SevenZipPpmdInput* input = reinterpret_cast<SevenZipPpmdInput*>(
        const_cast<IByteIn*>(byteInput));
    if (input->position >= input->size) {
        input->exhausted = true;
        return 0;
    }
    return input->data[input->position++];
}

bool decodeSevenZipPpmd(const std::vector<std::uint8_t>& packed,
                        const QByteArray& properties, size_t outputSize,
                        std::vector<std::uint8_t>* output)
{
    if (!output || properties.size() != 5 ||
        outputSize > (std::numeric_limits<SizeT>::max)()) return false;
    const unsigned order = static_cast<unsigned char>(properties.at(0));
    const Byte* propertyBytes = reinterpret_cast<const Byte*>(properties.constData());
    const UInt32 memorySize = UInt32(propertyBytes[1]) |
        (UInt32(propertyBytes[2]) << 8) | (UInt32(propertyBytes[3]) << 16) |
        (UInt32(propertyBytes[4]) << 24);
    if (order < PPMD7_MIN_ORDER || order > PPMD7_MAX_ORDER ||
        memorySize < PPMD7_MIN_MEM_SIZE || memorySize > 512U * 1024U * 1024U)
        return false;
    std::vector<std::uint8_t> decoded;
    try {
        decoded.resize(outputSize);
    } catch (...) {
        return false;
    }
    CPpmd7 model;
    Ppmd7_Construct(&model);
    if (!Ppmd7_Alloc(&model, memorySize, &kSevenZipAllocator)) return false;
    Ppmd7_Init(&model, order);
    static const Byte empty = 0;
    SevenZipPpmdInput input = {{readSevenZipPpmdByte},
        packed.empty() ? &empty : packed.data(), packed.size(), 0, false};
    model.rc.dec.Stream = &input.byteInput;
    bool valid = Ppmd7z_RangeDec_Init(&model.rc.dec) && !input.exhausted;
    for (size_t index = 0; valid && index < decoded.size(); ++index) {
        const int symbol = Ppmd7z_DecodeSymbol(&model);
        if (symbol < 0 || input.exhausted) valid = false;
        else decoded[index] = static_cast<std::uint8_t>(symbol);
    }
    valid = valid && Ppmd7z_RangeDec_IsFinishedOK(&model.rc.dec) &&
        !input.exhausted && input.position == packed.size();
    Ppmd7_Free(&model, &kSevenZipAllocator);
    if (!valid) return false;
    output->swap(decoded);
    return true;
}

bool decodeSevenZipDeflate(const std::vector<std::uint8_t>& packed,
                           size_t outputSize,
                           std::vector<std::uint8_t>* output)
{
    if (!output) return false;
    std::vector<std::uint8_t> decoded;
    try {
        decoded.resize(outputSize);
    } catch (...) {
        return false;
    }
    static const std::uint8_t empty = 0;
    const size_t result = tinfl_decompress_mem_to_mem(
        decoded.empty() ? const_cast<std::uint8_t*>(&empty) : decoded.data(),
        decoded.size(), packed.empty() ? &empty : packed.data(), packed.size(),
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (result != outputSize) return false;
    output->swap(decoded);
    return true;
}

bool decodeSevenZipBzip2(const std::vector<std::uint8_t>& packed,
                         size_t outputSize,
                         std::vector<std::uint8_t>* output)
{
    if (!output || packed.size() > UINT_MAX || outputSize > UINT_MAX) return false;
    std::vector<std::uint8_t> decoded;
    try {
        decoded.resize(outputSize);
    } catch (...) {
        return false;
    }
    bz_stream stream = {};
    if (BZ2_bzDecompressInit(&stream, 0, 0) != BZ_OK) return false;
    static char empty = 0;
    stream.next_in = packed.empty()
        ? &empty : reinterpret_cast<char*>(const_cast<std::uint8_t*>(packed.data()));
    stream.avail_in = static_cast<unsigned int>(packed.size());
    stream.next_out = decoded.empty() ? &empty : reinterpret_cast<char*>(decoded.data());
    stream.avail_out = static_cast<unsigned int>(decoded.size());
    const int result = BZ2_bzDecompress(&stream);
    const bool valid = result == BZ_STREAM_END &&
        stream.total_in_lo32 == static_cast<unsigned int>(packed.size()) &&
        stream.total_out_lo32 == static_cast<unsigned int>(decoded.size());
    BZ2_bzDecompressEnd(&stream);
    if (!valid) return false;
    output->swap(decoded);
    return true;
}

bool decodeSevenZipFolder(const SevenZipSourceVariant::Folder& folder,
                          const std::vector<std::vector<std::uint8_t> >& packed,
                          const QString& password,
                          std::vector<std::uint8_t>* unpacked)
{
    if (!unpacked || folder.unpackedSize > unpacked->max_size() ||
        folder.packInputIndices.size() == 0 ||
        packed.size() != folder.packInputIndices.size() ||
        folder.outputSizes.empty() ||
        folder.finalOutputIndex >= folder.outputSizes.size()) return false;
    std::vector<std::vector<std::uint8_t> > outputs(folder.outputSizes.size());
    std::vector<bool> outputReady(folder.outputSizes.size(), false);
    std::vector<bool> coderDone(folder.coders.size(), false);
    size_t completed = 0;
    while (completed < folder.coders.size()) {
        bool progressed = false;
        for (size_t coderIndex = 0; coderIndex < folder.coders.size(); ++coderIndex) {
            if (coderDone[coderIndex]) continue;
            const SevenZipSourceVariant::Folder::Coder& coder =
                folder.coders[coderIndex];
            if (!coder.outputCount ||
                (coder.inputCount != 1 &&
                 !(isSevenZipBcj2(coder.method) && coder.inputCount == 4)) ||
                coder.outputCount != 1) return false;
            std::vector<const std::vector<std::uint8_t>*> inputs;
            inputs.reserve(coder.inputCount);
            bool inputsReady = true;
            for (size_t inputOffset = 0; inputOffset < coder.inputCount; ++inputOffset) {
                const size_t inputIndex = coder.inputBase + inputOffset;
                const std::vector<std::uint8_t>* input = nullptr;
                for (const SevenZipSourceVariant::Folder::Bind& bind : folder.binds) {
                    if (bind.inputIndex == inputIndex) {
                        if (outputReady[bind.outputIndex])
                            input = &outputs[bind.outputIndex];
                        break;
                    }
                }
                if (!input) {
                    const std::vector<size_t>::const_iterator packIndex =
                        std::find(folder.packInputIndices.begin(),
                                  folder.packInputIndices.end(), inputIndex);
                    if (packIndex != folder.packInputIndices.end())
                        input = &packed[size_t(packIndex - folder.packInputIndices.begin())];
                }
                if (!input) {
                    inputsReady = false;
                    break;
                }
                inputs.push_back(input);
            }
            if (!inputsReady) continue;
            const std::vector<std::uint8_t>* input = inputs.size() == 1
                ? inputs[0] : nullptr;
            std::vector<std::uint8_t> decoded;
            const quint64 outputSize64 = folder.outputSizes[coder.outputBase];
            if (outputSize64 > decoded.max_size()) return false;
            const size_t outputSize = size_t(outputSize64);
            if (isSevenZipBcj2(coder.method)) {
                std::array<const std::uint8_t*, 4> branchData = {{nullptr, nullptr,
                                                                  nullptr, nullptr}};
                std::array<std::size_t, 4> branchSizes = {{0, 0, 0, 0}};
                for (size_t branch = 0; branch < 4; ++branch) {
                    branchData[branch] = inputs[branch]->data();
                    branchSizes[branch] = inputs[branch]->size();
                }
                if (!peare::compression::decodeSevenZipBcj2(
                        branchData, branchSizes, outputSize, &decoded)) return false;
            } else if (isSevenZipFolderFilter(coder.method)) {
                if (!input) return false;
                if (input->size() != outputSize) return false;
                decoded = *input;
                if (!applySevenZipFolderFilter(coder.method, coder.properties,
                                               &decoded)) return false;
            } else if (coder.method == QByteArray(1, char(0))) {
                if (!input) return false;
                if (input->size() != outputSize) return false;
                decoded = *input;
            } else if (coder.method == QByteArray::fromHex("030101")) {
                if (!input) return false;
                if (!decodeSevenZipLzma(*input, coder.properties,
                                        outputSize, &decoded)) return false;
            } else if (coder.method == QByteArray::fromHex("21")) {
                if (!input) return false;
                if (!decodeSevenZipLzma2(*input, coder.properties,
                                         outputSize, &decoded)) return false;
            } else if (coder.method == QByteArray::fromHex("030401")) {
                if (!input) return false;
                if (!decodeSevenZipPpmd(*input, coder.properties,
                                       outputSize, &decoded)) return false;
            } else if (coder.method == QByteArray::fromHex("040108")) {
                if (!input) return false;
                if (!decodeSevenZipDeflate(*input, outputSize, &decoded)) return false;
            } else if (coder.method == QByteArray::fromHex("040109")) {
                if (!input || !peare::compression::inflateRawDeflate64Exact(
                        input->data(), input->size(), outputSize, &decoded))
                    return false;
            } else if (coder.method == QByteArray::fromHex("040202")) {
                if (!input) return false;
                if (!decodeSevenZipBzip2(*input, outputSize, &decoded)) return false;
            } else if (coder.method == QByteArray::fromHex("06f10701")) {
                if (!input || password.isNull()) return false;
                if (!peare::compression::decryptSevenZipAes(
                        coder.properties, password, input->data(), input->size(),
                        outputSize, &decoded)) return false;
            } else {
                return false;
            }
            if (decoded.size() != outputSize || outputReady[coder.outputBase])
                return false;
            outputs[coder.outputBase] = std::move(decoded);
            outputReady[coder.outputBase] = true;
            coderDone[coderIndex] = true;
            ++completed;
            progressed = true;
        }
        if (!progressed) return false;
    }
    if (!outputReady[folder.finalOutputIndex] ||
        outputs[folder.finalOutputIndex].size() != size_t(folder.unpackedSize))
        return false;
    unpacked->swap(outputs[folder.finalOutputIndex]);
    return !folder.unpackedCrcDefined ||
        crc32Bytes(unpacked->data(), unpacked->size()) == folder.unpackedCrc;
}

bool readSevenZipArchive(QFile* input, const SevenZipSourceVariant& variant,
                         const QString& password,
                         QTemporaryDir* staging,
                         std::vector<SevenZipSourceVariant::File>* files,
                         QString* error)
{
    if (!input || !staging || !files || !input->isOpen()) return false;
    *files = variant.files;
    if (variant.folders.empty()) return true;
    if (variant.packCount != variant.packedSizes.size()) {
        if (error) *error = QStringLiteral(
            "The 7z archive uses a packed-stream layout the Writer cannot update.");
        return false;
    }
    quint64 packedOffset = 32 + variant.packPosition;
    quint64 streamFile = 0;
    for (size_t folderIndex = 0; folderIndex < variant.folders.size(); ++folderIndex) {
        const SevenZipSourceVariant::Folder& folder = variant.folders[folderIndex];
        if (folder.packedSizes.size() != folder.packInputIndices.size() ||
            folder.packedCrcs.size() != folder.packedSizes.size() ||
            folder.packedCrcDefined.size() != folder.packedSizes.size()) {
            if (error) *error = QStringLiteral("7z packed-stream metadata is inconsistent.");
            return false;
        }
        std::vector<std::vector<std::uint8_t> > packed;
        packed.reserve(folder.packedSizes.size());
        std::vector<char> buffer(static_cast<size_t>(kIoChunk), 0);
        for (size_t packIndex = 0; packIndex < folder.packedSizes.size(); ++packIndex) {
            const quint64 packedSize64 = folder.packedSizes[packIndex];
            if (packedOffset > quint64(input->size()) ||
                packedSize64 > quint64(input->size()) - packedOffset ||
                packedSize64 > std::vector<std::uint8_t>().max_size() ||
                packedOffset > quint64((std::numeric_limits<qint64>::max)()) ||
                !input->seek(qint64(packedOffset))) {
                if (error) *error = QStringLiteral("A 7z packed stream is out of bounds.");
                return false;
            }
            std::vector<std::uint8_t> stream;
            stream.reserve(size_t(packedSize64));
            quint64 remaining = packedSize64;
            while (remaining) {
                const qint64 amount = qint64(std::min<quint64>(
                    remaining, quint64(buffer.size())));
                const qint64 count = input->read(buffer.data(), amount);
                if (count != amount) {
                    if (error) *error = QStringLiteral("A 7z packed stream is truncated.");
                    return false;
                }
                stream.insert(stream.end(), buffer.begin(), buffer.begin() + count);
                remaining -= quint64(count);
            }
            if (folder.packedCrcDefined[packIndex] &&
                crc32Bytes(stream.data(), stream.size()) != folder.packedCrcs[packIndex]) {
                if (error) *error = QStringLiteral("A 7z packed stream failed its CRC check.");
                return false;
            }
            packed.push_back(std::move(stream));
            packedOffset += packedSize64;
        }
        std::vector<std::uint8_t> unpacked;
        if (!decodeSevenZipFolder(folder, packed, password, &unpacked)) {
            if (error) *error = QStringLiteral(
                "A 7z folder uses an invalid or unsupported codec stream.");
            return false;
        }
        quint64 streamOffset = 0;
        for (size_t fileIndex = 0; fileIndex < files->size(); ++fileIndex) {
            SevenZipSourceVariant::File& file = (*files)[fileIndex];
            if (!file.hasStream || file.folder != folderIndex) continue;
            if (streamFile >= variant.folders[folderIndex].substreamSizes.size() ||
                file.size != variant.folders[folderIndex].substreamSizes[
                    size_t(streamFile)] ||
                streamOffset > unpacked.size() ||
                file.size > unpacked.size() - size_t(streamOffset)) {
                if (error) *error = QStringLiteral("7z substream metadata is inconsistent.");
                return false;
            }
            const size_t substreamIndex = size_t(streamFile);
            const SevenZipSourceVariant::Folder& metadata = variant.folders[folderIndex];
            if (substreamIndex < metadata.substreamCrcDefined.size() &&
                metadata.substreamCrcDefined[substreamIndex] &&
                crc32Bytes(unpacked.data() + size_t(streamOffset), size_t(file.size)) !=
                    metadata.substreamCrcs[substreamIndex]) {
                if (error) *error = QStringLiteral("A 7z file failed its CRC check.");
                return false;
            }
            const QString path = staging->filePath(
                QStringLiteral("source-%1").arg(fileIndex));
            QFile staged(path);
            if (!staged.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                (file.size && !writeAll(&staged,
                    reinterpret_cast<const char*>(unpacked.data() +
                        size_t(streamOffset)), qint64(file.size))) ||
                !staged.flush()) {
                if (error) *error = staged.errorString();
                return false;
            }
            file.stagedPath = path;
            streamOffset += file.size;
            ++streamFile;
        }
        if (streamOffset != unpacked.size()) {
            if (error) *error = QStringLiteral("7z folder data has unreferenced bytes.");
            return false;
        }
        streamFile = 0;
    }
    return true;
}

} // namespace

SevenZipArchiveWriter::SevenZipArchiveWriter() = default;

SevenZipArchiveWriter::~SevenZipArchiveWriter()
{
    abort();
}

bool SevenZipArchiveWriter::canUpdate(const QString& archivePath)
{
    SevenZipSourceVariant variant;
    if (!readSevenZipSourceVariant(archivePath, QString(), &variant)) return false;
    const auto supportedMethod = [](const QByteArray& method) {
        return method == QByteArray(1, char(0)) ||
            method == QByteArray::fromHex("030401") ||
            method == QByteArray::fromHex("030101") ||
            method == QByteArray::fromHex("21") ||
            method == QByteArray::fromHex("040108") ||
            method == QByteArray::fromHex("040109") ||
            method == QByteArray::fromHex("040202");
    };
    for (const SevenZipSourceVariant::Folder& folder : variant.folders)
        if (!supportedMethod(folder.method)) return false;
    return true;
}

bool SevenZipArchiveWriter::beginUpdate(const QString& archivePath,
                                        const QString& password,
                                        QString* error)
{
    abort();
    if (archivePath.isEmpty()) {
        if (error) *error = QStringLiteral("An archive path is required.");
        return false;
    }
    SevenZipSourceVariant sourceVariant;
    if (!readSevenZipSourceVariant(archivePath, password, &sourceVariant)) {
        if (error) *error = QStringLiteral(
            "The 7z header cannot be rewritten without changing its compression variant.");
        return false;
    }
    if ((sourceVariant.dataEncrypted || sourceVariant.headerEncrypted) &&
        password.isNull()) {
        if (error) *error = QStringLiteral("A password is required for this 7z archive.");
        return false;
    }
    methodId_ = sourceVariant.foundStreams
        ? sourceVariant.method : QByteArray(1, char(0));
    methodProperties_ = sourceVariant.properties;
    headerCompressed_ = sourceVariant.headerCompressed;
    headerMethodId_ = sourceVariant.headerMethod;
    headerMethodProperties_ = sourceVariant.headerProperties;
    headerEncrypted_ = sourceVariant.headerEncrypted;
    headerAesProperties_ = sourceVariant.headerAesProperties;
    dataEncrypted_ = sourceVariant.dataEncrypted;
    password_ = password;
    if (methodId_ != QByteArray(1, char(0)) &&
        methodId_ != QByteArray::fromHex("030401") &&
        methodId_ != QByteArray::fromHex("030101") &&
        methodId_ != QByteArray::fromHex("21") &&
        methodId_ != QByteArray::fromHex("040108") &&
        methodId_ != QByteArray::fromHex("040109") &&
        methodId_ != QByteArray::fromHex("040202")) {
        if (error) *error = QStringLiteral(
            "The 7z compression method cannot be preserved by the Writer.");
        return false;
    }
    compressionLevel_ = methodId_ == QByteArray(1, char(0)) ? 0 : 5;
    if (methodId_ == QByteArray::fromHex("040202") &&
        !methodProperties_.isEmpty()) {
        if (methodProperties_.size() != 1 ||
            static_cast<unsigned char>(methodProperties_.at(0)) < 1 ||
            static_cast<unsigned char>(methodProperties_.at(0)) > 9) {
            if (error) *error = QStringLiteral("The 7z BZip2 properties are invalid.");
            return false;
        }
        compressionLevel_ = static_cast<unsigned char>(methodProperties_.at(0));
    }
    solidBlockSize_ = sourceVariant.solid
        ? (std::numeric_limits<quint64>::max)() : 0;
    std::unique_ptr<QTemporaryDir> staging(new QTemporaryDir);
    if (!staging->isValid()) {
        if (error) *error = QStringLiteral("Could not create the 7z staging directory.");
        return false;
    }
    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = input.errorString();
        return false;
    }
    std::vector<SevenZipSourceVariant::File> sourceFiles;
    if (!readSevenZipArchive(&input, sourceVariant, password, staging.get(), &sourceFiles,
                             error)) return false;
    std::vector<Entry> parsed;
    for (const SevenZipSourceVariant::File& source : sourceFiles) {
        Entry entry;
        entry.name = source.name.toUtf8();
        entry.stagedPath = source.stagedPath;
        entry.size = source.size;
        entry.sourceFolder = source.hasStream ? qint64(source.folder) : -1;
        entry.methodId = source.method;
        entry.methodProperties = source.properties;
        entry.directory = source.directory;
        entry.hasStream = source.hasStream;
        if (!validName(entry.name, entry.directory)) {
            if (error) *error = QStringLiteral("The archive contains an unsafe 7z path.");
            return false;
        }
        parsed.push_back(std::move(entry));
    }
    outputPath_ = archivePath;
    staging_ = std::move(staging);
    entries_ = std::move(parsed);
    nextStagedFile_ = 0;
    active_ = true;
    return true;
}

bool SevenZipArchiveWriter::beginCreate(const QString& outputPath,
                                       const QByteArray& variant,
                                       const QString& password, QString* error)
{
    abort();
    if (outputPath.isEmpty()) {
        if (error) *error = QStringLiteral("An output path is required.");
        return false;
    }
    std::unique_ptr<QTemporaryDir> staging(new QTemporaryDir);
    if (!staging->isValid()) {
        if (error) *error = QStringLiteral("Could not create the 7z staging directory.");
        return false;
    }
    outputPath_ = outputPath;
    password_ = password;
    dataEncrypted_ = !password.isNull();
    headerEncrypted_ = dataEncrypted_;
    if (headerEncrypted_) {
        headerCompressed_ = true;
        headerMethodId_ = QByteArray::fromHex("21");
    }
    staging_ = std::move(staging);
    nextStagedFile_ = 0;
    if (!parseVariant(variant, error)) {
        abort();
        return false;
    }
    active_ = true;
    return true;
}

bool SevenZipArchiveWriter::parseVariant(const QByteArray& variant, QString* error)
{
    QByteArray codecVariant = variant;
    methodProperties_.clear();
    solidBlockSize_ = 0;
    const int solidPosition = variant.indexOf("-solid-");
    if (solidPosition >= 0) {
        const QByteArray size = variant.mid(solidPosition + 7);
        if (size == "1m") solidBlockSize_ = quint64(1) << 20;
        else if (size == "16m") solidBlockSize_ = quint64(16) << 20;
        else if (size == "2g") solidBlockSize_ = quint64(2) << 30;
        if (!solidBlockSize_) {
            if (error) *error = QStringLiteral("The 7z solid block size is invalid.");
            return false;
        }
        codecVariant = variant.left(solidPosition);
    }
    if (codecVariant == "store") {
        methodId_ = QByteArray(1, char(0));
        compressionLevel_ = 0;
        return true;
    }
    if (codecVariant == "ppmd") {
        methodId_ = QByteArray::fromHex("030401");
        compressionLevel_ = 0;
        return true;
    }
    static const QByteArray lzmaPrefix("lzma-");
    if (codecVariant.startsWith(lzmaPrefix)) {
        bool ok = false;
        const int level = codecVariant.mid(lzmaPrefix.size()).toInt(&ok);
        if (ok && level >= 0 && level <= 9) {
            methodId_ = QByteArray::fromHex("030101");
            compressionLevel_ = level;
            return true;
        }
    }
    static const QByteArray prefix("lzma2-");
    if (codecVariant.startsWith(prefix)) {
        bool ok = false;
        const int level = codecVariant.mid(prefix.size()).toInt(&ok);
        if (ok && level >= 0 && level <= 9) {
            methodId_ = QByteArray::fromHex("21");
            compressionLevel_ = level;
            return true;
        }
    }
    static const QByteArray deflatePrefix("deflate-");
    if (codecVariant.startsWith(deflatePrefix)) {
        bool ok = false;
        const int level = codecVariant.mid(deflatePrefix.size()).toInt(&ok);
        if (ok && level >= 0 && level <= 9) {
            methodId_ = QByteArray::fromHex("040108");
            compressionLevel_ = level;
            return true;
        }
    }
    static const QByteArray deflate64Prefix("deflate64-");
    if (codecVariant.startsWith(deflate64Prefix)) {
        bool ok = false;
        const int level = codecVariant.mid(deflate64Prefix.size()).toInt(&ok);
        if (ok && level >= 0 && level <= 9) {
            methodId_ = QByteArray::fromHex("040109");
            compressionLevel_ = level;
            return true;
        }
    }
    static const QByteArray bzip2Prefix("bzip2-");
    if (codecVariant.startsWith(bzip2Prefix)) {
        bool ok = false;
        const int level = codecVariant.mid(bzip2Prefix.size()).toInt(&ok);
        if (ok && level >= 1 && level <= 9) {
            methodId_ = QByteArray::fromHex("040202");
            compressionLevel_ = level;
            return true;
        }
    }
    if (error) *error = QStringLiteral("The requested 7z compression variant is invalid.");
    return false;
}

bool SevenZipArchiveWriter::stageFile(const QString& sourcePath,
                                     QString* stagedPath, quint64* size,
                                     quint32* digest, QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = source.errorString();
        return false;
    }
    const QString path = staging_->filePath(
        QStringLiteral("input-%1").arg(nextStagedFile_++));
    QFile staged(path);
    if (!staged.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error) *error = staged.errorString();
        return false;
    }
    QByteArray buffer(int(kIoChunk), Qt::Uninitialized);
    quint64 total = 0;
    quint32 crc = 0xffffffffU;
    for (;;) {
        const qint64 count = source.read(buffer.data(), buffer.size());
        if (count < 0) {
            if (error) *error = source.errorString();
            return false;
        }
        if (count == 0) break;
        if (quint64(count) > (std::numeric_limits<quint64>::max)() - total ||
            !writeAll(&staged, buffer.constData(), count)) {
            if (error) *error = staged.errorString();
            return false;
        }
        total += quint64(count);
        crc = crc32Update(crc, buffer.constData(), count);
    }
    if (!staged.flush()) {
        if (error) *error = staged.errorString();
        return false;
    }
    *stagedPath = path;
    *size = total;
    *digest = crc ^ 0xffffffffU;
    return true;
}

bool SevenZipArchiveWriter::addFile(const QString& sourcePath,
                                   const QByteArray& entryName, QString* error)
{
    if (!active_ || !validName(entryName, false)) {
        if (error) *error = QStringLiteral("The 7z file path is invalid.");
        return false;
    }
    const QByteArray name = normalizedName(entryName, false);
    const QString key = keyForName(name);
    for (const Entry& entry : entries_) {
        if (keyForName(entry.name) == key) {
            if (error) *error = QStringLiteral("The 7z path already exists.");
            return false;
        }
    }
    Entry entry;
    entry.name = name;
    if (!stageFile(sourcePath, &entry.stagedPath, &entry.size, &entry.crc, error))
        return false;
    entry.hasStream = entry.size != 0;
    entries_.push_back(std::move(entry));
    return true;
}

bool SevenZipArchiveWriter::addEmptyFile(const QByteArray& entryName, QString* error)
{
    if (!active_ || !validName(entryName, false)) {
        if (error) *error = QStringLiteral("The 7z file path is invalid.");
        return false;
    }
    const QByteArray name = normalizedName(entryName, false);
    const QString key = keyForName(name);
    for (const Entry& entry : entries_) {
        if (keyForName(entry.name) == key) {
            if (error) *error = QStringLiteral("The 7z path already exists.");
            return false;
        }
    }
    Entry entry;
    entry.name = name;
    entries_.push_back(std::move(entry));
    return true;
}

bool SevenZipArchiveWriter::addDirectory(const QByteArray& entryName, QString* error)
{
    if (!active_ || !validName(entryName, true)) {
        if (error) *error = QStringLiteral("The 7z directory path is invalid.");
        return false;
    }
    const QByteArray name = normalizedName(entryName, true);
    const QString key = keyForName(name);
    for (const Entry& entry : entries_) {
        if (keyForName(entry.name) == key) {
            if (error) *error = QStringLiteral("The 7z path already exists.");
            return false;
        }
    }
    Entry entry;
    entry.name = name;
    entry.directory = true;
    entries_.push_back(std::move(entry));
    return true;
}

bool SevenZipArchiveWriter::replaceFile(const QString& sourcePath,
                                       const QByteArray& entryName, QString* error)
{
    if (!active_) {
        if (error) *error = QStringLiteral("No 7z update is active.");
        return false;
    }
    const QString key = keyForName(normalizedName(entryName, false));
    for (Entry& entry : entries_) {
        if (!entry.directory && keyForName(entry.name) == key) {
            QString stagedPath;
            quint64 size = 0;
            quint32 digest = 0;
            if (!stageFile(sourcePath, &stagedPath, &size, &digest, error)) return false;
            entry.stagedPath = stagedPath;
            entry.size = size;
            entry.crc = digest;
            entry.hasStream = size != 0;
            return true;
        }
    }
    if (error) *error = QStringLiteral("The 7z file to replace does not exist.");
    return false;
}

bool SevenZipArchiveWriter::renameEntry(const QByteArray& oldName,
                                        const QByteArray& newName,
                                        QString* error)
{
    if (!active_) {
        if (error) *error = QStringLiteral("No 7z update is active.");
        return false;
    }
    const QByteArray oldNormalized = normalizedName(oldName, false);
    const QString oldKey = keyForName(oldNormalized);
    const QString oldDirectoryKey = oldKey + QLatin1Char('/');
    size_t sourceIndex = entries_.size();
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (keyForName(entries_[i].name) == oldKey ||
            keyForName(entries_[i].name) == oldDirectoryKey) {
            sourceIndex = i;
            break;
        }
    }
    if (sourceIndex == entries_.size()) {
        if (error) *error = QStringLiteral("The 7z entry to rename does not exist.");
        return false;
    }
    const bool directory = entries_[sourceIndex].directory;
    if (!validName(newName, directory)) {
        if (error) *error = QStringLiteral("The new 7z path is invalid.");
        return false;
    }
    const QByteArray replacement = normalizedName(newName, directory);
    const QString newKey = keyForName(replacement);
    if (directory && newKey.startsWith(oldKey) && newKey != oldKey) {
        if (error) *error = QStringLiteral("A 7z directory cannot be moved inside itself.");
        return false;
    }
    if (newKey != oldKey) {
        for (const Entry& entry : entries_) {
            const QString candidate = keyForName(entry.name);
            if (candidate == newKey ||
                (directory && candidate.startsWith(newKey))) {
                if (error) *error = QStringLiteral("The new 7z path conflicts with an existing entry.");
                return false;
            }
        }
    }
    const QByteArray oldPrefix = entries_[sourceIndex].name;
    for (Entry& entry : entries_) {
        if (keyForName(entry.name) == oldKey) {
            entry.name = replacement;
        } else if (directory && keyForName(entry.name).startsWith(oldKey)) {
            entry.name = replacement + entry.name.mid(oldPrefix.size());
        }
    }
    return true;
}

bool SevenZipArchiveWriter::deleteEntry(const QByteArray& entryName, bool recursive,
                                       QString* error)
{
    if (!active_) {
        if (error) *error = QStringLiteral("No 7z update is active.");
        return false;
    }
    const QString key = keyForName(normalizedName(entryName, false));
    const QString directoryKey = key + QLatin1Char('/');
    size_t found = entries_.size();
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (keyForName(entries_[i].name) == key ||
            keyForName(entries_[i].name) == directoryKey) {
            found = i;
            break;
        }
    }
    if (found == entries_.size()) {
        if (error) *error = QStringLiteral("The 7z entry to delete does not exist.");
        return false;
    }
    const bool directory = entries_[found].directory;
    const QString storedKey = keyForName(entries_[found].name);
    const QString prefix = directory ? storedKey : key + QLatin1Char('/');
    if (directory && !recursive) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (i != found && keyForName(entries_[i].name).startsWith(prefix)) {
                if (error) *error = QStringLiteral("The 7z directory is not empty.");
                return false;
            }
        }
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const Entry& entry) {
            const QString candidate = keyForName(entry.name);
            return candidate == key || candidate == directoryKey ||
                   (directory && recursive &&
                                         candidate.startsWith(prefix));
        }), entries_.end());
    return true;
}

bool SevenZipArchiveWriter::writeArchive(QString* error)
{
    std::stable_sort(entries_.begin(), entries_.end(),
        [](const Entry& left, const Entry& right) {
            const qint64 leftFolder = left.sourceFolder < 0
                ? (std::numeric_limits<qint64>::max)() : left.sourceFolder;
            const qint64 rightFolder = right.sourceFolder < 0
                ? (std::numeric_limits<qint64>::max)() : right.sourceFolder;
            return leftFolder < rightFolder;
        });
    QSaveFile output(outputPath_);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = output.errorString();
        return false;
    }
    QByteArray signature(reinterpret_cast<const char*>(kSignature), sizeof(kSignature));
    signature.append(char(0));
    signature.append(char(4));
    signature.append(QByteArray(4 + 20, char(0)));
    if (!writeAll(&output, signature.constData(), signature.size())) {
        if (error) *error = output.errorString();
        return false;
    }

    struct FolderGroup {
        std::vector<Entry*> entries;
        quint64 unpackedSize = 0;
        quint64 packedSize = 0;
        quint64 codedSize = 0;
        quint32 unpackedCrc = 0;
        quint32 packedCrc = 0;
        qint64 sourceFolder = -1;
        QByteArray methodId;
        QByteArray methodProperties;
        QByteArray aesProperties;
        bool encrypted = false;
    };
    std::vector<Entry*> streams;
    std::vector<FolderGroup> groups;
    for (Entry& entry : entries_) {
        if (!entry.hasStream) continue;
        streams.push_back(&entry);
        if (entry.sourceFolder >= 0) {
            if (groups.empty() ||
                groups.back().sourceFolder != entry.sourceFolder) {
                groups.push_back(FolderGroup());
                groups.back().sourceFolder = entry.sourceFolder;
            }
        } else if (groups.empty() || !solidBlockSize_ ||
                   groups.back().unpackedSize > solidBlockSize_ ||
                   entry.size > solidBlockSize_ - groups.back().unpackedSize) {
            groups.push_back(FolderGroup());
        }
        if (entry.size > (std::numeric_limits<quint64>::max)() -
                         groups.back().unpackedSize) {
            if (error) *error = QStringLiteral("A 7z solid folder is too large.");
            return false;
        }
        groups.back().entries.push_back(&entry);
        groups.back().unpackedSize += entry.size;
    }

    std::vector<quint64> packedSizes;
    std::vector<quint32> packedCrcs;
    packedSizes.reserve(groups.size());
    packedCrcs.reserve(groups.size());
    for (FolderGroup& group : groups) {
        std::vector<std::unique_ptr<QIODevice> > ownedInputs;
        std::vector<QIODevice*> inputs;
        std::vector<quint64> inputSizes;
        std::vector<quint32> fileCrcs(group.entries.size(), 0xffffffffU);
        ownedInputs.reserve(group.entries.size());
        inputs.reserve(group.entries.size());
        inputSizes.reserve(group.entries.size());
        for (Entry* entry : group.entries) {
            std::unique_ptr<QIODevice> source;
            source.reset(new QFile(entry->stagedPath));
            if (!source->open(QIODevice::ReadOnly)) {
                if (error) *error = source->errorString();
                return false;
            }
            inputs.push_back(source.get());
            inputSizes.push_back(entry->size);
            ownedInputs.push_back(std::move(source));
        }
        CompositeInput input(inputs, inputSizes, &fileCrcs);
        if (!input.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Could not open a 7z folder stream.");
            return false;
        }
        quint64 unpackedSize = 0;
        quint32 unpackedCrc = 0;
        quint64 packedSize = 0;
        quint32 packedCrc = 0;
        QByteArray codedBytes;
        QBuffer codedOutput(&codedBytes);
        if (!codedOutput.open(QIODevice::WriteOnly)) {
            if (error) *error = QStringLiteral("Could not stage a 7z folder stream.");
            return false;
        }
        const bool preserveSourceMethod = group.sourceFolder >= 0;
        group.methodId = preserveSourceMethod
            ? group.entries.front()->methodId : methodId_;
        const QByteArray sourceProperties = preserveSourceMethod
            ? group.entries.front()->methodProperties : methodProperties_;
        if (group.methodId == QByteArray::fromHex("21")) {
            if (!encodeLzma2(&input, &codedOutput, compressionLevel_, sourceProperties,
                             group.unpackedSize, &group.methodProperties,
                             &unpackedSize, &unpackedCrc, &packedSize,
                             &packedCrc, error))
                return false;
        } else if (group.methodId == QByteArray::fromHex("030401")) {
            if (!encodePpmd7(&input, &codedOutput, sourceProperties,
                             &group.methodProperties,
                             &unpackedSize, &unpackedCrc, &packedSize,
                             &packedCrc, error))
                return false;
        } else if (group.methodId == QByteArray::fromHex("030101")) {
            if (!encodeLzma(&input, &codedOutput, compressionLevel_, sourceProperties,
                            group.unpackedSize, &group.methodProperties,
                            &unpackedSize, &unpackedCrc, &packedSize,
                            &packedCrc, error))
                return false;
        } else if (group.methodId == QByteArray::fromHex("040108")) {
            if (!encodeDeflate(&input, &codedOutput, compressionLevel_,
                               &unpackedSize, &unpackedCrc, &packedSize,
                               &packedCrc, error))
                return false;
        } else if (group.methodId == QByteArray::fromHex("040109")) {
            if (!encodeDeflate64Stored(&input, &codedOutput,
                                       &unpackedSize, &unpackedCrc, &packedSize,
                                       &packedCrc, error))
                return false;
        } else if (group.methodId == QByteArray::fromHex("040202")) {
            int level = compressionLevel_;
            if (sourceProperties.size() == 1) {
                const unsigned property = static_cast<unsigned char>(sourceProperties.at(0));
                if (property < 1 || property > 9) {
                    if (error) *error = QStringLiteral("The 7z BZip2 properties are invalid.");
                    return false;
                }
                level = int(property);
                group.methodProperties = sourceProperties;
            }
            if (!encodeBzip2(&input, &codedOutput, level,
                             &unpackedSize, &unpackedCrc, &packedSize,
                             &packedCrc, error))
                return false;
        } else {
            QByteArray buffer(int(kIoChunk), Qt::Uninitialized);
            quint32 crc = 0xffffffffU;
            for (;;) {
                const qint64 count = input.read(buffer.data(), buffer.size());
                if (count < 0) {
                    if (error) *error = QStringLiteral("Could not read a 7z input stream.");
                    return false;
                }
                if (!count) break;
                if (!writeAll(&codedOutput, buffer.constData(), count)) {
                    if (error) *error = codedOutput.errorString();
                    return false;
                }
                crc = crc32Update(crc, buffer.constData(), count);
                unpackedSize += quint64(count);
                packedSize += quint64(count);
            }
            unpackedCrc = crc ^ 0xffffffffU;
            packedCrc = unpackedCrc;
        }
        if (unpackedSize != group.unpackedSize ||
            input.groupCrc() != unpackedCrc) {
            if (error) *error = QStringLiteral("A 7z input changed during compression.");
            return false;
        }
        codedOutput.close();
        group.unpackedCrc = unpackedCrc;
        group.codedSize = quint64(codedBytes.size());
        group.encrypted = dataEncrypted_;
        QByteArray packedBytes = codedBytes;
        if (group.encrypted) {
            if (password_.isNull() ||
                !peare::compression::createSevenZipAesProperties(
                    &group.aesProperties)) {
                if (error) *error = QStringLiteral("Could not initialize 7z encryption.");
                return false;
            }
            std::vector<std::uint8_t> encrypted;
            if (!peare::compression::encryptSevenZipAes(
                    group.aesProperties, password_,
                    reinterpret_cast<const std::uint8_t*>(codedBytes.constData()),
                    size_t(codedBytes.size()), &encrypted)) {
                if (error) *error = QStringLiteral("Could not encrypt a 7z folder stream.");
                return false;
            }
            packedBytes = QByteArray(reinterpret_cast<const char*>(encrypted.data()),
                                     int(encrypted.size()));
        }
        if (!writeAll(&output, packedBytes.constData(), packedBytes.size())) {
            if (error) *error = output.errorString();
            return false;
        }
        group.packedSize = quint64(packedBytes.size());
        group.packedCrc = crc32(packedBytes);
        for (size_t index = 0; index < group.entries.size(); ++index) {
            Entry* entry = group.entries[index];
            entry->methodId = group.methodId;
            entry->methodProperties = group.methodProperties;
            entry->crc = fileCrcs[index] ^ 0xffffffffU;
        }
        packedSizes.push_back(group.packedSize);
        packedCrcs.push_back(group.packedCrc);
    }

    QByteArray header;
    header.append(char(1));
    if (!streams.empty()) {
        header.append(char(4));
        header.append(char(6));
        appendNumber(&header, 0);
        appendNumber(&header, quint64(groups.size()));
        header.append(char(9));
        for (quint64 size : packedSizes) appendNumber(&header, size);
        header.append(char(10));
        appendDigestSet(&header, packedCrcs);
        header.append(char(0));

        header.append(char(7));
        header.append(char(11));
        appendNumber(&header, quint64(groups.size()));
        header.append(char(0));
        for (const FolderGroup& group : groups) {
            if (group.encrypted) {
                const bool hasCodec = group.methodId != QByteArray(1, char(0));
                header.append(char(hasCodec ? 2 : 1));
                header.append(char(0x24));
                header.append(QByteArray::fromHex("06f10701"));
                appendNumber(&header, quint64(group.aesProperties.size()));
                header.append(group.aesProperties);
                if (hasCodec) {
                    header.append(char(group.methodId.size() |
                        (group.methodProperties.isEmpty() ? 0 : 0x20)));
                    header.append(group.methodId);
                    if (!group.methodProperties.isEmpty()) {
                        appendNumber(&header, quint64(group.methodProperties.size()));
                        header.append(group.methodProperties);
                    }
                }
                if (hasCodec) {
                    appendNumber(&header, 1);
                    appendNumber(&header, 0);
                }
            } else {
                header.append(char(1));
                header.append(char(group.methodId.size() |
                                   (group.methodProperties.isEmpty() ? 0 : 0x20)));
                header.append(group.methodId);
                if (!group.methodProperties.isEmpty()) {
                    appendNumber(&header, quint64(group.methodProperties.size()));
                    header.append(group.methodProperties);
                }
            }
        }
        header.append(char(12));
        for (const FolderGroup& group : groups) {
            if (group.encrypted) appendNumber(&header, group.codedSize);
            if (!group.encrypted || group.methodId != QByteArray(1, char(0)))
                appendNumber(&header, group.unpackedSize);
        }
        header.append(char(10));
        std::vector<quint32> folderCrcs;
        for (const FolderGroup& group : groups)
            folderCrcs.push_back(group.unpackedCrc);
        appendDigestSet(&header, folderCrcs);
        header.append(char(0));

        const bool hasSolidFolder = std::any_of(groups.begin(), groups.end(),
            [](const FolderGroup& group) { return group.entries.size() > 1; });
        if (hasSolidFolder) {
            header.append(char(8));
            header.append(char(13));
            for (const FolderGroup& group : groups)
                appendNumber(&header, quint64(group.entries.size()));
            header.append(char(9));
            for (const FolderGroup& group : groups)
                for (size_t index = 0; index + 1 < group.entries.size(); ++index)
                    appendNumber(&header, group.entries[index]->size);
            header.append(char(10));
            std::vector<quint32> substreamCrcs;
            for (const FolderGroup& group : groups)
                if (group.entries.size() > 1)
                    for (const Entry* entry : group.entries)
                        substreamCrcs.push_back(entry->crc);
            appendDigestSet(&header, substreamCrcs);
            header.append(char(0));
        }
        header.append(char(0));
    }

    header.append(char(5));
    appendNumber(&header, quint64(entries_.size()));
    std::vector<bool> emptyStreams;
    std::vector<bool> emptyFiles;
    emptyStreams.reserve(entries_.size());
    for (const Entry& entry : entries_)
        emptyStreams.push_back(!entry.hasStream);
    if (std::find(emptyStreams.begin(), emptyStreams.end(), true) != emptyStreams.end()) {
        appendFileProperty(&header, 14, bitVector(emptyStreams));
        for (const Entry& entry : entries_)
            if (!entry.hasStream) emptyFiles.push_back(!entry.directory);
        appendFileProperty(&header, 15, bitVector(emptyFiles));
    }
    QByteArray names(1, char(0));
    for (const Entry& entry : entries_) names.append(utf16Name(entry.name));
    appendFileProperty(&header, 17, names);
    header.append(char(0));
    header.append(char(0));

    QByteArray nextHeader = header;
    if (headerCompressed_) {
        QByteArray codedHeader;
        QBuffer codedOutput(&codedHeader);
        QBuffer headerInput(&header);
        if (!codedOutput.open(QIODevice::WriteOnly) ||
            !headerInput.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Could not stage the compressed 7z header.");
            return false;
        }
        QByteArray encodedProperties;
        quint64 unpackedSize = 0;
        quint32 unpackedCrc = 0;
        quint64 packedSize = 0;
        quint32 packedCrc = 0;
        bool encoded = false;
        if (headerMethodId_ == QByteArray::fromHex("21")) {
            encoded = encodeLzma2(
                &headerInput, &codedOutput, compressionLevel_,
                headerMethodProperties_, quint64(header.size()),
                &encodedProperties, &unpackedSize, &unpackedCrc,
                &packedSize, &packedCrc, error);
        } else if (headerMethodId_ == QByteArray::fromHex("030101")) {
            encoded = encodeLzma(
                &headerInput, &codedOutput, compressionLevel_,
                headerMethodProperties_, quint64(header.size()),
                &encodedProperties, &unpackedSize, &unpackedCrc,
                &packedSize, &packedCrc, error);
        } else if (headerMethodId_ == QByteArray(1, char(0))) {
            encoded = writeAll(&codedOutput, header.constData(), header.size());
            unpackedSize = packedSize = quint64(header.size());
            unpackedCrc = packedCrc = crc32(header);
        }
        codedOutput.close();
        headerInput.close();
        if (!encoded || (!headerMethodProperties_.isEmpty() &&
                         encodedProperties != headerMethodProperties_) ||
            unpackedSize != quint64(header.size()) ||
            packedSize != quint64(codedHeader.size()) ||
            unpackedCrc != crc32(header)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("Could not preserve the 7z header codec properties.");
            return false;
        }
        headerMethodProperties_ = encodedProperties;
        QByteArray packedHeader = codedHeader;
        quint64 encodedSize = packedSize;
        quint32 encodedCrc = packedCrc;
        QByteArray aesProperties;
        if (headerEncrypted_) {
            if (password_.isNull() ||
                !peare::compression::createSevenZipAesProperties(&aesProperties)) {
                if (error) *error = QStringLiteral("Could not initialize 7z header encryption.");
                return false;
            }
            std::vector<std::uint8_t> encrypted;
            if (!peare::compression::encryptSevenZipAes(
                    aesProperties, password_,
                    reinterpret_cast<const std::uint8_t*>(codedHeader.constData()),
                    size_t(codedHeader.size()), &encrypted)) {
                if (error) *error = QStringLiteral("Could not encrypt the 7z header.");
                return false;
            }
            packedHeader = QByteArray(reinterpret_cast<const char*>(encrypted.data()),
                                      int(encrypted.size()));
            encodedSize = quint64(codedHeader.size());
            encodedCrc = crc32(packedHeader);
            headerAesProperties_ = aesProperties;
        }
        const quint64 packedHeaderPosition = quint64(output.pos()) - 32;
        if (!writeAll(&output, packedHeader.constData(), packedHeader.size())) {
            if (error) *error = output.errorString();
            return false;
        }
        nextHeader.clear();
        nextHeader.append(char(0x17));
        nextHeader.append(char(6));
        appendNumber(&nextHeader, packedHeaderPosition);
        appendNumber(&nextHeader, quint64(1));
        nextHeader.append(char(9));
        appendNumber(&nextHeader, quint64(packedHeader.size()));
        nextHeader.append(char(10));
        appendDigestSet(&nextHeader, std::vector<quint32>(1, encodedCrc));
        nextHeader.append(char(0));
        nextHeader.append(char(7));
        nextHeader.append(char(11));
        appendNumber(&nextHeader, quint64(1));
        nextHeader.append(char(0));
        if (headerEncrypted_) {
            const bool hasCodec = headerMethodId_ != QByteArray(1, char(0));
            nextHeader.append(char(hasCodec ? 2 : 1));
            nextHeader.append(char(0x24));
            nextHeader.append(QByteArray::fromHex("06f10701"));
            appendNumber(&nextHeader, quint64(aesProperties.size()));
            nextHeader.append(aesProperties);
            if (hasCodec) {
                nextHeader.append(char(headerMethodId_.size() |
                    (headerMethodProperties_.isEmpty() ? 0 : 0x20)));
                nextHeader.append(headerMethodId_);
                if (!headerMethodProperties_.isEmpty()) {
                    appendNumber(&nextHeader, quint64(headerMethodProperties_.size()));
                    nextHeader.append(headerMethodProperties_);
                }
                appendNumber(&nextHeader, 1);
                appendNumber(&nextHeader, 0);
            }
        } else {
            nextHeader.append(char(1));
            nextHeader.append(char(headerMethodId_.size() |
                (headerMethodProperties_.isEmpty() ? 0 : 0x20)));
            nextHeader.append(headerMethodId_);
            if (!headerMethodProperties_.isEmpty()) {
                appendNumber(&nextHeader, quint64(headerMethodProperties_.size()));
                nextHeader.append(headerMethodProperties_);
            }
        }
        nextHeader.append(char(12));
        if (headerEncrypted_ && headerMethodId_ != QByteArray(1, char(0)))
            appendNumber(&nextHeader, encodedSize);
        appendNumber(&nextHeader, unpackedSize);
        nextHeader.append(char(10));
        appendDigestSet(&nextHeader, std::vector<quint32>(1, unpackedCrc));
        nextHeader.append(char(0));
        nextHeader.append(char(0));
    }
    const quint64 nextHeaderOffset = quint64(output.pos()) - 32;
    const quint64 nextHeaderSize = quint64(nextHeader.size());
    const quint32 nextHeaderCrc = crc32(nextHeader);
    if (!writeAll(&output, nextHeader.constData(), nextHeader.size())) {
        if (error) *error = output.errorString();
        return false;
    }
    QByteArray startHeader;
    append64(&startHeader, nextHeaderOffset);
    append64(&startHeader, nextHeaderSize);
    append32(&startHeader, nextHeaderCrc);
    const quint32 startHeaderCrc = crc32(startHeader);
    QByteArray signatureHeader(reinterpret_cast<const char*>(kSignature), sizeof(kSignature));
    signatureHeader.append(char(0));
    signatureHeader.append(char(4));
    append32(&signatureHeader, startHeaderCrc);
    signatureHeader.append(startHeader);
    if (!output.seek(0) || !writeAll(&output, signatureHeader.constData(),
                                     signatureHeader.size())) {
        if (error) *error = output.errorString();
        return false;
    }
    if (!output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    return true;
}

bool SevenZipArchiveWriter::finish(QString* error)
{
    if (!active_) {
        if (error) *error = QStringLiteral("No 7z write is active.");
        return false;
    }
    if (!writeArchive(error)) return false;
    abort();
    return true;
}

void SevenZipArchiveWriter::abort()
{
    password_.fill(QChar(0));
    password_.clear();
    staging_.reset();
    entries_.clear();
    nextStagedFile_ = 0;
    solidBlockSize_ = 0;
    headerCompressed_ = false;
    headerEncrypted_ = false;
    dataEncrypted_ = false;
    headerAesProperties_.clear();
    headerMethodId_.clear();
    headerMethodProperties_.clear();
    outputPath_.clear();
    active_ = false;
}

} // namespace writer
} // namespace peare
