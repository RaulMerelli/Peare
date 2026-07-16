using System;
using System.Collections.Generic;
using System.Drawing;

namespace PeareModule
{
    /// <summary>
    /// Decodes the FOCA outline command stream stored in OS/2 GPI font
    /// resources. The original conic curves are sampled into scalable contour
    /// points; RT_FONT later rasterizes those contours with an alternate
    /// (even/odd) fill rule.
    /// </summary>
    internal static class OS2OutlineDecoder
    {
        private const byte CommandLineContinue = 0x81;
        private const byte CommandConicContinue = 0xA4;
        private const byte CommandLineBegin = 0xC1;
        private const byte CommandConicBegin = 0xE4;
        private const byte CommandEndGlyph = 0xFF;

        private const int DefaultCurveSteps = 28;
        private const int MaximumGlyphCommands = 4096;
        private const int MaximumContourPoints = 65536;

        internal static List<FontOutlineContour> DecodeGlyph(byte[] data, int start, int length, double unitScale, int ascenderUnits)
        {
            List<FontOutlineContour> contours = new List<FontOutlineContour>();
            if (data == null || length <= 0 || start < 0 || start >= data.Length || (long)start + length > data.Length)
                return contours;

            List<PointF> current = null;
            PointF currentRaw = PointF.Empty;
            bool hasCurrentPoint = false;
            int position = start;
            int end = start + length;
            int commandCount = 0;

            while (position + 2 <= end)
            {
                if (++commandCount > MaximumGlyphCommands)
                    throw new InvalidOperationException("OS/2 outline glyph contains too many commands.");

                byte command = data[position++];
                int payloadLength = data[position++];
                if (position + payloadLength > end)
                    throw new InvalidOperationException("Truncated OS/2 outline command stream.");

                int payloadStart = position;
                int payloadEnd = position + payloadLength;
                position = payloadEnd;

                if (command == CommandEndGlyph)
                {
                    CloseContour(contours, ref current);
                    break;
                }

                if (command == CommandLineBegin || command == CommandConicBegin)
                {
                    CloseContour(contours, ref current);
                    hasCurrentPoint = false;
                }

                if (command == CommandLineBegin || command == CommandLineContinue)
                {
                    if ((payloadLength & 3) != 0)
                        throw new InvalidOperationException("Invalid OS/2 outline point list.");

                    int pointPosition = payloadStart;
                    if (current == null)
                        current = new List<PointF>();

                    while (pointPosition + 4 <= payloadEnd)
                    {
                        PointF rawPoint = ReadPoint(data, pointPosition);
                        pointPosition += 4;
                        currentRaw = rawPoint;
                        hasCurrentPoint = true;
                        AddPoint(current, TransformPoint(rawPoint, unitScale, ascenderUnits));
                    }
                    continue;
                }

                if (command == CommandConicBegin || command == CommandConicContinue)
                {
                    int curveDataStart = payloadStart;
                    if (command == CommandConicBegin)
                    {
                        if (payloadLength < 4)
                            throw new InvalidOperationException("OS/2 conic contour has no initial point.");

                        currentRaw = ReadPoint(data, payloadStart);
                        hasCurrentPoint = true;
                        current = new List<PointF>();
                        AddPoint(current, TransformPoint(currentRaw, unitScale, ascenderUnits));
                        curveDataStart += 4;
                    }

                    if (!hasCurrentPoint || current == null)
                        throw new InvalidOperationException("OS/2 conic continuation has no initial point.");

                    int remaining = payloadEnd - curveDataStart;
                    if (remaining < 0 || remaining % 12 != 0)
                        throw new InvalidOperationException("Invalid OS/2 conic command length.");

                    int segmentCount = remaining / 12;
                    int pointsStart = curveDataStart;
                    int weightsStart = pointsStart + segmentCount * 8;

                    for (int segment = 0; segment < segmentCount; segment++)
                    {
                        PointF control = ReadPoint(data, pointsStart + segment * 8);
                        PointF target = ReadPoint(data, pointsStart + segment * 8 + 4);
                        int fixedWeight = BitConverter.ToInt32(data, weightsStart + segment * 4);
                        double weight = fixedWeight / 65536.0;
                        if (double.IsNaN(weight) || double.IsInfinity(weight) || weight == 0.0)
                            weight = 1.0;

                        AddConic(current, currentRaw, control, target, weight,
                            DefaultCurveSteps, unitScale, ascenderUnits);
                        currentRaw = target;
                    }
                    continue;
                }

                throw new InvalidOperationException(
                    "Unsupported OS/2 outline command 0x" + command.ToString("X2") + ".");
            }

            CloseContour(contours, ref current);
            return contours;
        }

        private static PointF ReadPoint(byte[] data, int offset)
        {
            return new PointF(BitConverter.ToInt16(data, offset), BitConverter.ToInt16(data, offset + 2));
        }

        private static PointF TransformPoint(PointF point, double unitScale, int ascenderUnits)
        {
            return new PointF((float)(point.X * unitScale), (float)((ascenderUnits - point.Y) * unitScale));
        }

        private static void AddConic(List<PointF> contour, PointF start, PointF control, PointF target, double weight, int steps, double unitScale, int ascenderUnits)
        {
            for (int index = 1; index <= steps; index++)
            {
                double t = (double)index / steps;
                double mt = 1.0 - t;
                double a = mt * mt;
                double b = 2.0 * weight * t * mt;
                double c = t * t;
                double denominator = a + b + c;

                PointF rawPoint;
                if (denominator == 0.0)
                {
                    rawPoint = target;
                }
                else
                {
                    rawPoint = new PointF(
                        (float)((a * start.X + b * control.X + c * target.X) / denominator),
                        (float)((a * start.Y + b * control.Y + c * target.Y) / denominator));
                }
                AddPoint(contour, TransformPoint(rawPoint, unitScale, ascenderUnits));
            }
        }

        private static void AddPoint(List<PointF> contour, PointF point)
        {
            if (contour.Count >= MaximumContourPoints)
                throw new InvalidOperationException("OS/2 outline contour contains too many points.");

            if (contour.Count > 0)
            {
                PointF previous = contour[contour.Count - 1];
                if (Math.Abs(previous.X - point.X) < 0.0001f &&
                    Math.Abs(previous.Y - point.Y) < 0.0001f)
                    return;
            }
            contour.Add(point);
        }

        private static void CloseContour(List<FontOutlineContour> contours, ref List<PointF> current)
        {
            if (current == null)
                return;

            if (current.Count >= 3)
            {
                PointF first = current[0];
                PointF last = current[current.Count - 1];
                if (Math.Abs(first.X - last.X) > 0.0001f ||
                    Math.Abs(first.Y - last.Y) > 0.0001f)
                    current.Add(first);

                contours.Add(new FontOutlineContour { Points = current });
            }
            current = null;
        }
    }
}
