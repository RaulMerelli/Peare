#include "OS2_RT_DIALOG.h"

#include <QTextStream>
#include <QVector>
#include <stdexcept>

namespace peare {
namespace resources {
namespace {

quint16 readUInt16(const QByteArray& buffer, int offset)
{
    if (offset < 0 || offset + 2 > buffer.size())
        throw std::out_of_range("RT_DIALOG uint16 out of range");
    const auto* p = reinterpret_cast<const uchar*>(buffer.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 readUInt32(const QByteArray& buffer, int offset)
{
    if (offset < 0 || offset + 4 > buffer.size())
        throw std::out_of_range("RT_DIALOG uint32 out of range");
    const auto* p = reinterpret_cast<const uchar*>(buffer.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString hexValue(quint32 value, int width)
{
    return QStringLiteral("0x%1").arg(value, width, 16, QChar(u'0')).toUpper();
}

static const QVector<QString> par_pp = {
    QStringLiteral("0"), QStringLiteral("PP_FOREGROUNDCOLOR"), QStringLiteral("PP_FOREGROUNDCOLORINDEX"), QStringLiteral("PP_BACKGROUNDCOLOR"),
    QStringLiteral("PP_BACKGROUNDCOLORINDEX"), QStringLiteral("PP_HILITEFOREGROUNDCOLOR"), QStringLiteral("PP_HILITEFOREGROUNDCOLORINDEX"), QStringLiteral("PP_HILITEBACKGROUNDCOLOR"),
    QStringLiteral("PP_HILITEBACKGROUNDCOLORINDEX"), QStringLiteral("PP_DISABLEDFOREGROUNDCOLOR"), QStringLiteral("PP_DISABLEDFOREGROUNDCOLORINDEX"), QStringLiteral("PP_DISABLEDBACKGROUNDCOLOR"),
    QStringLiteral("PP_DISABLEDBACKGROUNDCOLORINDEX"), QStringLiteral("PP_BORDERCOLOR"), QStringLiteral("PP_BORDERCOLORINDEX"), QStringLiteral("PP_FONTNAMESIZE"),
    QStringLiteral("PP_FONTHANDLE"), QStringLiteral("PP_RESERVED"), QStringLiteral("PP_ACTIVECOLOR"), QStringLiteral("PP_ACTIVECOLORINDEX"),
    QStringLiteral("PP_INACTIVECOLOR"), QStringLiteral("PP_INACTIVECOLORINDEX"), QStringLiteral("PP_ACTIVETEXTFGNDCOLOR"), QStringLiteral("PP_ACTIVETEXTFGNDCOLORINDEX"),
    QStringLiteral("PP_ACTIVETEXTBGNDCOLOR"), QStringLiteral("PP_ACTIVETEXTBGNDCOLORINDEX"), QStringLiteral("PP_INACTIVETEXTFGNDCOLOR"), QStringLiteral("PP_INACTIVETEXTFGNDCOLORINDEX"),
    QStringLiteral("PP_INACTIVETEXTBGNDCOLOR"), QStringLiteral("PP_INACTIVETEXTBGNDCOLORINDEX"), QStringLiteral("PP_SHADOW"), QStringLiteral("PP_MENUFOREGROUNDCOLOR"),
    QStringLiteral("PP_MENUFOREGROUNDCOLORINDEX"), QStringLiteral("PP_MENUBACKGROUNDCOLOR"), QStringLiteral("PP_MENUBACKGROUNDCOLORINDEX"), QStringLiteral("PP_MENUHILITEFGNDCOLOR"),
    QStringLiteral("PP_MENUHILITEFGNDCOLORINDEX"), QStringLiteral("PP_MENUHILITEBGNDCOLOR"), QStringLiteral("PP_MENUHILITEBGNDCOLORINDEX"), QStringLiteral("PP_MENUDISABLEDFGNDCOLOR"),
    QStringLiteral("PP_MENUDISABLEDFGNDCOLORINDEX"), QStringLiteral("PP_MENUDISABLEDBGNDCOLOR"), QStringLiteral("PP_MENUDISABLEDBGNDCOLORINDEX"), QStringLiteral("PP_SHADOWTEXTCOLOR"),
    QStringLiteral("PP_SHADOWTEXTCOLORINDEX"), QStringLiteral("PP_SHADOWHILITEFGNDCOLOR"), QStringLiteral("PP_SHADOWHILITEFGNDCOLORINDEX"), QStringLiteral("PP_SHADOWHILITEBGNDCOLOR"),
    QStringLiteral("PP_SHADOWHILITEBGNDCOLORINDEX"), QStringLiteral("PP_ICONTEXTBACKGROUNDCOLOR"), QStringLiteral("PP_ICONTEXTBACKGROUNDCOLORINDEX"), QString(),
};

static const QVector<QString> par_wc = {
    QString(), QStringLiteral("WC_FRAME"), QStringLiteral("WC_COMBOBOX"), QStringLiteral("WC_BUTTON"),
    QStringLiteral("WC_MENU"), QStringLiteral("WC_STATIC"), QStringLiteral("WC_ENTRYFIELD"), QStringLiteral("WC_LISTBOX"),
    QStringLiteral("WC_SCROLLBAR"), QStringLiteral("WC_TITLEBAR"), QStringLiteral("WC_MLE"), QString(),
    QString(), QString(), QString(), QString(),
    QStringLiteral("WC_APPSTAT"), QStringLiteral("WC_KBDSTAT"), QStringLiteral("WC_PECIC"), QStringLiteral("WC_DBE_KKPOPUP"),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QStringLiteral("WC_SPINBUTTON"),
    QString(), QString(), QString(), QString(),
    QStringLiteral("WC_CONTAINER"), QStringLiteral("WC_SLIDER"), QStringLiteral("WC_VALUESET"), QStringLiteral("WC_NOTEBOOK"),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QStringLiteral("WC_CIRCULARSLIDER"), QString(),
};

static const QVector<QString> par_ws = {
    QStringLiteral("WS_GROUP"), QStringLiteral("WS_TABSTOP"), QStringLiteral("WS_MULTISELECT"), QStringLiteral("WS_ANIMATE"),
    QStringLiteral("WS_MAXIMIZED"), QStringLiteral("WS_MINIMIZED"), QStringLiteral("WS_SYNCPAINT"), QStringLiteral("WS_SAVEBITS"),
    QStringLiteral("WS_PARENTCLIP"), QStringLiteral("WS_CLIPSIBLINGS"), QStringLiteral("WS_CLIPCHILDREN"), QStringLiteral("WS_DISABLED"),
    QStringLiteral("WS_VISIBLE"), QString(), QString(), QStringLiteral("WS_DBE_APPSTAT"),
    QString(),
};

static const QVector<QString> par_fs = {
    QStringLiteral("FS_ICON"), QStringLiteral("FS_ACCELTABLE"), QStringLiteral("FS_SHELLPOSITION"), QStringLiteral("FS_TASKLIST"),
    QStringLiteral("FS_NOBYTEALIGN"), QStringLiteral("FS_NOMOVEWITHOWNER"), QStringLiteral("FS_SYSMODAL"), QStringLiteral("FS_DLGBORDER"),
    QStringLiteral("FS_BORDER"), QStringLiteral("FS_SCREENALIGN"), QStringLiteral("FS_MOUSEALIGN"), QStringLiteral("FS_SIZEBORDER"),
    QStringLiteral("FS_AUTOICON"), QString(), QString(), QStringLiteral("FS_DBE_APPSTAT"),
    QString(),
};

static const QVector<QString> par_fcf = {
    QStringLiteral("FCF_TITLEBAR"), QStringLiteral("FCF_SYSMENU"), QStringLiteral("FCF_MENU"), QStringLiteral("FCF_SIZEBORDER"),
    QStringLiteral("FCF_MINBUTTON"), QStringLiteral("FCF_MAXBUTTON"), QStringLiteral("FCF_VERTSCROLL"), QStringLiteral("FCF_HORZSCROLL"),
    QStringLiteral("FCF_DLGBORDER"), QStringLiteral("FCF_BORDER"), QStringLiteral("FCF_SHELLPOSITION"), QStringLiteral("FCF_TASKLIST"),
    QStringLiteral("FCF_NOBYTEALIGN"), QStringLiteral("FCF_NOMOVEWITHOWNER"), QStringLiteral("FCF_ICON"), QStringLiteral("FCF_ACCELTABLE"),
    QStringLiteral("FCF_SYSMODAL"), QStringLiteral("FCF_SCREENALIGN"), QStringLiteral("FCF_MOUSEALIGN"), QString(),
    QString(), QString(), QString(), QStringLiteral("FCF_HIDEBUTTON"),
    QString(), QStringLiteral("FCF_CLOSEBUTTON"), QString(), QString(),
    QString(), QStringLiteral("FCF_AUTOICON"), QStringLiteral("FCF_DBE_APPSTAT"), QString(),
};

static const QVector<QString> par_cbs = {
    QStringLiteral("CBS_SIMPLE"), QStringLiteral("CBS_DROPDOWN"), QStringLiteral("CBS_DROPDOWNLIST"), QStringLiteral("LS_HORZSCROLL"),
    QStringLiteral("ES_AUTOTAB"), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QString(),
};

static const QVector<QString> par_bs1 = {
    QString(), QString(), QString(), QString(),
    QStringLiteral("BS_TEXT"), QStringLiteral("BS_MINIICON"), QStringLiteral("BS_BITMAP"), QStringLiteral("BS_ICON"),
    QStringLiteral("BS_HELP"), QStringLiteral("BS_SYSCOMMAND"), QStringLiteral("BS_DEFAULT"), QStringLiteral("BS_NOPOINTERFOCUS"),
    QStringLiteral("BS_NOBORDER"), QStringLiteral("BS_NOCURSORSELECT"), QStringLiteral("BS_AUTOSIZE"), QString(),
};

static const QVector<QString> par_bs2 = {
    QString(), QString(), QString(), QString(),
    QString(), QStringLiteral("BS_3STATE"), QStringLiteral("BS_AUTO3STATE"), QStringLiteral("BS_USERBUTTON"),
    QStringLiteral("BS_NOTEBOOK"), QString(),
};

static const QVector<QString> par_dt = {
    QString(), QString(), QString(), QString(),
    QString(), QString(), QStringLiteral("SS_AUTOSIZE"), QStringLiteral("DT_EXTERNALLEADING"),
    QStringLiteral("DT_CENTER"), QStringLiteral("DT_RIGHT"), QStringLiteral("DT_VCENTER"), QStringLiteral("DT_BOTTOM"),
    QStringLiteral("DT_HALFTONE"), QStringLiteral("DT_MNEMONIC"), QStringLiteral("DT_WORDBREAK"), QStringLiteral("DT_ERASERECT"),
    QString(),
};

static const QVector<QString> par_ss = {
    QString(), QString(), QString(), QString(),
    QStringLiteral("SS_BITMAP"), QStringLiteral("SS_FGNDRECT"), QStringLiteral("SS_HALFTONERECT"), QStringLiteral("SS_BKGNDRECT"),
    QStringLiteral("SS_FGNDFRAME"), QStringLiteral("SS_HALFTONEFRAME"), QStringLiteral("SS_BKGNDFRAME"), QStringLiteral("SS_SYSICON"),
    QString(),
};

static const QVector<QString> par_es1 = {
    QStringLiteral("ES_CENTER"), QStringLiteral("ES_RIGHT"), QStringLiteral("ES_AUTOSCROLL"), QStringLiteral("ES_MARGIN"),
    QStringLiteral("ES_AUTOTAB"), QStringLiteral("ES_READONLY"), QStringLiteral("ES_COMMAND"), QStringLiteral("ES_UNREADABLE"),
    QStringLiteral("ES_AUTOSIZE"), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
};

static const QVector<QString> par_es2 = {
    QString(), QStringLiteral("ES_SBCS"), QStringLiteral("ES_DBCS"), QStringLiteral("ES_MIXED"),
    QString(),
};

static const QVector<QString> par_ls = {
    QStringLiteral("LS_MULTIPLESEL"), QStringLiteral("LS_OWNERDRAW"), QStringLiteral("LS_NOADJUSTPOS"), QStringLiteral("LS_HORZSCROLL"),
    QStringLiteral("LS_EXTENDEDSEL"), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
};

static const QVector<QString> par_sbs = {
    QStringLiteral("SBS_VERT"), QStringLiteral("SBS_THUMBSIZE"), QStringLiteral("SBS_AUTOTRACK"), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
    QStringLiteral("SBS_AUTOSIZE"), QString(), QString(), QString(),
};

static const QVector<QString> par_mls = {
    QStringLiteral("MLS_WORDWRAP"), QStringLiteral("MLS_BORDER"), QStringLiteral("MLS_VSCROLL"), QStringLiteral("MLS_HSCROLL"),
    QStringLiteral("MLS_READONLY"), QStringLiteral("MLS_IGNORETAB"), QStringLiteral("MLS_DISABLEUNDO"), QString(),
    QString(), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
};

static const QVector<QString> par_spbs = {
    QStringLiteral("SPBS_NUMERICONLY"), QStringLiteral("SPBS_READONLY"), QStringLiteral("SPBS_JUSTRIGHT"), QStringLiteral("SPBS_JUSTLEFT"),
    QStringLiteral("SPBS_MASTER"), QStringLiteral("SPBS_NOBORDER"), QString(), QStringLiteral("SPBS_PADWITHZEROS"),
    QStringLiteral("SPBS_FASTSPIN"), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
};

static const QVector<QString> par_ccs = {
    QStringLiteral("CCS_EXTENDSEL"), QStringLiteral("CCS_MULTIPLESEL"), QStringLiteral("CCS_SINGLESEL"), QStringLiteral("CCS_AUTOPOSITION"),
    QStringLiteral("CCS_VERIFYPOINTERS"), QStringLiteral("CCS_READONLY"), QStringLiteral("CCS_MINIRECORDCORE"), QString(),
    QString(), QString(), QString(), QStringLiteral("CCS_MINIICONS"),
    QStringLiteral("CCS_NOCONTROLPTR"), QString(), QString(), QString(),
};

static const QVector<QString> par_sls1 = {
    QStringLiteral("SLS_VERTICAL"), QStringLiteral("SLS_BOTTOM"), QStringLiteral("SLS_TOP"), QStringLiteral("SLS_SNAPTOINCREMENT"),
    QStringLiteral("SLS_BUTTONSBOTTOM"), QStringLiteral("SLS_BUTTONSTOP"), QStringLiteral("SLS_OWNERDRAW"), QStringLiteral("SLS_READONLY"),
    QStringLiteral("SLS_RIBBONSTRIP"), QStringLiteral("SLS_HOMETOP"), QStringLiteral("SLS_PRIMARYSCALE2"), QString(),
    QString(), QString(), QString(), QString(),
};

static const QVector<QString> par_sls2 = {
    QStringLiteral("SLS_VERTICAL"), QStringLiteral("SLS_LEFT"), QStringLiteral("SLS_RIGHT"), QStringLiteral("SLS_SNAPTOINCREMENT"),
    QStringLiteral("SLS_BUTTONSLEFT"), QStringLiteral("SLS_BUTTONSRIGHT"), QStringLiteral("SLS_OWNERDRAW"), QStringLiteral("SLS_READONLY"),
    QStringLiteral("SLS_RIBBONSTRIP"), QStringLiteral("SLS_HOMERIGHT"), QStringLiteral("SLS_PRIMARYSCALE2"), QString(),
    QString(), QString(), QString(), QString(),
};

static const QVector<QString> par_vs = {
    QStringLiteral("VS_BITMAP"), QStringLiteral("VS_ICON"), QStringLiteral("VS_TEXT"), QStringLiteral("VS_RGB"),
    QStringLiteral("VS_COLORINDEX"), QStringLiteral("VS_BORDER"), QStringLiteral("VS_ITEMBORDER"), QStringLiteral("VS_SCALEBITMAPS"),
    QStringLiteral("VS_RIGHTTOLEFT"), QStringLiteral("VS_OWNERDRAW"), QString(), QString(),
    QString(), QString(), QString(), QString(),
};

static const QVector<QString> par_bks = {
    QStringLiteral("BKS_BACKPAGESBR"), QStringLiteral("BKS_BACKPAGESBL"), QStringLiteral("BKS_BACKPAGESTR"), QStringLiteral("BKS_BACKPAGESTL"),
    QStringLiteral("BKS_MAJORTABRIGHT"), QStringLiteral("BKS_MAJORTABLEFT"), QStringLiteral("BKS_MAJORTABTOP"), QStringLiteral("BKS_MAJORTABBOTTOM"),
    QStringLiteral("BKS_ROUNDEDTABS"), QStringLiteral("BKS_POLYGONTABS"), QStringLiteral("BKS_SPIRALBIND"), QString(),
    QStringLiteral("BKS_STATUSTEXTRIGHT"), QStringLiteral("BKS_STATUSTEXTCENTER"), QStringLiteral("BKS_TABTEXTRIGHT"), QStringLiteral("BKS_TABTEXTCENTER"),
};

static const QVector<QString> par_css = {
    QStringLiteral("CSS_NOBUTTON"), QStringLiteral("CSS_NOTEXT"), QStringLiteral("CSS_NONUMBER"), QStringLiteral("CSS_POINTSELECT"),
    QStringLiteral("CSS_360"), QStringLiteral("CSS_MIDPOINT"), QStringLiteral("CSS_PROPORTIONALTICKS"), QStringLiteral("CSS_NOTICKS"),
    QStringLiteral("CSS_CIRCULARVALUE"), QString(), QString(), QString(),
    QString(), QString(), QString(), QString(),
};


class Parser {
public:
    explicit Parser(const QByteArray& data) : buffer(data), stream(&output) {}

    QString parse()
    {
        // Special thanks to Paul Ratcliffe for the Res2Dlg utility.
        // This code is heavily ispired to it and I have used it to compare and test the results.
        // Many issues are still here and the result is mostly still not the same.
        stream << "#ifndef OS2_INCLUDED\n";
        stream << "   #ifndef INCL_NLS\n";
        stream << "      #define INCL_NLS\n";
        stream << "   #endif\n";
        stream << "   #include <os2.h>\n";
        stream << "#endif\n\n";
        stream << "#ifndef BS_NOTEBOOK\n";
        stream << "   #define BS_NOTEBOOK 8\n";
        stream << "#endif\n\n";
        stream << "#ifndef FCF_CLOSEBUTTON\n";
        stream << "   #define FCF_CLOSEBUTTON 0x04000000L\n";
        stream << "#endif\n\n";

        if (buffer.size() < 8)
            throw std::out_of_range("Insufficient data for OS/2 dialog header");

        stream << "CODEPAGE " << readUInt16(buffer, 4) << "\n";
        formatMemoryFlags(readUInt16(buffer, 0));
        stream << "DLGTEMPLATE " << readUInt16(buffer, 0) << outputBuffer2 << "\nBEGIN\n";

        int resourceDataPtr = readUInt16(buffer, 6);
        processBlock(resourceDataPtr, 0, 4);
        stream << "END\n";

        if (nullsDetected)
            stream << "/* Warning: Nulls detected in strings */\n";

        return output.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    }

private:
    const QByteArray& buffer;
    QString output;
    QTextStream stream;
    QString outputBuffer;
    QString outputBuffer2;
    QString idBuffer;
    bool nullsDetected = false;

    static QString spaces(int count) { return QString(count, QChar(u' ')); }

    void formatControlParameters(int parameterBlockPtr, ushort indentationLevel)
    {
        stream << spaces(indentationLevel) << "PRESPARAMS ";
        const quint32 paramValue = readUInt32(buffer, parameterBlockPtr);
        if (paramValue > 0x32u || int(paramValue) >= par_pp.size())
            stream << paramValue << ", ";
        else
            stream << par_pp[int(paramValue)] << ", ";

        if (paramValue == 15) {
            const int start = parameterBlockPtr + 8;
            int end = buffer.indexOf('\0', start);
            if (end < 0) end = buffer.size();
            stream << '"' << QString::fromLatin1(buffer.constData() + start, end - start) << "\"\n";
            return;
        }

        const quint32 paramCount = readUInt32(buffer, parameterBlockPtr + 4) >> 2;
        for (quint32 i = 0; i < paramCount; ++i) {
            if (i > 0) stream << ", ";
            stream << hexValue(readUInt32(buffer, parameterBlockPtr + 8 + int(4 * i)), 8);
        }

        const quint32 flags = readUInt32(buffer, parameterBlockPtr + 4) & 3;
        if (flags != 0) {
            if (paramCount > 0) stream << ", ";
            const int p = parameterBlockPtr + 8 + int(4 * paramCount);
            if (flags == 1)
                stream << hexValue(quint8(buffer.at(p)), 8);
            else if (flags == 2)
                stream << hexValue(readUInt16(buffer, p), 8);
            else
                stream << hexValue(readUInt32(buffer, p) & 0xFFFFFF, 8);
        }
        stream << "\n";
    }

    void writeFormatted2(int resourcePtr, quint32 dataLength)
    {
        const quint32 wordCount = dataLength >> 1;
        for (quint32 i = 0; i < wordCount; ++i) {
            if (i > 0) stream << ", ";
            stream << readUInt16(buffer, resourcePtr + int(2 * i));
        }
        stream << "\n";
    }

    quint32 formatAndPrintIndexedStyle(quint32 result, const QVector<QString>& styles, ushort styleTableSize)
    {
        if (result != 0) {
            if (!outputBuffer.isEmpty()) outputBuffer += QStringLiteral(" | ");
            if (styleTableSize <= result || int(result) >= styles.size() || styles[int(result)].isNull())
                outputBuffer += QStringLiteral("0x%1").arg(result, 0, 16).toUpper();
            else
                outputBuffer += styles[int(result)];
            result = 0;
        }
        return result;
    }

    void formatAndPrintStyles(qint32 currentStyle, qint32 defaultStyle,
                              const QVector<QString>* styleNames, ushort bitCount, short direction)
    {
        const qint32 originalCurrentStyle = currentStyle;
        const qint32 styleDifference = originalCurrentStyle ^ defaultStyle;
        const short bitIncrement = direction == -1 ? -1 : 1;
        int currentBitIndex = direction == -1 ? int(bitCount) - 1 : 0;
        for (ushort processedBits = 0; processedBits < bitCount; ++processedBits, currentBitIndex += bitIncrement) {
            const quint32 bit = quint32(1) << currentBitIndex;
            if ((quint32(styleDifference) & bit) == 0) continue;
            if (!outputBuffer.isEmpty()) outputBuffer += QStringLiteral(" | ");
            if ((quint32(originalCurrentStyle) & bit) == 0) outputBuffer += QStringLiteral("NOT ");
            if (styleNames && currentBitIndex < styleNames->size() && !styleNames->at(currentBitIndex).isNull())
                outputBuffer += styleNames->at(currentBitIndex);
            else
                outputBuffer += QStringLiteral("0x%1").arg(bit, 0, 16).toUpper();
        }
    }

    QString getControlName(quint16 controlType, qint16 controlStyle, quint32& defaultStyle, quint16& textFormatFlag)
    {
        defaultStyle = 0;
        textFormatFlag = 1;
        QString controlName;
        if (controlType != 0 && controlType < 0x42u && controlType < par_wc.size() && !par_wc[controlType].isNull())
            controlName = par_wc[controlType];
        else
            controlName = QStringLiteral("((PSZ)0x%1L)").arg(controlType, 8, 16, QChar(u'0')).toUpper();

        switch (controlType) {
        case 0: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF: case 0x10:
        case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
        case 0x1F: case 0x21: case 0x22: case 0x23: case 0x24:
            return QString();
        case 1: defaultStyle = 335544448u; return QStringLiteral("DIALOG");
        case 2: defaultStyle = 0x80100001u; return QStringLiteral("COMBOBOX");
        case 3: {
            const int subType = controlStyle & 0xF;
            defaultStyle = quint32(subType) | 0x80000000u;
            if (subType != 0) {
                switch (subType) {
                case 1: defaultStyle |= 0x20000; return QStringLiteral("CHECKBOX");
                case 2: defaultStyle |= 0x20000; return QStringLiteral("AUTOCHECKBOX");
                case 3: return QStringLiteral("RADIOBUTTON");
                case 4: return QStringLiteral("AUTORADIOBUTTON");
                default: defaultStyle = 0; return QString();
                }
            }
            if ((controlStyle & 0x400) != 0) { defaultStyle |= 0x400; return QStringLiteral("DEFPUSHBUTTON"); }
            return QStringLiteral("PUSHBUTTON");
        }
        case 4: return QString();
        case 5: {
            const quint32 subType = quint32(controlStyle & 0x3F) | 0x80000000u;
            defaultStyle = subType;
            switch (subType & 0x3F) {
            case 1:
                if ((controlStyle & 0x100) != 0) { defaultStyle |= 0x100; return QStringLiteral("CTEXT"); }
                if ((controlStyle & 0x200) != 0) { defaultStyle |= 0x200; return QStringLiteral("RTEXT"); }
                return QStringLiteral("LTEXT");
            case 2: defaultStyle |= 0x10000; return QStringLiteral("GROUPBOX");
            case 3: textFormatFlag = 1; return QStringLiteral("ICON");
            case 4: textFormatFlag = 1; defaultStyle = 0; return QString();
            default: defaultStyle = 0; return QString();
            }
        }
        case 6: defaultStyle = 0x80100004u; return QStringLiteral("ENTRYFIELD");
        case 7: defaultStyle = 0x80100000u; textFormatFlag = 0; return QStringLiteral("LISTBOX");
        case 8: case 9: return QString();
        case 0xA: defaultStyle = 0x80100002u; return QStringLiteral("MLE");
        case 0x20: defaultStyle = 0x80100000u; textFormatFlag = 0; return QStringLiteral("SPINBUTTON");
        case 0x25: defaultStyle = 0x80100000u; textFormatFlag = 0; return QStringLiteral("CONTAINER");
        case 0x26: defaultStyle = 0x80100000u; textFormatFlag = 0; return QStringLiteral("SLIDER");
        case 0x27: defaultStyle = 0x80100000u; textFormatFlag = 0; return QStringLiteral("VALUESET");
        case 0x28: defaultStyle = 0x80100000u; textFormatFlag = 0; return QStringLiteral("NOTEBOOK");
        default: return QString();
        }
    }

    void processBlock(int& currentResPtr, int resourceBaseOffset, ushort indentationLevel)
    {
        const int currentResourcePtr = currentResPtr;
        stream << spaces(indentationLevel);

        quint32 controlDefaultStyle = 0;
        quint16 controlTextType = 1;
        QString controlName;

        if (readUInt16(buffer, currentResourcePtr + 4) != 0) {
            // Custom control
            controlName.clear();
            controlDefaultStyle = 0;
            controlTextType = 1;
            stream << QStringLiteral("CONTROL").leftJustified(16, QChar(u' '));
        } else {
            // Standard control
            const quint16 controlType = readUInt16(buffer, currentResourcePtr + 6);
            const qint16 controlStyle = qint16(readUInt16(buffer, currentResourcePtr + 12));
            controlName = getControlName(controlType, controlStyle, controlDefaultStyle, controlTextType);
            if (controlTextType == 0 && readUInt16(buffer, currentResourcePtr + 8) != 0) {
                controlName.clear();
                controlDefaultStyle = 0;
                controlTextType = 1;
            }
            const QString keyword = controlName.isEmpty() ? QStringLiteral("CONTROL") : controlName;
            stream << keyword.leftJustified(controlType == 1 ? 8 : 16, QChar(u' '));
        }

        // Handle control text
        if (controlTextType == 1 && readUInt16(buffer, currentResourcePtr + 8) != 0) {
            const int p = resourceBaseOffset + readUInt16(buffer, currentResourcePtr + 10);
            if (p >= 0 && p < buffer.size() && quint8(buffer.at(p)) == 0xFF)
                controlTextType = 2;
        }

        if (controlTextType == 1) {
            stream << '"';
            const int textLength = readUInt16(buffer, currentResourcePtr + 8);
            const int textOffset = resourceBaseOffset + readUInt16(buffer, currentResourcePtr + 10);
            if (textOffset < 0 || textOffset + textLength > buffer.size())
                throw std::out_of_range("RT_DIALOG text out of range");
            for (int i = 0; i < textLength; ++i) {
                const uchar currentChar = uchar(buffer.at(textOffset + i));
                switch (currentChar) {
                case 0:
                    nullsDetected = true;
                    Q_FALLTHROUGH();
                case 7: case 8: case 9: case 0xA: case 0xB: case 0xC: case 0xD:
                    stream << QStringLiteral("\\x%1").arg(currentChar, 2, 16, QChar(u'0')).toUpper();
                    break;
                case 1: case 2: case 3: case 4: case 5: case 6:
                    stream << QChar(currentChar);
                    break;
                default:
                    if (currentChar == '"') stream << "\"\"";
                    else if (currentChar == '\\') stream << "\\\\";
                    else stream << QChar::fromLatin1(char(currentChar));
                    break;
                }
            }
            stream << "\", ";
        } else if (controlTextType == 2) {
            const int p = resourceBaseOffset + readUInt16(buffer, currentResourcePtr + 10) + 1;
            stream << readUInt16(buffer, p) << ", ";
        }

        // Format styles
        outputBuffer.clear();
        const quint16 highWordStyle = readUInt16(buffer, currentResourcePtr + 14);
        quint32 defaultStyle = (controlDefaultStyle >> 16) & 0xFFFF;
        if (highWordStyle != defaultStyle)
            formatAndPrintStyles(highWordStyle, qint32(defaultStyle), &par_ws, 0x10, -1);

        const quint16 lowWordStyle = readUInt16(buffer, currentResourcePtr + 12);
        defaultStyle = controlDefaultStyle & 0xFFFF;
        if (lowWordStyle != defaultStyle) {
            const quint16 controlType = readUInt16(buffer, currentResourcePtr + 6);
            switch (controlType) {
            case 0: case 4: case 9: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF:
            case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
            case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
            case 0x1E: case 0x1F: case 0x21: case 0x22: case 0x23: case 0x24:
                formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), nullptr, 0x10, 1); break;
            case 1:
                formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_fs, 0x10, 1); break;
            case 2: {
                const quint32 indexed = (defaultStyle ^ lowWordStyle) >> 12;
                formatAndPrintStyles(lowWordStyle & 0xFFF, qint32(defaultStyle & 0xFFF), &par_cbs, 0x10, 1);
                formatAndPrintIndexedStyle(indexed, par_es2, 4); break;
            }
            case 3: {
                const quint32 indexed = quint32(quint8(defaultStyle) ^ quint8(lowWordStyle)) & 0xF;
                formatAndPrintStyles(lowWordStyle & 0xFFF0, qint32(defaultStyle & 0xFFF0), &par_bs1, 0x10, 1);
                formatAndPrintIndexedStyle(indexed, par_bs2, 9); break;
            }
            case 5: {
                const quint32 indexed = quint32(quint8(defaultStyle) ^ quint8(lowWordStyle)) & 0x3F;
                formatAndPrintStyles(lowWordStyle & 0xFFC0, qint32(defaultStyle & 0xFFC0), &par_dt, 0x10, 1);
                formatAndPrintIndexedStyle(indexed, par_ss, 0xC); break;
            }
            case 6: {
                const quint32 indexed = (defaultStyle ^ lowWordStyle) >> 12;
                formatAndPrintStyles(lowWordStyle & 0xFFF, qint32(defaultStyle & 0xFFF), &par_es1, 0x10, 1);
                formatAndPrintIndexedStyle(indexed, par_es2, 4); break;
            }
            case 7: formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_ls, 0x10, 1); break;
            case 8: formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_sbs, 0x10, 1); break;
            case 0xA: formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_mls, 0x10, 1); break;
            case 0x20: formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_spbs, 0x10, 1); break;
            case 0x25: formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_ccs, 0x10, 1); break;
            case 0x26: {
                const QVector<QString>& sliderStyles = (lowWordStyle & 1) != 0 ? par_sls1 : par_sls2;
                formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &sliderStyles, 0x10, 1); break;
            }
            case 0x27: formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_vs, 0x10, 1); break;
            case 0x28: formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_bks, 0x10, 1); break;
            default:
                if (controlType == 65)
                    formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), &par_css, 0x10, 1);
                else
                    formatAndPrintStyles(lowWordStyle, qint32(defaultStyle), nullptr, 0x10, 1);
                break;
            }
        }

        // Write control coordinates and ID
        stream << readUInt16(buffer, currentResourcePtr + 24) << ", "
               << qint16(readUInt16(buffer, currentResourcePtr + 16)) << ", "
               << qint16(readUInt16(buffer, currentResourcePtr + 18)) << ", "
               << qint16(readUInt16(buffer, currentResourcePtr + 20)) << ", "
               << qint16(readUInt16(buffer, currentResourcePtr + 22));

        if (controlName.isEmpty()) {
            QString name;
            if (readUInt16(buffer, currentResourcePtr + 4) != 0) {
                const int length = readUInt16(buffer, currentResourcePtr + 4);
                const int offset = readUInt16(buffer, currentResourcePtr + 6) + resourceBaseOffset;
                if (offset < 0 || offset + length > buffer.size())
                    throw std::out_of_range("RT_DIALOG custom control name out of range");
                name = QStringLiteral("\"%1\"").arg(QString::fromLatin1(buffer.constData() + offset, length));
            } else {
                name = QStringLiteral("((PSZ)0x%1L)").arg(readUInt16(buffer, currentResourcePtr + 6), 8, 16, QChar(u'0')).toUpper();
            }
            stream << ", " << name;
        }

        if (!outputBuffer.isEmpty())
            stream << ", " << outputBuffer;

        // Handle font data for dialogs
        if (readUInt16(buffer, currentResourcePtr + 6) == 1) {
            const qint16 fontDataOffset = qint16(readUInt16(buffer, currentResourcePtr + 28));
            if (fontDataOffset != 0 && fontDataOffset != -1) {
                const qint32 fontStyleValue = qint32(readUInt32(buffer, resourceBaseOffset + readUInt16(buffer, currentResourcePtr + 28)));
                if (fontStyleValue != 0) {
                    if (outputBuffer.isEmpty()) stream << ", ";
                    outputBuffer.clear();
                    formatAndPrintStyles(fontStyleValue, 0, &par_fcf, 0x20, -1);
                    stream << ", " << outputBuffer;
                }
            }
            stream << "\n";
        } else {
            stream << "\n";
            // Handle control data
            const qint16 ctlDataOffset = qint16(readUInt16(buffer, currentResourcePtr + 28));
            if (ctlDataOffset != 0 && ctlDataOffset != -1) {
                const int tempBlockPtr = readUInt16(buffer, currentResourcePtr + 28) + resourceBaseOffset;
                const quint16 ctlDataSize = readUInt16(buffer, tempBlockPtr);
                if (ctlDataSize != 0) {
                    stream << spaces(indentationLevel + 16) << "CTLDATA ";
                    writeFormatted2(tempBlockPtr, ctlDataSize);
                }
            }
        }

        // Handle child controls (presentation parameters)
        const qint16 childControlsOffset = qint16(readUInt16(buffer, currentResourcePtr + 26));
        if (childControlsOffset != 0 && childControlsOffset != -1) {
            const int childListStartPtr = readUInt16(buffer, currentResourcePtr + 26) + resourceBaseOffset;
            const quint16 childListSize = readUInt16(buffer, childListStartPtr);
            if (childListSize != 0) {
                int currentChildPtr = childListStartPtr + 2;

                // Some LX dialog resources insert a padding WORD between the
                // presentation-parameter list size and the first parameter
                // block.  Res2Dlg accepts these resources; starting at +2
                // interprets the padding as the low half of paramValue and the
                // following DWORD as an enormous count.  Select +4 only when
                // +2 is structurally impossible and +4 forms a complete block.
                const auto parameterBlockFits = [this](int ptr) -> bool {
                    if (ptr < 0 || ptr + 8 > buffer.size())
                        return false;
                    const quint32 value = readUInt32(buffer, ptr);
                    const quint32 sizeAndFlags = readUInt32(buffer, ptr + 4);
                    if (value == 15) {
                        const int stringStart = ptr + 8;
                        return stringStart < buffer.size() && buffer.indexOf('\0', stringStart) >= 0;
                    }
                    const quint32 count = sizeAndFlags >> 2;
                    const quint32 flags = sizeAndFlags & 3u;
                    const quint64 tail = flags == 0 ? 0u : (flags == 1 ? 1u : (flags == 2 ? 2u : 4u));
                    return quint64(ptr) + 8u + quint64(count) * 4u + tail <= quint64(buffer.size());
                };

                if (!parameterBlockFits(currentChildPtr) &&
                    readUInt16(buffer, currentChildPtr) == 0 &&
                    parameterBlockFits(currentChildPtr + 2))
                {
                    currentChildPtr += 2;
                }

                // In these resources the list size excludes its own WORD.
                const int childListEndPtr = qMin(buffer.size(), childListStartPtr + 2 + int(childListSize));
                while (currentChildPtr < childListEndPtr) {
                    const int incrementValue = readUInt16(buffer, currentResourcePtr + 6) == 1 ? 8 : 16;
                    formatControlParameters(currentChildPtr, ushort(incrementValue + indentationLevel));
                    const quint32 blockSize = readUInt32(buffer, currentChildPtr + 4) + 8u;
                    if (blockSize == 0 || quint64(currentChildPtr) + blockSize > quint64(buffer.size()))
                        throw std::out_of_range("RT_DIALOG presentation parameter block out of range");
                    currentChildPtr += int(blockSize);
                }
            }
        }

        // Handle child controls (nested controls)
        const quint16 childCount = readUInt16(buffer, currentResourcePtr + 2);
        currentResPtr += 30;
        if (childCount != 0) {
            stream << spaces(indentationLevel) << "BEGIN\n";
            for (quint16 i = 1; i < childCount; ++i)
                processBlock(currentResPtr, resourceBaseOffset, ushort(indentationLevel + 4));
            stream << spaces(indentationLevel) << "END\n";
        }
    }

    void formatMemoryFlags(int memoryFlags)
    {
        outputBuffer2.clear();
        if ((memoryFlags & 0x40) != 0 || (memoryFlags & 0x1010) == 0) {
            const QString loadOption = (memoryFlags & 0x40) != 0 ? QStringLiteral("PRELOAD") : QStringLiteral("LOADONCALL");
            const QString moveableOption = (memoryFlags & 0x10) != 0 ? QStringLiteral(" MOVEABLE") : QString();
            const QString discardableOption = (memoryFlags & 0x1000) != 0 ? QStringLiteral(" DISCARDABLE") : QString();
            const QString fixedOption = (memoryFlags & 0x1010) != 0 ? QString() : QStringLiteral(" FIXED");
            outputBuffer2 = QStringLiteral(" %1%2%3%4").arg(loadOption, moveableOption, discardableOption, fixedOption);
        }
    }
};

} // namespace

QString OS2_RT_DIALOG::Get(const QByteArray& data)
{
    try {
        Parser parser(data);
        return parser.parse();
    } catch (const std::exception& error) {
        return QStringLiteral("Error decoding OS/2 dialog: %1").arg(QString::fromLocal8Bit(error.what()));
    }
}

} // namespace resources
} // namespace peare
