
bool initvideograb(const TCHAR *filename);
void uninitvideograb(void);
bool getvideograb(long **buffer, int *width, int *height);
void pausevideograb(int pause);
uae_s64 getsetpositionvideograb(uae_s64 framepos);
uae_s64 getdurationvideograb(void);
bool isvideograb(void);
bool getpausevideograb(void);
void setvolumevideograb(int volume);
void setmastervolumevideograb(int volume, bool mute);
void setsoundoutputvideograb(bool enabled);
void setchflagsvideograb(int chflags, bool mute);
/* Hold the picture on the frame that was on screen when the freeze was set,
 * while the graph keeps running for sound. This is the Sneak Prevue frame
 * grab: the Amiga keeps the background, the player goes elsewhere for the
 * audio bed. */
void setfreezevideograb(int freeze);
bool getfreezevideograb(void);
void isvideograb_status(void);
