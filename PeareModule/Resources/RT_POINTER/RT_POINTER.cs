﻿using System.Collections.Generic;
using System.Drawing;

namespace PeareModule
{
    public class RT_POINTER
    {
        public static List<Img> Get(byte[] resData)
        {
            // RT_BITMAP is already fully able to handle everything a RT_POINTER may have.
            return RT_BITMAP.Get(resData);
        }
    }
}
