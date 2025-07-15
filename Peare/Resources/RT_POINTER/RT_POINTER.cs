using System.Collections.Generic;
using System.Drawing;

namespace Peare
{
    public class RT_POINTER
    {
        public static List<Bitmap> Get(byte[] resData)
        {
            // RT_BITMAP is already fully able to handle everything a RT_POINTER may have.
            return RT_BITMAP.Get(resData);
        }
    }
}
