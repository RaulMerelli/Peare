namespace Peare
{
    partial class MainForm
    {
        /// <summary>
        /// Variabile di progettazione necessaria.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Pulire le risorse in uso.
        /// </summary>
        /// <param name="disposing">ha valore true se le risorse gestite devono essere eliminate, false in caso contrario.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Codice generato da Progettazione Windows Form

        /// <summary>
        /// Metodo necessario per il supporto della finestra di progettazione. Non modificare
        /// il contenuto del metodo con l'editor di codice.
        /// </summary>
        private void InitializeComponent()
        {
            this.components = new System.ComponentModel.Container();
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(MainForm));
            this.treeView1 = new System.Windows.Forms.TreeView();
            this.flowLayoutPanel1 = new System.Windows.Forms.FlowLayoutPanel();
            this.mainMenu1 = new System.Windows.Forms.MainMenu(this.components);
            this.mnu_File = new System.Windows.Forms.MenuItem();
            this.mnu_Open = new System.Windows.Forms.MenuItem();
            this.menuItem1 = new System.Windows.Forms.MenuItem();
            this.mnu_Exit = new System.Windows.Forms.MenuItem();
            this.mnu_View = new System.Windows.Forms.MenuItem();
            this.mnu_ExpandTreeview = new System.Windows.Forms.MenuItem();
            this.mnu_CollapseTreeview = new System.Windows.Forms.MenuItem();
            this.menuItem2 = new System.Windows.Forms.MenuItem();
            this.mnu_OpenIssue = new System.Windows.Forms.MenuItem();
            this.menuItem4 = new System.Windows.Forms.MenuItem();
            this.mnu_About = new System.Windows.Forms.MenuItem();
            this.lbMessage = new System.Windows.Forms.Label();
            this.imageList1 = new System.Windows.Forms.ImageList(this.components);
            this.SuspendLayout();
            // 
            // treeView1
            // 
            this.treeView1.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom) 
            | System.Windows.Forms.AnchorStyles.Left)));
            this.treeView1.ImageIndex = 0;
            this.treeView1.ImageList = this.imageList1;
            this.treeView1.ItemHeight = 18;
            this.treeView1.Location = new System.Drawing.Point(0, 1);
            this.treeView1.Name = "treeView1";
            this.treeView1.SelectedImageIndex = 0;
            this.treeView1.Size = new System.Drawing.Size(341, 497);
            this.treeView1.TabIndex = 6;
            this.treeView1.BeforeCollapse += new System.Windows.Forms.TreeViewCancelEventHandler(this.treeView1_BeforeExpandCollapse);
            this.treeView1.BeforeExpand += new System.Windows.Forms.TreeViewCancelEventHandler(this.treeView1_BeforeExpandCollapse);
            this.treeView1.AfterSelect += new System.Windows.Forms.TreeViewEventHandler(this.treeView1_AfterSelect);
            // 
            // flowLayoutPanel1
            // 
            this.flowLayoutPanel1.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom) 
            | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.flowLayoutPanel1.AutoScroll = true;
            this.flowLayoutPanel1.Location = new System.Drawing.Point(348, 27);
            this.flowLayoutPanel1.Name = "flowLayoutPanel1";
            this.flowLayoutPanel1.Size = new System.Drawing.Size(707, 459);
            this.flowLayoutPanel1.TabIndex = 5;
            // 
            // mainMenu1
            // 
            this.mainMenu1.MenuItems.AddRange(new System.Windows.Forms.MenuItem[] {
            this.mnu_File,
            this.mnu_View,
            this.menuItem2});
            // 
            // mnu_File
            // 
            this.mnu_File.Index = 0;
            this.mnu_File.MenuItems.AddRange(new System.Windows.Forms.MenuItem[] {
            this.mnu_Open,
            this.menuItem1,
            this.mnu_Exit});
            this.mnu_File.Text = "File";
            // 
            // mnu_Open
            // 
            this.mnu_Open.Index = 0;
            this.mnu_Open.Text = "Open";
            this.mnu_Open.Click += new System.EventHandler(this.menuClick);
            // 
            // menuItem1
            // 
            this.menuItem1.Index = 1;
            this.menuItem1.Text = "-";
            // 
            // mnu_Exit
            // 
            this.mnu_Exit.Index = 2;
            this.mnu_Exit.Text = "Exit";
            this.mnu_Exit.Click += new System.EventHandler(this.menuClick);
            // 
            // mnu_View
            // 
            this.mnu_View.Index = 1;
            this.mnu_View.MenuItems.AddRange(new System.Windows.Forms.MenuItem[] {
            this.mnu_ExpandTreeview,
            this.mnu_CollapseTreeview});
            this.mnu_View.Text = "View";
            // 
            // mnu_ExpandTreeview
            // 
            this.mnu_ExpandTreeview.Index = 0;
            this.mnu_ExpandTreeview.Text = "Expand treeview";
            this.mnu_ExpandTreeview.Click += new System.EventHandler(this.menuClick);
            // 
            // mnu_CollapseTreeview
            // 
            this.mnu_CollapseTreeview.Index = 1;
            this.mnu_CollapseTreeview.Text = "Collapse treeview";
            this.mnu_CollapseTreeview.Click += new System.EventHandler(this.menuClick);
            // 
            // menuItem2
            // 
            this.menuItem2.Index = 2;
            this.menuItem2.MenuItems.AddRange(new System.Windows.Forms.MenuItem[] {
            this.mnu_OpenIssue,
            this.menuItem4,
            this.mnu_About});
            this.menuItem2.Text = "Help";
            // 
            // mnu_OpenIssue
            // 
            this.mnu_OpenIssue.Index = 0;
            this.mnu_OpenIssue.Text = "Open an issue";
            this.mnu_OpenIssue.Click += new System.EventHandler(this.menuClick);
            // 
            // menuItem4
            // 
            this.menuItem4.Index = 1;
            this.menuItem4.Text = "-";
            // 
            // mnu_About
            // 
            this.mnu_About.Index = 2;
            this.mnu_About.Text = "About";
            this.mnu_About.Click += new System.EventHandler(this.menuClick);
            // 
            // lbMessage
            // 
            this.lbMessage.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.lbMessage.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.lbMessage.Location = new System.Drawing.Point(347, 1);
            this.lbMessage.Name = "lbMessage";
            this.lbMessage.Size = new System.Drawing.Size(708, 23);
            this.lbMessage.TabIndex = 7;
            this.lbMessage.TextAlign = System.Drawing.ContentAlignment.MiddleLeft;
            // 
            // imageList1
            // 
            this.imageList1.ColorDepth = System.Windows.Forms.ColorDepth.Depth32Bit;
            this.imageList1.ImageSize = new System.Drawing.Size(16, 16);
            this.imageList1.TransparentColor = System.Drawing.Color.Transparent;
            // 
            // MainForm
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(1067, 498);
            this.Controls.Add(this.lbMessage);
            this.Controls.Add(this.treeView1);
            this.Controls.Add(this.flowLayoutPanel1);
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.Menu = this.mainMenu1;
            this.Name = "MainForm";
            this.Text = "Peare";
            this.Load += new System.EventHandler(this.MainForm_Load);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.TreeView treeView1;
        private System.Windows.Forms.FlowLayoutPanel flowLayoutPanel1;
        private System.Windows.Forms.MainMenu mainMenu1;
        private System.Windows.Forms.MenuItem mnu_File;
        private System.Windows.Forms.MenuItem mnu_Open;
        private System.Windows.Forms.MenuItem mnu_View;
        private System.Windows.Forms.MenuItem mnu_ExpandTreeview;
        private System.Windows.Forms.MenuItem mnu_CollapseTreeview;
        private System.Windows.Forms.MenuItem menuItem1;
        private System.Windows.Forms.MenuItem mnu_Exit;
        private System.Windows.Forms.MenuItem menuItem2;
        private System.Windows.Forms.MenuItem mnu_OpenIssue;
        private System.Windows.Forms.MenuItem menuItem4;
        private System.Windows.Forms.MenuItem mnu_About;
        private System.Windows.Forms.Label lbMessage;
        private System.Windows.Forms.ImageList imageList1;
    }
}

