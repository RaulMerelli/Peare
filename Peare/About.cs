using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using PeareModule;

namespace Peare
{
    public partial class About : Form
    {
        public About()
        {
            InitializeComponent();
        }

        private void linkLabel1_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            Process.Start("https://github.com/RaulMerelli/Peare");
        }

        private void About_Load(object sender, EventArgs e)
        {
            try
            {
                // load the image from the our executable using our function!
                pictureBox1.Image = RT_ICON.Get(
                    ModuleResources.OpenResource(System.Reflection.Assembly.GetEntryAssembly().Location, 
                    "RT_ICON", 
                    "2", 
                    out _, 
                    out _)).Bitmap;
            }
            catch 
            {

            }
        }
    }
}
