using System;
using System.Collections.Generic;
using System.Drawing;

namespace PeareModule
{
    internal sealed class FontVectorSegment
    {
        internal int X1;
        internal int Y1;
        internal int X2;
        internal int Y2;
    }

    internal sealed class FontOutlineContour
    {
        internal List<PointF> Points { get; set; }
    }

    public sealed class FontGlyph
    {
        public int CharacterCode { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
        public int AdvanceX { get; set; }
        public int OffsetX { get; set; }
        public int OffsetY { get; set; }
        public Bitmap Bitmap { get; set; }

        internal List<FontVectorSegment> VectorSegments { get; set; }
        internal List<FontOutlineContour> OutlineContours { get; set; }

        public bool HasFilledOutline
        {
            get { return OutlineContours != null && OutlineContours.Count > 0; }
        }

        public bool HasVectorOutline
        {
            get
            {
                return HasFilledOutline ||
                    (VectorSegments != null && VectorSegments.Count > 0);
            }
        }
    }

    public sealed class DecodedFont : IDisposable
    {
        public DecodedFont()
        {
            Glyphs = new List<FontGlyph>();
        }

        public string FaceName { get; set; }
        public string FormatName { get; set; }
        public int FirstCharacter { get; set; }
        public int LastCharacter { get; set; }
        public int DefaultCharacter { get; set; }
        public int BreakCharacter { get; set; }
        public int PixelHeight { get; set; }
        public int Ascent { get; set; }
        public int Descent { get; set; }
        public int LineHeight { get; set; }
        public int CodePage { get; set; }
        public int CharacterSet { get; set; }
        public bool IsVector { get; set; }
        public int DeclaredGlyphCount { get; set; }
        public string PreviewMessage { get; set; }
        public List<FontGlyph> Glyphs { get; private set; }

        public void Dispose()
        {
            for (int i = 0; i < Glyphs.Count; i++)
            {
                FontGlyph glyph = Glyphs[i];
                if (glyph != null && glyph.Bitmap != null)
                {
                    glyph.Bitmap.Dispose();
                    glyph.Bitmap = null;
                }
                if (glyph != null && glyph.VectorSegments != null)
                    glyph.VectorSegments.Clear();
                if (glyph != null && glyph.OutlineContours != null)
                {
                    for (int contourIndex = 0; contourIndex < glyph.OutlineContours.Count; contourIndex++)
                    {
                        FontOutlineContour contour = glyph.OutlineContours[contourIndex];
                        if (contour != null && contour.Points != null)
                            contour.Points.Clear();
                    }
                    glyph.OutlineContours.Clear();
                }
            }
            Glyphs.Clear();
        }

        public FontGlyph FindGlyph(int characterCode)
        {
            for (int i = 0; i < Glyphs.Count; i++)
            {
                FontGlyph glyph = Glyphs[i];
                if (glyph != null && glyph.CharacterCode == characterCode)
                    return glyph;
            }

            return null;
        }
    }
}
