sooperlooper-lv2-plugin
=======================

A Lv2 version of the old LADSPA SooperLooper plugin by Jesse Chappell [http://essej.net/sooperlooper/oldplugin.html]

This is a work in progress. 

I started porting the old LADSPA SooperLooper plugin, but it uses the LV2 Control ports to control the Looper instead of MIDI events.

Currently it supports:

 * mono in/out
 * record/overdub
 * play/pause
 * undo
 * redo

Next features planned include:

 * stereo in/out
 * tempo/bpm sync
 * pitch change
