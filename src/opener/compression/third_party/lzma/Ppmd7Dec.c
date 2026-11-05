/* Ppmd7Dec.c -- Ppmd7z (PPMdH with 7z Range Coder) Decoder
2023-09-07 : Igor Pavlov : Public domain
This code is based on:
  PPMd var.H (2001): Dmitry Shkarin : Public domain */


#include "Precomp.h"

#include "Ppmd7.h"

#define kTopValue ((UInt32)1 << 24)
#define kBottomValue ((UInt32)1 << 15)


#define READ_BYTE(p) IByteIn_Read((p)->Stream)

BoolInt Ppmd7z_RangeDec_Init(CPpmd7_RangeDec *p)
{
  unsigned i;
  p->Code = 0;
  p->Range = 0xFFFFFFFF;
  if (READ_BYTE(p) != 0)
    return False;
  for (i = 0; i < 4; i++)
    p->Code = (p->Code << 8) | READ_BYTE(p);
  return (p->Code < 0xFFFFFFFF);
}

BoolInt Ppmd7r_RangeDec_Init(CPpmd7_RangeDec *p)
{
  unsigned i;
  p->Low = 0;
  p->Code = 0;
  p->Range = 0xFFFFFFFF;
  for (i = 0; i < 4; i++)
    p->Code = (p->Code << 8) | READ_BYTE(p);
  return True;
}

static UInt32 Ppmd7z_GetThreshold(CPpmd7 *p, UInt32 total)
{
  UInt32 range = p->rc.dec.Range /= total;
  return p->rc.dec.Code / range;
}

static void Ppmd7z_DecodeInterval(CPpmd7 *p, UInt32 start, UInt32 size)
{
  p->rc.dec.Code -= start * p->rc.dec.Range;
  p->rc.dec.Range *= size;
}

static void Ppmd7z_Normalize(CPpmd7_RangeDec *range)
{
  if (range->Range < kTopValue) {
    range->Code = (range->Code << 8) | READ_BYTE(range);
    range->Range <<= 8;
  }
}

static UInt32 Ppmd7r_GetThreshold(CPpmd7 *p, UInt32 total)
{
  UInt32 range = p->rc.dec.Range /= total;
  return (p->rc.dec.Code - p->rc.dec.Low) / range;
}

static void Ppmd7r_Normalize(CPpmd7_RangeDec *range)
{
  while ((range->Low ^ (range->Low + range->Range)) < kTopValue ||
         range->Range < kBottomValue) {
    if (range->Range < kBottomValue)
      range->Range = (0 - range->Low) & (kBottomValue - 1);
    range->Code = (range->Code << 8) | READ_BYTE(range);
    range->Range <<= 8;
    range->Low <<= 8;
  }
}

static void Ppmd7r_DecodeInterval(CPpmd7 *p, UInt32 start, UInt32 size)
{
  p->rc.dec.Low += start * p->rc.dec.Range;
  p->rc.dec.Range *= size;
  Ppmd7r_Normalize(&p->rc.dec);
}

#define R (&p->rc.dec)

Z7_FORCE_INLINE
// Z7_NO_INLINE
static void Ppmd7z_RD_Decode(CPpmd7 *p, UInt32 start, UInt32 size)
{
  Ppmd7z_DecodeInterval(p, start, size);
}

#define RC_NORM_1(range) Ppmd7z_Normalize(range);
#define RC_NORM_REMOTE(range) { Ppmd7z_Normalize(range); Ppmd7z_Normalize(range); }
#define RC_Decode(start, size)  Ppmd7z_RD_Decode(p, start, size);
#define RC_DecodeFinal(start, size)  RC_Decode(start, size) RC_NORM_REMOTE(R)
#define RC_GetThreshold(total)  Ppmd7z_GetThreshold(p, total)


#define CTX(ref) ((CPpmd7_Context *)Ppmd7_GetContext(p, ref))
// typedef CPpmd7_Context * CTX_PTR;
#define SUCCESSOR(p) Ppmd_GET_SUCCESSOR(p)
void Ppmd7_UpdateModel(CPpmd7 *p);

#define MASK(sym)  ((Byte *)charMask)[sym]
// Z7_FORCE_INLINE
// static
static int Ppmd7z_DecodeSymbolCore(CPpmd7 *p)
{
  size_t charMask[256 / sizeof(size_t)];

  if (p->MinContext->NumStats != 1)
  {
    CPpmd_State *s = Ppmd7_GetStats(p, p->MinContext);
    unsigned i;
    UInt32 count, hiCnt;
    const UInt32 summFreq = p->MinContext->Union2.SummFreq;

    
    
    
    count = RC_GetThreshold(summFreq);
    hiCnt = count;
    
    if ((Int32)(count -= s->Freq) < 0)
    {
      Byte sym;
      RC_DecodeFinal(0, s->Freq)
      p->FoundState = s;
      sym = s->Symbol;
      Ppmd7_Update1_0(p);
      return sym;
    }
  
    p->PrevSuccess = 0;
    i = (unsigned)p->MinContext->NumStats - 1;
    
    do
    {
      if ((Int32)(count -= (++s)->Freq) < 0)
      {
        Byte sym;
        RC_DecodeFinal((hiCnt - count) - s->Freq, s->Freq)
        p->FoundState = s;
        sym = s->Symbol;
        Ppmd7_Update1(p);
        return sym;
      }
    }
    while (--i);
    
    if (hiCnt >= summFreq) {
      return PPMD7_SYM_ERROR;
    }
    
    hiCnt -= count;
    RC_Decode(hiCnt, summFreq - hiCnt)

    p->HiBitsFlag = PPMD7_HiBitsFlag_3(p->FoundState->Symbol);
    PPMD_SetAllBitsIn256Bytes(charMask)
    // i = p->MinContext->NumStats - 1;
    // do { MASK((--s)->Symbol) = 0; } while (--i);
    {
      CPpmd_State *s2 = Ppmd7_GetStats(p, p->MinContext);
      MASK(s->Symbol) = 0;
      do
      {
        const unsigned sym0 = s2[0].Symbol;
        const unsigned sym1 = s2[1].Symbol;
        s2 += 2;
        MASK(sym0) = 0;
        MASK(sym1) = 0;
      }
      while (s2 < s);
    }
  }
  else
  {
    CPpmd_State *s = Ppmd7Context_OneState(p->MinContext);
    UInt16 *prob = Ppmd7_GetBinSumm(p);
    UInt32 prob0 = *prob;
    UInt32 size0 = (R->Range >> 14) * prob0;
    UInt32 pr = PPMD_UPDATE_PROB_1(prob0);

    if (R->Code < size0)
    {
      Byte sym;
      *prob = (UInt16)(pr + (1 << PPMD_INT_BITS));
      
      // RangeDec_DecodeBit0(size0);
      R->Range = size0;
      RC_NORM_1(R)
      /* we can use single byte normalization here because of
         (min(BinSumm[][]) = 95) > (1 << (14 - 8)) */

      // sym = (p->FoundState = Ppmd7Context_OneState(p->MinContext))->Symbol;
      // Ppmd7_UpdateBin(p);
      {
        unsigned freq = s->Freq;
        CPpmd7_Context *c = CTX(SUCCESSOR(s));
        sym = s->Symbol;
        p->FoundState = s;
        p->PrevSuccess = 1;
        p->RunLength++;
        s->Freq = (Byte)(freq + (freq < 128));
        // NextContext(p);
        if (p->OrderFall == 0 && (const Byte *)c > p->Text)
          p->MaxContext = p->MinContext = c;
        else
          Ppmd7_UpdateModel(p);
      }
      return sym;
    }

    *prob = (UInt16)pr;
    p->InitEsc = p->ExpEscape[pr >> 10];

    // RangeDec_DecodeBit1(size0);
    
    R->Code -= size0;
    R->Range -= size0;
    
    PPMD_SetAllBitsIn256Bytes(charMask)
    MASK(Ppmd7Context_OneState(p->MinContext)->Symbol) = 0;
    p->PrevSuccess = 0;
  }

  for (;;)
  {
    CPpmd_State *s, *s2;
    UInt32 freqSum, count, hiCnt;

    CPpmd_See *see;
    CPpmd7_Context *mc;
    unsigned numMasked;
    RC_NORM_REMOTE(R)
    mc = p->MinContext;
    numMasked = mc->NumStats;

    do
    {
      p->OrderFall++;
      if (!mc->Suffix)
        return PPMD7_SYM_END;
      mc = Ppmd7_GetContext(p, mc->Suffix);
    }
    while (mc->NumStats == numMasked);
    
    s = Ppmd7_GetStats(p, mc);

    {
      unsigned num = mc->NumStats;
      unsigned num2 = num / 2;
      
      num &= 1;
      hiCnt = (s->Freq & (UInt32)(MASK(s->Symbol))) & (0 - (UInt32)num);
      s += num;
      p->MinContext = mc;

      do
      {
        const unsigned sym0 = s[0].Symbol;
        const unsigned sym1 = s[1].Symbol;
        s += 2;
        hiCnt += (s[-2].Freq & (UInt32)(MASK(sym0)));
        hiCnt += (s[-1].Freq & (UInt32)(MASK(sym1)));
      }
      while (--num2);
    }

    see = Ppmd7_MakeEscFreq(p, numMasked, &freqSum);
    freqSum += hiCnt;




    count = RC_GetThreshold(freqSum);
    
    if (count < hiCnt)
    {
      Byte sym;

      s = Ppmd7_GetStats(p, p->MinContext);
      hiCnt = count;
      // count -= s->Freq & (UInt32)(MASK(s->Symbol));
      // if ((Int32)count >= 0)
      {
        for (;;)
        {
          count -= s->Freq & (UInt32)(MASK((s)->Symbol)); s++; if ((Int32)count < 0) break;
          // count -= s->Freq & (UInt32)(MASK((s)->Symbol)); s++; if ((Int32)count < 0) break;
        }
      }
      s--;
      RC_DecodeFinal((hiCnt - count) - s->Freq, s->Freq)

      // new (see->Summ) value can overflow over 16-bits in some rare cases
      Ppmd_See_UPDATE(see)
      p->FoundState = s;
      sym = s->Symbol;
      Ppmd7_Update2(p);
      return sym;
    }

    if (count >= freqSum) {
      return PPMD7_SYM_ERROR;
    }
    
    RC_Decode(hiCnt, freqSum - hiCnt)

    // We increase (see->Summ) for sum of Freqs of all non_Masked symbols.
    // new (see->Summ) value can overflow over 16-bits in some rare cases
    see->Summ = (UInt16)(see->Summ + freqSum);

    s = Ppmd7_GetStats(p, p->MinContext);
    s2 = s + p->MinContext->NumStats;
    do
    {
      MASK(s->Symbol) = 0;
      s++;
    }
    while (s != s2);
  }
}

int Ppmd7z_DecodeSymbol(CPpmd7 *p)
{
  return Ppmd7z_DecodeSymbolCore(p);
}

int Ppmd7r_DecodeSymbol(CPpmd7 *p)
{
  size_t charMask[256 / sizeof(size_t)];

  if (p->MinContext->NumStats != 1)
  {
    CPpmd_State *s = Ppmd7_GetStats(p, p->MinContext);
    unsigned i;
    UInt32 count, hiCnt;
    const UInt32 summFreq = p->MinContext->Union2.SummFreq;

    count = Ppmd7r_GetThreshold(p, summFreq);
    hiCnt = count;

    if ((Int32)(count -= s->Freq) < 0)
    {
      Byte sym;
      Ppmd7r_DecodeInterval(p, 0, s->Freq);
      p->FoundState = s;
      sym = s->Symbol;
      Ppmd7_Update1_0(p);
      return sym;
    }

    p->PrevSuccess = 0;
    i = (unsigned)p->MinContext->NumStats - 1;
    do
    {
      if ((Int32)(count -= (++s)->Freq) < 0)
      {
        Byte sym;
        Ppmd7r_DecodeInterval(p, (hiCnt - count) - s->Freq, s->Freq);
        p->FoundState = s;
        sym = s->Symbol;
        Ppmd7_Update1(p);
        return sym;
      }
    }
    while (--i);

    if (hiCnt >= summFreq)
      return PPMD7_SYM_ERROR;

    hiCnt -= count;
    Ppmd7r_DecodeInterval(p, hiCnt, summFreq - hiCnt);

    p->HiBitsFlag = PPMD7_HiBitsFlag_3(p->FoundState->Symbol);
    PPMD_SetAllBitsIn256Bytes(charMask)
    {
      CPpmd_State *s2 = Ppmd7_GetStats(p, p->MinContext);
      MASK(s->Symbol) = 0;
      do
      {
        const unsigned sym0 = s2[0].Symbol;
        const unsigned sym1 = s2[1].Symbol;
        s2 += 2;
        MASK(sym0) = 0;
        MASK(sym1) = 0;
      }
      while (s2 < s);
    }
  }
  else
  {
    CPpmd_State *s = Ppmd7Context_OneState(p->MinContext);
    UInt16 *prob = Ppmd7_GetBinSumm(p);
    UInt32 prob0 = *prob;
    UInt32 rangeUnit = R->Range >> 14;
    UInt32 size0 = rangeUnit * prob0;
    UInt32 pr = PPMD_UPDATE_PROB_1(prob0);

    if ((R->Code - R->Low) < size0)
    {
      Byte sym;
      *prob = (UInt16)(pr + (1 << PPMD_INT_BITS));
      R->Range = size0;
      Ppmd7r_Normalize(R);

      {
        unsigned freq = s->Freq;
        CPpmd7_Context *c = CTX(SUCCESSOR(s));
        sym = s->Symbol;
        p->FoundState = s;
        p->PrevSuccess = 1;
        p->RunLength++;
        s->Freq = (Byte)(freq + (freq < 128));
        if (p->OrderFall == 0 && (const Byte *)c > p->Text)
          p->MaxContext = p->MinContext = c;
        else
          Ppmd7_UpdateModel(p);
      }
      return sym;
    }

    *prob = (UInt16)pr;
    p->InitEsc = p->ExpEscape[pr >> 10];
    R->Low += size0;
    R->Range = rangeUnit * (0x4000 - prob0);
    Ppmd7r_Normalize(R);

    PPMD_SetAllBitsIn256Bytes(charMask)
    MASK(Ppmd7Context_OneState(p->MinContext)->Symbol) = 0;
    p->PrevSuccess = 0;
  }

  for (;;)
  {
    CPpmd_State *s, *s2;
    UInt32 freqSum, count, hiCnt;
    CPpmd_See *see;
    CPpmd7_Context *mc;
    unsigned numMasked;

    mc = p->MinContext;
    numMasked = mc->NumStats;
    do
    {
      p->OrderFall++;
      if (!mc->Suffix)
        return PPMD7_SYM_END;
      mc = Ppmd7_GetContext(p, mc->Suffix);
    }
    while (mc->NumStats == numMasked);

    s = Ppmd7_GetStats(p, mc);
    {
      unsigned num = mc->NumStats;
      unsigned num2 = num / 2;
      num &= 1;
      hiCnt = (s->Freq & (UInt32)(MASK(s->Symbol))) & (0 - (UInt32)num);
      s += num;
      p->MinContext = mc;
      do
      {
        const unsigned sym0 = s[0].Symbol;
        const unsigned sym1 = s[1].Symbol;
        s += 2;
        hiCnt += (s[-2].Freq & (UInt32)(MASK(sym0)));
        hiCnt += (s[-1].Freq & (UInt32)(MASK(sym1)));
      }
      while (--num2);
    }

    see = Ppmd7_MakeEscFreq(p, numMasked, &freqSum);
    freqSum += hiCnt;
    count = Ppmd7r_GetThreshold(p, freqSum);

    if (count < hiCnt)
    {
      Byte sym;
      s = Ppmd7_GetStats(p, p->MinContext);
      hiCnt = count;
      for (;;)
      {
        count -= s->Freq & (UInt32)MASK(s->Symbol);
        s++;
        if ((Int32)count < 0) break;
      }
      s--;
      Ppmd7r_DecodeInterval(p, (hiCnt - count) - s->Freq, s->Freq);
      Ppmd_See_UPDATE(see)
      p->FoundState = s;
      sym = s->Symbol;
      Ppmd7_Update2(p);
      return sym;
    }

    if (count >= freqSum)
      return PPMD7_SYM_ERROR;

    Ppmd7r_DecodeInterval(p, hiCnt, freqSum - hiCnt);
    see->Summ = (UInt16)(see->Summ + freqSum);

    s = Ppmd7_GetStats(p, p->MinContext);
    s2 = s + p->MinContext->NumStats;
    do
    {
      MASK(s->Symbol) = 0;
      s++;
    }
    while (s != s2);
  }
}

/*
Byte *Ppmd7z_DecodeSymbols(CPpmd7 *p, Byte *buf, const Byte *lim)
{
  int sym = 0;
  if (buf != lim)
  do
  {
    sym = Ppmd7z_DecodeSymbol(p);
    if (sym < 0)
      break;
    *buf = (Byte)sym;
  }
  while (++buf < lim);
  p->LastSymbol = sym;
  return buf;
}
*/

#undef kTopValue
#undef kBottomValue
#undef READ_BYTE
#undef RC_NORM_1
#undef RC_NORM
#undef RC_NORM_LOCAL
#undef RC_NORM_REMOTE
#undef R
#undef RC_Decode
#undef RC_DecodeFinal
#undef RC_GetThreshold
#undef CTX
#undef SUCCESSOR
#undef MASK
