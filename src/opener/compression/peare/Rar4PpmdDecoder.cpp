#include "Rar4PpmdDecoder.h"

#include <cstring>

extern "C" void Ppmd7_UpdateModel(CPpmd7 *model);

namespace {

const uint32_t kTopValue = 1u << 24;
const uint32_t kBottomValue = 1u << 15;
const uint32_t kBinaryScale = 1u << 14;

enum DecoderStatus {
    kReady = 0,
    kInputError = 1,
    kModelError = 2
};

bool readByte(PeareRar4PpmdDecoder *decoder, uint8_t *value)
{
    if (!decoder->read_byte(decoder->read_context, value)) {
        decoder->status = kInputError;
        return false;
    }
    return true;
}

bool normalize(PeareRar4PpmdDecoder *decoder, CPpmd7 *model)
{
    for (;;) {
        const uint32_t upper = decoder->low + decoder->range;
        if ((decoder->low ^ upper) >= kTopValue) {
            if (decoder->range >= kBottomValue)
                break;
            decoder->range = (0u - decoder->low) & (kBottomValue - 1);
        }

        uint8_t next;
        if (!readByte(decoder, &next))
            return false;
        decoder->code = (decoder->code << 8) | next;
        decoder->range <<= 8;
        decoder->low <<= 8;
    }
    model->rc.dec.Low = decoder->low;
    model->rc.dec.Range = decoder->range;
    model->rc.dec.Code = decoder->code;
    return true;
}

bool threshold(PeareRar4PpmdDecoder *decoder, uint32_t total,
               uint32_t *unit, uint32_t *count)
{
    if (total == 0 || decoder->range < total) {
        decoder->status = kModelError;
        return false;
    }
    *unit = decoder->range / total;
    if (*unit == 0) {
        decoder->status = kModelError;
        return false;
    }
    *count = (uint32_t)(decoder->code - decoder->low) / *unit;
    if (*count >= total) {
        decoder->status = kModelError;
        return false;
    }
    return true;
}

bool updateInterval(PeareRar4PpmdDecoder *decoder, uint32_t start,
                    uint32_t size, uint32_t unit)
{
    if (size == 0) {
        decoder->status = kModelError;
        return false;
    }
    decoder->low += start * unit;
    decoder->range = size * unit;
    return true;
}

void maskContextSymbols(uint8_t mask[256], const CPpmd_State *states,
                        unsigned stateCount)
{
    for (unsigned index = 0; index < stateCount; ++index)
        mask[states[index].Symbol] = 0;
}

void updateBinarySuccess(CPpmd7 *model)
{
    CPpmd_State *state = model->FoundState;
    if (state->Freq < 128)
        ++state->Freq;
    model->PrevSuccess = 1;
    ++model->RunLength;

    const CPpmd_Void_Ref successorRef = Ppmd_GET_SUCCESSOR(state);
    Byte *successorAddress = (Byte *)Ppmd7_GetPtr(model, successorRef);
    if (model->OrderFall == 0 && successorAddress > model->Text) {
        model->MinContext = (CPpmd7_Context *)successorAddress;
        model->MaxContext = model->MinContext;
        return;
    }
    Ppmd7_UpdateModel(model);
}

int decodeSymbolImpl(PeareRar4PpmdDecoder *decoder, CPpmd7 *model)
{
    uint8_t masked[256];
    std::memset(masked, 1, sizeof(masked));

    if (!model->MinContext || !model->FoundState || !model->Base ||
        !model->MinContext->NumStats) {
        decoder->status = kModelError;
        return PEARE_RAR4_PPMD_ERROR;
    }

    CPpmd7_Context *context = model->MinContext;
    if (context->NumStats != 1) {
        CPpmd_State *states = Ppmd7_GetStats(model, context);
        uint32_t unit, count;
        if (!states || !threshold(decoder, context->Union2.SummFreq,
                                  &unit, &count))
            return PEARE_RAR4_PPMD_ERROR;

        uint32_t cumulative = states[0].Freq;
        if (count < cumulative) {
            if (!updateInterval(decoder, 0, states[0].Freq, unit) ||
                !normalize(decoder, model))
                return PEARE_RAR4_PPMD_ERROR;
            model->FoundState = &states[0];
            const int symbol = states[0].Symbol;
            Ppmd7_Update1_0(model);
            return symbol;
        }

        model->PrevSuccess = 0;
        unsigned i;
        for (i = 1; i < context->NumStats; ++i) {
            cumulative += states[i].Freq;
            if (count < cumulative) {
                if (!updateInterval(decoder, cumulative - states[i].Freq,
                                    states[i].Freq, unit) ||
                    !normalize(decoder, model))
                    return PEARE_RAR4_PPMD_ERROR;
                model->FoundState = &states[i];
                const int symbol = states[i].Symbol;
                Ppmd7_Update1(model);
                return symbol;
            }
        }

        if (cumulative >= context->Union2.SummFreq ||
            !updateInterval(decoder, cumulative,
                            context->Union2.SummFreq - cumulative, unit))
            return PEARE_RAR4_PPMD_ERROR;
        model->HiBitsFlag = PPMD7_HiBitsFlag_3(model->FoundState->Symbol);
        maskContextSymbols(masked, states, context->NumStats);
    } else {
        CPpmd_State *state = Ppmd7Context_OneState(context);
        UInt16 *probability = Ppmd7_GetBinSumm(model);
        uint32_t unit, count;
        if (!threshold(decoder, kBinaryScale, &unit, &count))
            return PEARE_RAR4_PPMD_ERROR;

        const uint32_t split = *probability;
        if (count < split) {
            if (!updateInterval(decoder, 0, split, unit) ||
                !normalize(decoder, model))
                return PEARE_RAR4_PPMD_ERROR;
            *probability = (UInt16)PPMD_UPDATE_PROB_0(*probability);
            model->FoundState = state;
            const int symbol = state->Symbol;
            updateBinarySuccess(model);
            return symbol;
        }

        if (!updateInterval(decoder, split, kBinaryScale - split, unit))
            return PEARE_RAR4_PPMD_ERROR;
        *probability = (UInt16)PPMD_UPDATE_PROB_1(*probability);
        model->InitEsc = model->ExpEscape[*probability >> 10];
        maskContextSymbols(masked, state, 1);
        model->PrevSuccess = 0;
    }

    for (;;) {
        if (!normalize(decoder, model))
            return PEARE_RAR4_PPMD_ERROR;

        const unsigned maskedCount = context->NumStats;
        do {
            ++model->OrderFall;
            if (context->Suffix == 0)
                return PEARE_RAR4_PPMD_END;
            context = Ppmd7_GetContext(model, context->Suffix);
        } while (context->NumStats == maskedCount);
        if (!context->NumStats) {
            decoder->status = kModelError;
            return PEARE_RAR4_PPMD_ERROR;
        }

        model->MinContext = context;
        uint32_t nonMaskedTotal = 0;
        CPpmd_State *states = context->NumStats == 1
            ? Ppmd7Context_OneState(context)
            : Ppmd7_GetStats(model, context);
        if (!states) {
            decoder->status = kModelError;
            return PEARE_RAR4_PPMD_ERROR;
        }
        for (unsigned i = 0; i < context->NumStats; ++i) {
            if (masked[states[i].Symbol])
                nonMaskedTotal += states[i].Freq;
        }

        UInt32 escapeFrequency;
        CPpmd_See *see = Ppmd7_MakeEscFreq(model, maskedCount,
                                           &escapeFrequency);
        if (!see || escapeFrequency == 0 ||
            nonMaskedTotal > UINT32_MAX - escapeFrequency) {
            decoder->status = kModelError;
            return PEARE_RAR4_PPMD_ERROR;
        }
        const uint32_t total = nonMaskedTotal + escapeFrequency;
        uint32_t unit, count;
        if (!threshold(decoder, total, &unit, &count))
            return PEARE_RAR4_PPMD_ERROR;

        if (count < nonMaskedTotal) {
            uint32_t cumulative = 0;
            for (unsigned i = 0; i < context->NumStats; ++i) {
                if (!masked[states[i].Symbol])
                    continue;
                cumulative += states[i].Freq;
                if (count < cumulative) {
                    if (!updateInterval(decoder,
                                        cumulative - states[i].Freq,
                                        states[i].Freq, unit) ||
                        !normalize(decoder, model))
                        return PEARE_RAR4_PPMD_ERROR;
                    if (see->Shift < PPMD_PERIOD_BITS && --see->Count == 0) {
                        see->Summ = (UInt16)(see->Summ << 1);
                        see->Count = (Byte)(3u << see->Shift++);
                    }
                    model->FoundState = &states[i];
                    const int symbol = states[i].Symbol;
                    Ppmd7_Update2(model);
                    return symbol;
                }
            }
            decoder->status = kModelError;
            return PEARE_RAR4_PPMD_ERROR;
        }

        if (!updateInterval(decoder, nonMaskedTotal, escapeFrequency, unit))
            return PEARE_RAR4_PPMD_ERROR;
        see->Summ = (UInt16)(see->Summ + total);
        maskContextSymbols(masked, states, context->NumStats);
    }
}

} // namespace

extern "C" int peare_rar4_ppmd_init(PeareRar4PpmdDecoder *decoder,
                                     PeareRar4PpmdReadByte read_byte,
                                     void *read_context)
{
    if (!decoder || !read_byte)
        return 0;
    decoder->low = 0;
    decoder->range = 0xFFFFFFFFu;
    decoder->code = 0;
    decoder->read_byte = read_byte;
    decoder->read_context = read_context;
    decoder->status = kReady;
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t byte;
        if (!readByte(decoder, &byte))
            return 0;
        decoder->code = (decoder->code << 8) | byte;
    }
    return 1;
}

extern "C" int peare_rar4_ppmd_decode_symbol(
    PeareRar4PpmdDecoder *decoder, CPpmd7 *model)
{
    if (!decoder || !model || decoder->status != kReady ||
        !decoder->read_byte)
        return PEARE_RAR4_PPMD_ERROR;
    return decodeSymbolImpl(decoder, model);
}
