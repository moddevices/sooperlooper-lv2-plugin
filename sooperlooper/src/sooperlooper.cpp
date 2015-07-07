/* SooperLooper.c  :  
   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
// ------------------------------------------------------------------------


   This LADSPA plugin provides an Echoplex like realtime sampling
   looper.  Plus some extra features.

   There is a fixed maximum sample memory.  The featureset is derived
   from the Gibson-Oberheim Echoplex Digital Pro.


*/
   
/*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <values.h>
#include <string.h>
#include <math.h>
#include <lv2.h>
#include <string.h>

#define PLUGIN_URI "http://moddevices.com/plugins/sooperlooper"

/**********************************************************************************************************************************************************/

enum {IN_0, OUT_0, PLAY_PAUSE, RECORD, RESET, UNDO, REDO, PLUGIN_PORT_COUNT};

#define PLUGIN_AUDIO_PORT_COUNT     2
#define PLUGIN_CONTROL_PORT_COUNT   PLUGIN_PORT_COUNT - PLUGIN_AUDIO_PORT_COUNT

/*****************************************************************************/

typedef float LADSPA_Data;

/*****************************************************************************/

#ifdef DEBUG
#define DBG(x) x
#else
#define DBG(x)
#endif

#define VERSION "0.93"

/* The maximum sample memory  (in seconds). */

#ifndef SAMPLE_MEMORY
#define SAMPLE_MEMORY 400.0
#endif

#define XFADE_SAMPLES 512

// settle time for tap trigger (trigger if two changes
// happen within at least X samples)
//#define TRIG_SETTLE  4410
#define TRIG_SETTLE  2205  

/*****************************************************************************/

#define STATE_OFF        0
#define STATE_TRIG_START 1
#define STATE_RECORD     2
#define STATE_TRIG_STOP  3
#define STATE_PLAY       4
#define STATE_OVERDUB    5
#define STATE_MULTIPLY   6
#define STATE_INSERT     7
#define STATE_REPLACE    8
#define STATE_DELAY      9
#define STATE_MUTE       10
#define STATE_SCRATCH    11
#define STATE_ONESHOT    12

#define LIMIT_BETWEEN_0_AND_1(x)          \
(((x) < 0) ? 0 : (((x) > 1) ? 1 : (x)))

#define LIMIT_BETWEEN_NEG1_AND_1(x)          \
(((x) < -1) ? -1 : (((x) > 1) ? 1 : (x)))

#define LIMIT_BETWEEN_0_AND_MAX_DELAY(x)  \
(((x) < 0) ? 0 : (((x) > MAX_DELAY) ? MAX_DELAY : (x)))

/*****************************************************************************/

// defines all a loop needs to know to cycle properly in memory
// one of these will prefix the actual loop data in our buffer memory
typedef struct _LoopChunk {

    /* pointers in buffer memory. */
    LADSPA_Data * pLoopStart;
    LADSPA_Data * pLoopStop;    
    //unsigned long lLoopStart;
    //unsigned long lLoopStop;    
    unsigned long lLoopLength;

    // adjustment needed in the case of multiply/insert
    unsigned long lStartAdj;
    unsigned long lEndAdj;
    unsigned long lInsPos; // used only by INSERT mode
    unsigned long lRemLen; // used only by INSERT mode
    
    // markers needed for frontfilling and backfilling
    unsigned long lMarkL;
    unsigned long lMarkH;    
    unsigned long lMarkEndL;
    unsigned long lMarkEndH;        

    int firsttime;
    int frontfill;
    int backfill;
    
    unsigned long lCycles;
    unsigned long lCycleLength;
    LADSPA_Data dOrigFeedback;
    
    // current position is double to support alternative rates easier
    double dCurrPos;

    // the loop where we should be frontfilled and backfilled from
    struct _LoopChunk* srcloop;
    
    struct _LoopChunk* next;
    struct _LoopChunk* prev;
    
} LoopChunk;

/* Instance data */
typedef struct {
    
    LADSPA_Data fSampleRate;

    /* the sample memory */
    //LADSPA_Data * pfSampleBuf;
    char * pSampleBuf;
    
    /* Buffer size, not necessarily a power of two. */
    unsigned long lBufferSize;

    /* the current state of the sampler */
    int state;

    int nextState;

    long lLastMultiCtrl;

    // initial location of params
    LADSPA_Data fQuantizeMode;
    LADSPA_Data fRoundMode;    
    LADSPA_Data fRedoTapMode;

    
    // used only when in DELAY mode
    int bHoldMode;

    
    unsigned long lTapTrigSamples;

    LADSPA_Data fLastOverTrig;    
    unsigned long lOverTrigSamples;    

    unsigned long lRampSamples;
    
    LADSPA_Data fCurrRate;
    LADSPA_Data fNextCurrRate;

    LADSPA_Data fLastScratchVal;
    unsigned long lScratchSamples;
    LADSPA_Data fCurrScratchRate;
    LADSPA_Data fLastRateSwitch;
    int bRateCtrlActive;
    
    LADSPA_Data fLastTapCtrl;
    int bPreTap;
    
    // linked list of loop chunks
    LoopChunk * headLoopChunk;
    LoopChunk * tailLoopChunk;    

    
    /* Ports:
       ------ */
    LADSPA_Data * pfWet;
    LADSPA_Data * pfDry;
    
    /* Feedback 0 for none, 1 for infinite */
    LADSPA_Data * pfFeedback;

    /* Trigger level for record and stop record */
    LADSPA_Data * pfTrigThresh;


    /* The rate of loop playback, if RateSwitch is on */
    LADSPA_Data * pfRate;

    /* The destination position in the loop to scratch to. 0 is the start */
    /*  and 1.0 is the end of the loop.  Only active if RateSwitch is on */
    LADSPA_Data * pfScratchPos;

    /* The multicontrol port.  Each value from (0-127) has a
     * meaning.  This is considered a momentary control, thus
     * ANY change to a value within the value range is only
     * noticed at the moment it changes from something different.
     *  If you want to do two identical values in a row, you must change
     * the value to something outside our range for a cycle before using
     * the real value again.
     */
    LADSPA_Data * pfMultiCtrl;

    /* This specifies which multiple of ten this plugin responds to
     * for the multi-control port.  For instance, if 0 is given we respond
     * to 0-9 on the multi control port, if 1 is given, 10-19.  This allows you
     * to separately control multiple looper instances with the same footpedal,
     * for instance.  Range is 0-12.
     */
    LADSPA_Data * pfMultiTens;
    
    /* changes on this control signal with more than TAP_THRESH_SAMP samples
     * between them (to handle settle time) is treated as a a TAP Delay trigger
     */
    LADSPA_Data *pfTapCtrl;

    /* non zero here toggle quantize and round mode
     *  WARNING: the plugin may set this value internally... cause I want
     *  it controllable (via mute mode)
     */
    LADSPA_Data *pfQuantMode;
    LADSPA_Data *pfRoundMode;    

    /* if non zero, the redo command is treated like a tap trigger */
    LADSPA_Data *pfRedoTapMode;
    
    /* Input audio port data location. */
    LADSPA_Data * pfInput;
    
    /* Output audio port data location. */
    LADSPA_Data * pfOutput;

    
    /* Control outputs */

    LADSPA_Data * pfStateOut;
    LADSPA_Data * pfLoopLength;
    LADSPA_Data * pfLoopPos;        
    LADSPA_Data * pfCycleLength;

    /* how many seconds of loop memory free and total */
    LADSPA_Data * pfSecsFree;
    LADSPA_Data * pfSecsTotal;    
    
} SooperLooper;


class SooperLooperPlugin
{
public:
    SooperLooperPlugin() {}
    ~SooperLooperPlugin() {}
    static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double samplerate, const char* bundle_path, const LV2_Feature* const* features);
    static void activate(LV2_Handle instance);
    static void deactivate(LV2_Handle instance);
    static void connect_port(LV2_Handle instance, uint32_t port, void *data);
    static void run(LV2_Handle instance, uint32_t n_samples);
    static void cleanup(LV2_Handle instance);
    static const void* extension_data(const char* uri);
    float *in_0;
    float *out_0;
    float *play_pause;
    float *record;
    float *reset;
    float *undo;
    float *redo;
    SooperLooper *pLS;
    int playing;
    int started;
    int recording; 
    int params_state[PLUGIN_CONTROL_PORT_COUNT];
};


// creates a new loop chunk and puts it on the head of the list
// returns the new chunk
static LoopChunk * pushNewLoopChunk(SooperLooper* pLS, unsigned long initLength)
{
   //LoopChunk * loop = malloc(sizeof(LoopChunk));
   LoopChunk * loop;   

   if (pLS->headLoopChunk) {
      // use the next spot in memory
      loop  = (LoopChunk *) pLS->headLoopChunk->pLoopStop;

      if ((char *)((char*)loop + sizeof(LoopChunk) + (initLength * sizeof(LADSPA_Data)))
	  >= (pLS->pSampleBuf + pLS->lBufferSize)) {
	 // out of memory, return NULL
	 //DBG(fprintf(stderr, "Error pushing new loop, out of loop memory\n");)
	 return NULL;
      }
      
      loop->prev = pLS->headLoopChunk;
      loop->next = NULL;
      
      loop->prev->next = loop;
      
      // the loop data actually starts directly following this struct
      loop->pLoopStart = (LADSPA_Data *) (loop + sizeof(LoopChunk));

      // the stop will be filled in later
      
      // we are the new head
      pLS->headLoopChunk = loop;
      
   }
   else {
      // first loop on the list!
      loop = (LoopChunk *) pLS->pSampleBuf;
      loop->next = loop->prev = NULL;
      pLS->headLoopChunk = pLS->tailLoopChunk = loop;
      loop->pLoopStart = (LADSPA_Data *) (loop + sizeof(LoopChunk));
   }
   

   //DBG(fprintf(stderr, "New head is %08x\n", (unsigned)loop);)

   
   return loop;
}

// pop the head off and free it
static void popHeadLoop(SooperLooper *pLS)
{
   LoopChunk *dead;
   dead = pLS->headLoopChunk;

   if (dead && dead->prev) {
      // leave the next where is is for redo
      //dead->prev->next = NULL;
      pLS->headLoopChunk = dead->prev;
      if (!pLS->headLoopChunk->prev) {
	 pLS->tailLoopChunk = pLS->headLoopChunk; 
      }
      //free(dead);
   }
   else {
      pLS->headLoopChunk = NULL;
      // pLS->tailLoopChunk is still valid to support redo
      // from nothing
   }
}

// clear all LoopChunks (undoAll , can still redo them back)
static void clearLoopChunks(SooperLooper *pLS)
{
   /*
   LoopChunk *prev, *tmp;
   
   prev = pLS->headLoopChunk;
   
   while (prev)
   {
      tmp = prev->prev;
      free(prev);
      prev = tmp;
   }
   */
   
   pLS->headLoopChunk = NULL;
}

void undoLoop(SooperLooper *pLS)
{
   LoopChunk *loop = pLS->headLoopChunk;
   LoopChunk *prevloop;
   
   prevloop = loop->prev;
   if (prevloop && prevloop == loop->srcloop) {
      // if the previous was the source of the one we're undoing
      // pass the dCurrPos along, otherwise leave it be.
      prevloop->dCurrPos = fmod(loop->dCurrPos+loop->lStartAdj, prevloop->lLoopLength);
   }
   
   popHeadLoop(pLS);
   //DBG(fprintf(stderr, "Undoing last loop %08x: new head is %08x\n", (unsigned)loop,
	       //(unsigned)pLS->headLoopChunk);)
}


void redoLoop(SooperLooper *pLS)
{
   LoopChunk *loop = NULL;
   LoopChunk *nextloop = NULL;

   if (pLS->headLoopChunk) {
      loop = pLS->headLoopChunk;
      nextloop = loop->next;
   }
   else if (pLS->tailLoopChunk) {
      // we've undone everything, use the tail
      loop = NULL;
      nextloop = pLS->tailLoopChunk;
   }

   if (nextloop) {
      
      if (loop && loop == nextloop->srcloop) {
	 // if the next is using us as a source
	 // pass the dCurrPos along, otherwise leave it be.
	 nextloop->dCurrPos = fmod(loop->dCurrPos+loop->lStartAdj, nextloop->lLoopLength);
      }

      pLS->headLoopChunk = nextloop;
      
      //DBG(fprintf(stderr, "Redoing last loop %08x: new head is %08x\n", (unsigned)loop,
		  //(unsigned)pLS->headLoopChunk);)

   }
}

static void fillLoops(SooperLooper *pLS, LoopChunk *mloop, unsigned long lCurrPos)
{
   LoopChunk *loop=NULL, *nloop, *srcloop;

   // descend to the oldest unfilled loop
   for (nloop=mloop; nloop; nloop = nloop->srcloop)
   {
      if (nloop->frontfill || nloop->backfill) {
	 loop = nloop;
	 continue;
      }
      
      break;
   }

   // everything is filled!
   if (!loop) return;

   // do filling from earliest to latest
   for (; loop; loop=loop->next)
   {
      srcloop = loop->srcloop;
      
      if (loop->frontfill && lCurrPos<=loop->lMarkH && lCurrPos>=loop->lMarkL)
      {
	 // we need to finish off a previous
	 *(loop->pLoopStart + lCurrPos) = 
	    *(srcloop->pLoopStart + (lCurrPos % srcloop->lLoopLength));
      
	 // move the right mark according to rate
	 if (pLS->fCurrRate > 0) {
	    loop->lMarkL = lCurrPos;
	 }
	 else {
	    loop->lMarkH = lCurrPos;
	 }
      
	 // ASSUMPTION: our overdub rate is +/- 1 only
	 if (loop->lMarkL == loop->lMarkH) {
	    // now we take the input from ourself
	    //DBG(fprintf(stderr,"front segment filled for %08x for %08x in at %lu\n",
			//(unsigned)loop, (unsigned) srcloop, loop->lMarkL);)
	    loop->frontfill = 0;
	    loop->lMarkL = loop->lMarkH = MAXLONG;
	 }
      }
      else if (loop->backfill && lCurrPos<=loop->lMarkEndH && lCurrPos>=loop->lMarkEndL)		
      {

	 // we need to finish off a previous
	 *(loop->pLoopStart + lCurrPos) = 
	    *(srcloop->pLoopStart +
	      ((lCurrPos  + loop->lStartAdj - loop->lEndAdj) % srcloop->lLoopLength));
	 
	  
	 // move the right mark according to rate
	 if (pLS->fCurrRate > 0) {
	    loop->lMarkEndL = lCurrPos;
	 }
	 else {
	    loop->lMarkEndH = lCurrPos;

	 }
	 // ASSUMPTION: our overdub rate is +/- 1 only
	 if (loop->lMarkEndL == loop->lMarkEndH) {
	    // now we take the input from ourself
	    //DBG(fprintf(stderr,"back segment filled in for %08x from %08x at %lu\n",
			//(unsigned)loop, (unsigned)srcloop, loop->lMarkEndL);)
	    loop->backfill = 0;
	    loop->lMarkEndL = loop->lMarkEndH = MAXLONG;
	 }

      }

      if (mloop == loop) break;
   }

}

static LoopChunk* transitionToNext(SooperLooper *pLS, LoopChunk *loop, int nextstate);

static LoopChunk * beginOverdub(SooperLooper *pLS, LoopChunk *loop)
{
   LoopChunk * srcloop;
   // make new loop chunk
   loop = pushNewLoopChunk(pLS, loop->lLoopLength);
   if (loop) {
      pLS->state = STATE_OVERDUB;
      // always the same length as previous loop
      loop->srcloop = srcloop = loop->prev;
      loop->lCycleLength = srcloop->lCycleLength;
      loop->lLoopLength = srcloop->lLoopLength;
      loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
      loop->dCurrPos = srcloop->dCurrPos;
      loop->lStartAdj = 0;
      loop->lEndAdj = 0;
      pLS->nextState = -1;

     // loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*(pLS->pfFeedback));
      loop->dOrigFeedback = 1.0;
      if (loop->dCurrPos > 0) 
	 loop->frontfill = 1;
      else
	 loop->frontfill = 0;

      loop->backfill = 1;
      // logically we need to fill in the cycle up to the
      // srcloop's current position.
      // we let the overdub loop itself do this when it gets around to it
      
      if (pLS->fCurrRate < 0) {
	 pLS->fCurrRate = -1.0;
	 // negative rate
	 // need to fill in between these values 
	 loop->lMarkL = (unsigned long) loop->dCurrPos + 1;		       
	 loop->lMarkH = loop->lLoopLength - 1;
	 loop->lMarkEndL = 0;
	 loop->lMarkEndH = (unsigned long) loop->dCurrPos;
      } else {
	 pLS->fCurrRate = 1.0;
	 loop->lMarkL = 0;
	 loop->lMarkH = (unsigned long) loop->dCurrPos - 1;
	 loop->lMarkEndL = (unsigned long) loop->dCurrPos;
	 loop->lMarkEndH = loop->lLoopLength - 1;
      }
      
      //DBG(fprintf(stderr,"Mark at L:%lu  h:%lu\n",loop->lMarkL, loop->lMarkH));
      //DBG(fprintf(stderr,"EndMark at L:%lu  h:%lu\n",loop->lMarkEndL, loop->lMarkEndH));
      //DBG(fprintf(stderr,"Entering OVERDUB state: srcloop is %08x\n", (unsigned)srcloop));
   }

   return loop;
}

static LoopChunk * beginReplace(SooperLooper *pLS, LoopChunk *loop)
{
   LoopChunk * srcloop;

   // NOTE: THIS SHOULD BE IDENTICAL TO OVERDUB
   // make new loop chunk
   loop = pushNewLoopChunk(pLS, loop->lLoopLength);
   if (loop)
   {
      pLS->state = STATE_REPLACE;
		       
      // always the same length as previous loop
      loop->srcloop = srcloop = loop->prev;
      loop->lCycleLength = srcloop->lCycleLength;
      loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*pLS->pfFeedback);
		       
      loop->lLoopLength = srcloop->lLoopLength;
      loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
      loop->dCurrPos = srcloop->dCurrPos;
      loop->lStartAdj = 0;
      loop->lEndAdj = 0;
      pLS->nextState = -1;

      loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*pLS->pfFeedback);

      if (loop->dCurrPos > 0) 
	 loop->frontfill = 1;
      else
	 loop->frontfill = 0;


      loop->backfill = 1;
      // logically we need to fill in the cycle up to the
      // srcloop's current position.
      // we let the  loop itself do this when it gets around to it
		       
		       
      if (pLS->fCurrRate < 0) {
	 pLS->fCurrRate = -1.0;
	 // negative rate
	 // need to fill in between these values 
	 loop->lMarkL = (unsigned long) loop->dCurrPos + 1;		       
	 loop->lMarkH = loop->lLoopLength - 1;
	 loop->lMarkEndL = 0;
	 loop->lMarkEndH = (unsigned long) loop->dCurrPos;
      } else {
	 pLS->fCurrRate = 1.0;
	 loop->lMarkL = 0;
	 loop->lMarkH = (unsigned long) loop->dCurrPos - 1;
	 loop->lMarkEndL = (unsigned long) loop->dCurrPos;
	 loop->lMarkEndH = loop->lLoopLength - 1;
      }
		       
      //DBG(fprintf(stderr,"Mark at L:%lu  h:%lu\n",loop->lMarkL, loop->lMarkH);
	  //fprintf(stderr,"EndMark at L:%lu  h:%lu\n",loop->lMarkEndL, loop->lMarkEndH);
	  //fprintf(stderr,"Entering REPLACE state: srcloop is %08x\n", (unsigned)srcloop));
   }

   return loop;
}


static LoopChunk * transitionToNext(SooperLooper *pLS, LoopChunk *loop, int nextstate)
{
   LoopChunk * newloop = loop;
   
   switch(nextstate)
   {
      case STATE_PLAY:
      case STATE_MUTE:
	 // nothing special
	 break;

      case STATE_OVERDUB:
	 newloop = beginOverdub(pLS, loop);
	 break;

      case STATE_REPLACE:
	 newloop = beginReplace(pLS, loop);
	 break;
   }

   if (nextstate != -1) {
      //DBG(fprintf(stderr,"Entering state %d from %d\n", nextstate, pLS->state));
      pLS->state = nextstate;

   }
   else {
      //DBG(fprintf(stderr,"Next state is -1?? Why?\n"));
      pLS->state = STATE_PLAY;
   }
   
   return newloop;
}


/*****************************************************************************/

/* Run the sampler  for a block of SampleCount samples. */
void SooperLooperPlugin::run(LV2_Handle instance, uint32_t SampleCount)
{
    SooperLooperPlugin *plugin;
    plugin = (SooperLooperPlugin *) instance;

// LADSPA_Data * pfBuffer;
  LADSPA_Data * pfInput;
  LADSPA_Data * pfOutput;
  LADSPA_Data fDry=1.0, fWet=1.0, tmpWet;
  LADSPA_Data fInputSample;
  LADSPA_Data fOutputSample;

  LADSPA_Data fRate = 1.0;
  LADSPA_Data fScratchPos = 0.0;
  LADSPA_Data fTrigThresh = 0.0;
  
  LADSPA_Data fTapTrig = 0.0;
  
  LADSPA_Data fFeedback = 1.0;
  unsigned int lCurrPos = 0;
  unsigned int lpCurrPos = 0;  
  long slCurrPos;
  double dDummy;
  int firsttime, backfill;
  
  float fPosRatio;
  
  SooperLooper * pLS;
  LoopChunk *loop, *srcloop;


  unsigned long lSampleIndex;

  
  pLS = plugin->pLS;

  if (!pLS || !plugin->in_0 || !plugin->out_0) {
     // something is badly wrong!!!
     return;
  }
  
  pfInput = plugin->in_0;
  pfOutput = plugin->out_0;
  // pfBuffer = (LADSPA_Data *)pLS->pSampleBuf;

  // we set up default bindings in case the host hasn't
  if (!pLS->pfQuantMode)
     pLS->pfQuantMode = &pLS->fQuantizeMode;
  if (!pLS->pfRoundMode)
     pLS->pfRoundMode = &pLS->fRoundMode;
  if (!pLS->pfRedoTapMode)
     pLS->pfRedoTapMode = &pLS->fRedoTapMode;

  if (pLS->pfTrigThresh) {
     fTrigThresh = *pLS->pfTrigThresh;
  }

  if (pLS->pfTapCtrl) {
     fTapTrig = *(pLS->pfTapCtrl);
  }
  
  if (fTapTrig == pLS->fLastTapCtrl) {
     // ignore it, we must have a change to trigger a tap
     
  } else if (pLS->lTapTrigSamples >= TRIG_SETTLE) {
     // signal to below to trigger the delay tap command
     if (pLS->bPreTap) {
	// ignore the first time
	pLS->bPreTap = 0;
     }
     else {
	//DBG(fprintf(stderr, "Tap triggered\n"));
     }
  }
  pLS->fLastTapCtrl = fTapTrig;
 
  //fRateSwitch = *(pLS->pfRateSwitch);


  if (pLS->pfScratchPos)
     fScratchPos = LIMIT_BETWEEN_0_AND_1(*(pLS->pfScratchPos));

  
  // the rate switch is ON if it is below 1 but not 0
  // rate is 1 if rate switch is off
  //if (fRateSwitch > 1.0 || fRateSwitch==0.0) {
  //  fRate = 1.0;
  //}
  //else {
     //fprintf(stderr, "rateswitch is 1.0: %f!\n", fRate);
  //}

  if (pLS->pfWet)
     fWet = LIMIT_BETWEEN_0_AND_1(*(pLS->pfWet));

  if (pLS->pfDry)
     fDry = LIMIT_BETWEEN_0_AND_1(*(pLS->pfDry));  


  if (pLS->pfFeedback) {
     fFeedback = LIMIT_BETWEEN_0_AND_1(*(pLS->pfFeedback));

     // probably against the rules, but I'm doing it anyway
     *pLS->pfFeedback = fFeedback;
  }


  loop = pLS->headLoopChunk;

  fRate = pLS->fCurrRate;
  
  lSampleIndex = 0;


  /* 
   * LV2 run, reading control ports and setting states 
   */

    if (*(plugin->reset) > 0.0) {
        clearLoopChunks(pLS);
        plugin->recording = 0;
        plugin->playing = 0;
        plugin->started = 0;
        *(plugin->reset) = 0.0;
    }
    
    if (*(plugin->play_pause) > 0.0 && !plugin->playing) {
        plugin->playing = 1;
        if (!plugin->started) {
            if (plugin->recording) {
                plugin->pLS->state = STATE_TRIG_START;
                printf("Starting!!\n");
                plugin->started = 1;
            }
        } else {
            if (plugin->recording) {
                if (loop) {
                    loop = beginOverdub(pLS, loop);  
                    if (loop)
                        srcloop = loop->srcloop;
                    else
                        srcloop = NULL;
                }
            } else {
                plugin->pLS->state = STATE_PLAY;
            }
        }
    } else if (*(plugin->play_pause) <= 0.0 && plugin->playing) {
            plugin->pLS->state = STATE_OFF;
            plugin->playing = 0;
    }
    
    if (*(plugin->record) > 0.0 && !plugin->recording) {
        plugin->recording = 1;
        if (!plugin->started) {
            if (plugin->playing) {
                printf("Starting!!\n");
                plugin->pLS->state = STATE_TRIG_START;
                plugin->started = 1;
            }
        } else {
            if (plugin->playing) {
                if (loop) {
                    loop = beginOverdub(pLS, loop);  
                    if (loop)
                        srcloop = loop->srcloop;
                    else
                        srcloop = NULL;
                }
            }
        }
    } else if (*(plugin->record) <= 0.0 && plugin->recording) {
        plugin->recording = 0;
        printf("stopping record\n");
        if (plugin->started && plugin->playing)
            plugin->pLS->state = STATE_PLAY;
        else 
            plugin->pLS->state = STATE_OFF;
    } 

    if (*(plugin->reset) > 0.0) {
        clearLoopChunks(pLS);
        plugin->recording = 0;
        plugin->playing = 0;
        plugin->started = 0;
        *(plugin->reset) = 0.0;
    }

    if (*(plugin->undo) > 0.0) {
        if(loop) { 
            undoLoop(pLS); 
            pLS->state = STATE_PLAY;
        }
        *(plugin->undo) = 0.0;
    }

    if (*(plugin->redo) > 0.0) {
        if(loop) { 
            redoLoop(pLS); 
            pLS->state = STATE_PLAY;
        }
        *(plugin->redo) = 0.0;
    }
    /* end control reading */

  while (lSampleIndex < SampleCount)
  {
     loop = pLS->headLoopChunk;
     switch(pLS->state)
     {

	case STATE_TRIG_START:
	{
	   // we are looking for the threshold to actually
	   // start the recording on (while still playing dry signal)
	   
	   for (;lSampleIndex < SampleCount;
		lSampleIndex++)
	   {
	      fInputSample = pfInput[lSampleIndex];
	      if (fInputSample > fTrigThresh
		  || fTrigThresh==0.0)
	      {
		 
		 loop = pushNewLoopChunk(pLS, 0);
		 if (loop) {
		    pLS->state = STATE_RECORD;
		    // force rate to be 1.0
		    fRate = pLS->fCurrRate = 1.0;

		    loop->pLoopStop = loop->pLoopStart;
		    loop->lLoopLength = 0;
		    loop->lStartAdj = 0;
		    loop->lEndAdj = 0;
		    loop->dCurrPos = 0.0;
		    loop->firsttime = 0;
		    loop->lMarkL = loop->lMarkEndL = MAXLONG;
		    loop->frontfill = loop->backfill = 0;
		    loop->lCycles = 1; // at first just one		 
		    loop->srcloop = NULL;
		    pLS->nextState = -1;
		    loop->dOrigFeedback = fFeedback;
		    break;
		 }
		 else {
		    //DBG(fprintf(stderr, "out of memory! back to PLAY mode\n"));
		    pLS->state = STATE_PLAY;
		    break;
		 }

	      }
	      
	      pfOutput[lSampleIndex] = fDry * fInputSample;
	   }
     
	} break;
	
	case STATE_RECORD:
	{
	   // play the input out while recording it.
	   
	   for (;lSampleIndex < SampleCount;
		lSampleIndex++)
	   {
	      // wrap at the proper loop end
	      lCurrPos = static_cast<unsigned int>(loop->dCurrPos);
	      if ((char *)(lCurrPos + loop->pLoopStart) >= (pLS->pSampleBuf + pLS->lBufferSize)) {
             // stop the recording RIGHT NOW
             // we don't support loop crossing the end of memory
             // it's easier.
             //DBG(fprintf(stderr, "Entering PLAY state -- END of memory! %08x\n",
                     //(unsigned) (pLS->pSampleBuf + pLS->lBufferSize) ));
             pLS->state = STATE_PLAY;
             break;
	      }
	      fInputSample = pfInput[lSampleIndex];
	      
	      *(loop->pLoopStart + lCurrPos) = fInputSample;
	      
	      // increment according to current rate
	      loop->dCurrPos = loop->dCurrPos + fRate;
	      
	      
	      pfOutput[lSampleIndex] = fDry * fInputSample;
	   }

	   // update loop values (in case we get stopped by an event)
	   lCurrPos = ((unsigned int)loop->dCurrPos);
	   loop->pLoopStop = loop->pLoopStart + lCurrPos;
	   loop->lLoopLength = (unsigned long) (loop->pLoopStop - loop->pLoopStart);
	   loop->lCycleLength = loop->lLoopLength;

	   
	} break;

	case STATE_TRIG_STOP:
	{
	   //fprintf(stderr,"in trigstop\n");	   
	   // play the input out.  Keep recording until we go
	   // above the threshold, then go into next state.
	   
	   for (;lSampleIndex < SampleCount;
		lSampleIndex++)
	   {
	      lCurrPos = (unsigned int) loop->dCurrPos;
	      
	      fInputSample = pfInput[lSampleIndex];
	      
	      
	      if ( fInputSample > fTrigThresh
		  || fTrigThresh == 0.0) {
		 //DBG(fprintf(stderr,"Entering %d state\n", pLS->nextState));
		 pLS->state = pLS->nextState;
		 // reset for audio ramp
		 pLS->lRampSamples = XFADE_SAMPLES;
		 loop->pLoopStop = loop->pLoopStart + lCurrPos;
		 loop->lLoopLength = (unsigned long) (loop->pLoopStop - loop->pLoopStart);
		 loop->lCycles = 1;
		 loop->lCycleLength = loop->lLoopLength;
		 loop->dCurrPos = 0.0;
		 break;
	      }
	      
	      *(loop->pLoopStart + lCurrPos) = fInputSample;
	      
	      // increment according to current rate
	      loop->dCurrPos = loop->dCurrPos + fRate;


	      if ((char *)(loop->pLoopStart + (unsigned int)loop->dCurrPos)
		  > (pLS->pSampleBuf + pLS->lBufferSize)) {
		 // out of space! give up for now!
		 // undo!
		 pLS->state = STATE_PLAY;
		 //undoLoop(pLS);
		 //DBG(fprintf(stderr,"Record Stopped early! Out of memory!\n"));
		 loop->dCurrPos = 0.0;
		 break;
	      }

	      
	      pfOutput[lSampleIndex] = fDry * fInputSample;


	   }

	   // update loop values (in case we get stopped by an event)
	   loop->pLoopStop = loop->pLoopStart + lCurrPos;
	   loop->lLoopLength = (unsigned long) (loop->pLoopStop - loop->pLoopStart);
	   loop->lCycleLength = loop->lLoopLength;
	   loop->lCycles = 1;
	   
	} break;


	
	case STATE_OVERDUB:
	case STATE_REPLACE:
	{
	   if (loop &&  loop->lLoopLength && loop->srcloop)
	   {
	      srcloop = loop->srcloop;
	      
	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {
	      
		 lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);
		 
		 fInputSample = pfInput[lSampleIndex];
		 
		 fillLoops(pLS, loop, lCurrPos);

		 if (pLS->state == STATE_OVERDUB)
		 {
		    // use our self as the source (we have been filled by the call above)
		    fOutputSample = fWet  *  *(loop->pLoopStart + lCurrPos)
		       + fDry * fInputSample;
		    
		    *(loop->pLoopStart + lCurrPos) =  
		       (fInputSample + 0.95 * fFeedback *  *(loop->pLoopStart + lCurrPos));
		 }
		 else {
		    // state REPLACE use only the new input
		    // use our self as the source (we have been filled by the call above)
		    fOutputSample = fDry * fInputSample;
		    
		    *(loop->pLoopStart + lCurrPos) = fInputSample;

		 }
		 
		 pfOutput[lSampleIndex] = fOutputSample;
	      
		 // increment and wrap at the proper loop end
		 loop->dCurrPos = loop->dCurrPos + fRate;
	      
		 if (loop->dCurrPos < 0)
		 {
		    // our rate must be negative
		    // adjust around to the back
		    loop->dCurrPos += loop->lLoopLength;

		    if (pLS->fNextCurrRate != 0) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0;
		       //DBG(fprintf(stderr, "Starting quantized rate change\n"));
		    }

		 }
		 else if (loop->dCurrPos >= loop->lLoopLength) {
		    // wrap around length
		    loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
		    if (pLS->fNextCurrRate != 0) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0;
		       //DBG(fprintf(stderr, "Starting quantized rate change\n"));
		    }
		 }
		 
	      }


	      
	   }
	   else {
	      goto passthrough;

	   }
	   
	   
	} break;


	case STATE_MULTIPLY:
	{
	   if (loop && loop->lLoopLength && loop->srcloop)
	   {
	      srcloop = loop->srcloop;
	      firsttime = loop->firsttime;

	      if (pLS->nextState == STATE_MUTE) {
		 // no loop output
		 fWet = 0.0;
	      }
	      

	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {

		 lpCurrPos =(unsigned int) fmod(loop->dCurrPos + loop->lStartAdj, srcloop->lLoopLength);
		 slCurrPos =(long) loop->dCurrPos;

		 fillLoops(pLS, loop, lpCurrPos);
		 
		 fInputSample = pfInput[lSampleIndex];
		 

		 // always use the source loop as the source
		 
		 fOutputSample = (fWet *  *(srcloop->pLoopStart + lpCurrPos)
				  + fDry * fInputSample);


		 if (slCurrPos < 0) {
		    // this is part of the loop that we need to ignore
		    // fprintf(stderr, "Ignoring at %ul\n", lCurrPos);
		 }
		 else if ((loop->lCycles <=1 && *pLS->pfQuantMode != 0)
		     || (slCurrPos > (unsigned)(loop->lMarkEndL) && *pLS->pfRoundMode == 0)) {
		    // do not include the new input
		    *(loop->pLoopStart + slCurrPos)
		       = fFeedback *  *(srcloop->pLoopStart + lpCurrPos);
		    // fprintf(stderr, "Not including input at %ul\n", lCurrPos);
		 }
		 else {
		    *(loop->pLoopStart + slCurrPos)
		       = (fInputSample + 0.95 *  fFeedback *  *(srcloop->pLoopStart + lpCurrPos));
		 }
		 
		 pfOutput[lSampleIndex] = fOutputSample;
	      
		 // increment 
		 loop->dCurrPos = loop->dCurrPos + fRate;
	      

		 if (slCurrPos > 0 && (unsigned)(*(loop->pLoopStart + slCurrPos))
		     > (unsigned)(*(pLS->pSampleBuf + pLS->lBufferSize))) {
		    // out of space! give up for now!
		    // undo!
		    pLS->state = STATE_PLAY;
		    undoLoop(pLS);
		    //DBG(fprintf(stderr,"Multiply Undone! Out of memory!\n"));
		    break;
		 }

		 // ASSUMPTION: our rate is +1 only		 
		 if (loop->dCurrPos  >= (loop->lLoopLength)) {
		    if (loop->dCurrPos >= loop->lMarkEndH) {
		       // we be done this only happens in round mode
		       // adjust curr position
		       loop->lMarkEndH = MAXLONG;
		       backfill = loop->backfill = 0;
		       // do adjust it for our new length
		       loop->dCurrPos = 0.0;

		       loop->lLoopLength = loop->lCycles * loop->lCycleLength;
		       loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
		       
		       
		       loop = transitionToNext(pLS, loop, pLS->nextState);
		       break;
		    }
		    // increment cycle and looplength
		    loop->lCycles += 1;
		    loop->lLoopLength += loop->lCycleLength;
		    loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
		    //loop->lLoopStop = loop->lLoopStart + loop->lLoopLength;
		    // this signifies the end of the original cycle
		    loop->firsttime = 0;
		    //DBG(fprintf(stderr,"Multiply added cycle %lu\n", loop->lCycles));

		 }
	      }
	   }
	   else {
	      goto passthrough;
	   }
	   
	} break;

	case STATE_INSERT:
	{
	   if (loop && loop->lLoopLength && loop->srcloop)
	   {
	      srcloop = loop->srcloop;
	      firsttime = loop->firsttime;

	      if (pLS->nextState == STATE_MUTE) {
		 // no loop output
		 fWet = 0.0;
	      }
	      

	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {

		 lpCurrPos =(unsigned int) fmod(loop->dCurrPos, srcloop->lLoopLength);
		 lCurrPos =(unsigned int) loop->dCurrPos;

		 fillLoops(pLS, loop, lCurrPos);
		 
		 fInputSample = pfInput[lSampleIndex];

		 if (firsttime && *pLS->pfQuantMode != 0 )
		 {
		    // just the source and input
		    fOutputSample = (fWet *  *(srcloop->pLoopStart + lpCurrPos)
				     + fDry * fInputSample);
		    
		    // do not include the new input
		    //*(loop->pLoopStart + lCurrPos)
		    //  = fFeedback *  *(srcloop->pLoopStart + lpCurrPos);

		 }
		 else if (lCurrPos > loop->lMarkEndL && *pLS->pfRoundMode == 0)
		 {
		    // insert zeros, we finishing an insert with nothingness
		    fOutputSample = fDry * fInputSample;

		    *(loop->pLoopStart + lCurrPos) = 0.0;

		 }
		 else {
		    // just the input we are now inserting
		    fOutputSample = fDry * fInputSample;

		    *(loop->pLoopStart + lCurrPos) = (fInputSample);

		 }
		 
		 
		 pfOutput[lSampleIndex] = fOutputSample;
	      
		 // increment 
		 loop->dCurrPos = loop->dCurrPos + fRate;
	      

		 
		 if ((unsigned long)loop->dCurrPos >= loop->lMarkEndH) {
		    // we be done.. this only happens in round mode
		    // adjust curr position to 0

		    
		    loop->lMarkEndL = (unsigned long) loop->dCurrPos;
		    loop->lMarkEndH = loop->lLoopLength - 1;
		    backfill = loop->backfill = 1;

		    loop->lLoopLength = loop->lCycles * loop->lCycleLength;
		    loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
		    
		    
		    loop = transitionToNext(pLS, loop, pLS->nextState);
		    //DBG(fprintf(stderr,"Entering state %d from insert\n", pLS->state));
		    break;
		 }

		 // ASSUMPTION: our rate is +1 only		 
		 if (firsttime && lCurrPos % loop->lCycleLength == 0)
		 {
		    firsttime = loop->firsttime = 0;
		    //DBG(fprintf(stderr, "first time done\n"));
		 }
		 
		 if ((lCurrPos % loop->lCycleLength) == ((loop->lInsPos-1) % loop->lCycleLength)) {

		    if ((unsigned)(*(loop->pLoopStart + loop->lLoopLength + loop->lCycleLength))
			> (unsigned)(*(pLS->pSampleBuf + pLS->lBufferSize)))
		    {
		       // out of space! give up for now!
		       pLS->state = STATE_PLAY;
		       //undoLoop(pLS);
		       //DBG(fprintf(stderr,"Insert finish early! Out of memory!\n"));
		       break;
		    }
		    else {
		       // increment cycle and looplength
		       loop->lCycles += 1;
		       loop->lLoopLength += loop->lCycleLength;
		       loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
		       //loop->lLoopStop = loop->lLoopStart + loop->lLoopLength;
		       // this signifies the end of the original cycle
		       //DBG(fprintf(stderr,"insert added cycle. Total=%lu\n", loop->lCycles));
		    }
		 }
	      }
	   }
	   else {
	      goto passthrough;
	   }
	   
	} break;

	
	
	case STATE_PLAY:
	case STATE_ONESHOT:
	case STATE_SCRATCH:
	case STATE_MUTE:
	{
	   //fprintf(stderr,"in play begin\n");	   
	   // play  the input out mixed with the recorded loop.
	   if (loop && loop->lLoopLength)
	   {
	      tmpWet = fWet;
	      
	      if (pLS->state == STATE_MUTE) {
		 if (pLS->lRampSamples <= 0)
		    tmpWet = 0.0;
		 // otherwise the ramp takes care of it
	      }
	      else if(pLS->state == STATE_SCRATCH)
	      {
	      
		 // calculate new rate if rateSwitch is on
		 fPosRatio = (loop->dCurrPos / loop->lLoopLength);
	      
		 if (pLS->fLastScratchVal != fScratchPos
		     && pLS->lScratchSamples > 0) {
		    // we have a change in scratching pos. Find new rate

		    if (pLS->lScratchSamples < 14000) {
		       pLS->fCurrScratchRate = (fScratchPos - fPosRatio) * loop->lLoopLength
			  / pLS->lScratchSamples;

		    }
		    else if (pLS->bRateCtrlActive && pLS->pfRate) {
		       fRate = *pLS->pfRate;
		    }
		    else {
		       fRate = 0.0;
		    }
		    
		    pLS->lScratchSamples = 0;
		    pLS->fLastScratchVal = fScratchPos;


		    
		    //fprintf(stderr, "fScratchPos: %f   fCurrScratchRate: %f  \n", fScratchPos,
		    //   pLS->fCurrScratchRate);
		 
		 }
		 else if (fabs(pLS->fCurrScratchRate) < 0.2
			  || ( pLS->lScratchSamples > 14000)
			  || ( pLS->fCurrScratchRate > 0.0 && (fPosRatio >= pLS->fLastScratchVal ))
			  || ( pLS->fCurrScratchRate < 0.0 && (fPosRatio <= pLS->fLastScratchVal )))
		 {
		    // we have reached the destination, no more scratching
		    pLS->fCurrScratchRate = 0.0;

		    if (pLS->bRateCtrlActive && pLS->pfRate) {
		       fRate = *pLS->pfRate;
		    }
		    else {
		       // pure scratching
		       fRate = 0.0;
		    }
		    //fprintf(stderr, "fScratchPos: %f   fCurrScratchRate: %f  ******\n", fScratchPos,
		    //	   pLS->fCurrScratchRate);
		 
		 }
		 else {
		    fRate = pLS->fCurrScratchRate;
		 }

	      }


	      srcloop = loop->srcloop;
	      
	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {
		 lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);
		 //fprintf(stderr, "curr = %u\n", lCurrPos);



		 // modify fWet if we are in a ramp up/down
		 if (pLS->lRampSamples > 0) {
		    if (pLS->state == STATE_MUTE) {
		       //negative linear ramp
		       tmpWet = fWet * (pLS->lRampSamples * 1.0) / XFADE_SAMPLES;
		    }
		    else {
		       // positive linear ramp
		       tmpWet = fWet * (XFADE_SAMPLES - pLS->lRampSamples)
			  * 1.0 / XFADE_SAMPLES;
		    }

		    pLS->lRampSamples -= 1;
		 }

		 
		 // fill loops if necessary
		 fillLoops(pLS, loop, lCurrPos);

		 		    
		 fInputSample = pfInput[lSampleIndex];
		 fOutputSample =   tmpWet *  *(loop->pLoopStart + lCurrPos)
		    + fDry * fInputSample;
		 
		 // increment and wrap at the proper loop end
		 loop->dCurrPos = loop->dCurrPos + fRate;

		 pfOutput[lSampleIndex] = fOutputSample;
		 

		 if (loop->dCurrPos >= loop->lLoopLength) {
		    if (pLS->state == STATE_ONESHOT) {
		       // done with one shot
		       //DBG(fprintf(stderr, "finished ONESHOT\n"));
		       pLS->state = STATE_MUTE;
		       pLS->lRampSamples = XFADE_SAMPLES;
		       //fWet = 0.0;
		    }

		    if (pLS->fNextCurrRate != 0) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0;
		       //DBG(fprintf(stderr, "Starting quantized rate change\n"));
		    }
		    
		 }
		 else if (loop->dCurrPos < 0)
		 {
		    // our rate must be negative
		    // adjust around to the back
		    loop->dCurrPos += loop->lLoopLength;
		    if (pLS->state == STATE_ONESHOT) {
		       // done with one shot
		       //DBG(fprintf(stderr, "finished ONESHOT neg\n"));
		       pLS->state = STATE_MUTE;
		       //fWet = 0.0;
		       pLS->lRampSamples = XFADE_SAMPLES;
		    }

		    if (pLS->fNextCurrRate != 0) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0;
		       //DBG(fprintf(stderr, "Starting quantized rate change\n"));
		    }

		 }


	      }
	      
	      
	      // recenter around the mod
	      lCurrPos = (unsigned int) fabs(fmod(loop->dCurrPos, loop->lLoopLength));
	      
	      loop->dCurrPos = lCurrPos + modf(loop->dCurrPos, &dDummy); 
	   }
	   else {
	      goto passthrough;
	   }
	   
	} break;

	case STATE_DELAY:
	{
	   if (loop && loop->lLoopLength)
	   {
	      // the loop length is our delay time.
	      backfill = loop->backfill;
	      
	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {
		 // wrap properly
		 lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);

		 fInputSample = pfInput[lSampleIndex];

		 if (backfill && lCurrPos >= loop->lMarkEndL && lCurrPos <= loop->lMarkEndH) {
		    // our delay buffer is invalid here, clear it
		    *(loop->pLoopStart + lCurrPos) = 0.0;

		    if (fRate > 0) {
		       loop->lMarkEndL = lCurrPos;
		    }
		    else {
		       loop->lMarkEndH = lCurrPos;
		    }
		 }


		 fOutputSample =   fWet *  *(loop->pLoopStart + lCurrPos)
		    + fDry * fInputSample;


		 if (!pLS->bHoldMode) {
		    // now fill in from input if we are not holding the delay
		    *(loop->pLoopStart + lCurrPos) = 
		      (fInputSample +  fFeedback *  *(loop->pLoopStart + lCurrPos));
		 }
		 
		 pfOutput[lSampleIndex] = fOutputSample;
		 
		 // increment 
		 loop->dCurrPos = loop->dCurrPos + fRate;

		 if (backfill && loop->lMarkEndL == loop->lMarkEndH) {
		    // no need to clear the buf first now
		    backfill = loop->backfill = 0;
		 }

		 else if (loop->dCurrPos < 0)
		 {
		    // our rate must be negative
		    // adjust around to the back
		    loop->dCurrPos += loop->lLoopLength;
		 }

		 
	      }

	      // recenter around the mod
	      lCurrPos = (unsigned int) fabs(fmod(loop->dCurrPos, loop->lLoopLength));
	      
	      loop->dCurrPos = lCurrPos + modf(loop->dCurrPos, &dDummy); 

	      
	   }
	   else {
	      goto passthrough;
	   }
	} break;
	
	default:
	{
	   goto passthrough;

	}  break;
	
     }

     goto loopend;
     
    passthrough:

     // simply play the input out directly
     // no loop has been created yet
     for (;lSampleIndex < SampleCount;
	  lSampleIndex++)
     {
	pfOutput[lSampleIndex] = fDry * pfInput[lSampleIndex];
     }
     
     
    loopend:
     continue;
  }
  
  // keep track of time between triggers to ignore settling issues
  // pLS->lRecTrigSamples += SampleCount;
  pLS->lScratchSamples += SampleCount;  
  pLS->lTapTrigSamples += SampleCount;


  // update output ports
  if (pLS->pfStateOut) {
     *pLS->pfStateOut = (LADSPA_Data) pLS->state;
  }

  if (pLS->pfSecsFree) {
     *pLS->pfSecsFree = ((LADSPA_Data)SAMPLE_MEMORY) -
	(pLS->headLoopChunk ?
	 ((((unsigned)(*(pLS->headLoopChunk->pLoopStop)) - (unsigned)(*(pLS->pSampleBuf)))
	  / sizeof(LADSPA_Data)) / pLS->fSampleRate)   :
	 0);
  }
  
  if (loop) {
     if (pLS->pfLoopPos)
	*pLS->pfLoopPos = (LADSPA_Data) (loop->dCurrPos / pLS->fSampleRate);

     if (pLS->pfLoopLength)
	*pLS->pfLoopLength = ((LADSPA_Data) loop->lLoopLength) / pLS->fSampleRate;

     if (pLS->pfCycleLength)
	*pLS->pfCycleLength = ((LADSPA_Data) loop->lCycleLength) / pLS->fSampleRate;

     
  }
  else {
     if (pLS->pfLoopPos)
	*pLS->pfLoopPos = 0.0;
     if (pLS->pfLoopLength)
	*pLS->pfLoopLength = 0.0;     
     if (pLS->pfCycleLength)
	*pLS->pfCycleLength = 0.0;

     if (pLS->pfStateOut && pLS->state != STATE_MUTE && pLS->state != STATE_TRIG_START)
	*pLS->pfStateOut = (LADSPA_Data) STATE_OFF;

  }
 
}

/*****************************************************************************/

/*****************************************************************************/

static const LV2_Descriptor Descriptor =
{
    PLUGIN_URI,
    SooperLooperPlugin::instantiate,
    SooperLooperPlugin::connect_port,
    SooperLooperPlugin::activate,
    SooperLooperPlugin::run,
    SooperLooperPlugin::deactivate,
    SooperLooperPlugin::cleanup,
    SooperLooperPlugin::extension_data
};

/**********************************************************************************************************************************************************/

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    if (index == 0) return &Descriptor;
    else return NULL;
}

/**********************************************************************************************************************************************************/

LV2_Handle SooperLooperPlugin::instantiate(const LV2_Descriptor* descriptor, double SampleRate, const char* bundle_path, const LV2_Feature* const* features)
{
    SooperLooperPlugin *plugin = new SooperLooperPlugin();

    SooperLooper * pLS;
    plugin->started = 0;
    plugin->playing = 0;
    // important note: using calloc to zero all data
    pLS = (SooperLooper *) calloc(1, sizeof(SooperLooper));
    if (pLS == NULL) 
      return NULL;
    plugin->pLS = pLS;
  
   pLS->fSampleRate = (LADSPA_Data)SampleRate;

   // we do include the LoopChunk structures in the Buf, so we really
   // get a little less the SAMPLE_MEMORY seconds
   pLS->lBufferSize = (unsigned long)((LADSPA_Data)SampleRate * SAMPLE_MEMORY * sizeof(LADSPA_Data));
   
   pLS->pSampleBuf = (char*)calloc(pLS->lBufferSize, 1);
   if (pLS->pSampleBuf == NULL) {
      free(pLS);
      return NULL;
   }

   /* just one for now */
   //pLS->lLoopStart = 0;
   //pLS->lLoopStop = 0;   
   //pLS->lCurrPos = 0;

   pLS->state = STATE_PLAY;

   //DBG(fprintf(stderr,"instantiated\n"));

   
   pLS->pfQuantMode = &pLS->fQuantizeMode;
   pLS->pfRoundMode = &pLS->fRoundMode;
   pLS->pfRedoTapMode = &pLS->fRedoTapMode;
    return (LV2_Handle)plugin;
}

/**********************************************************************************************************************************************************/

void SooperLooperPlugin::activate(LV2_Handle instance)
{
  SooperLooperPlugin *plugin = (SooperLooperPlugin *) instance;

  SooperLooper *pLS = plugin->pLS;
  pLS->lLastMultiCtrl = -1;

  pLS->lScratchSamples = 0;
  pLS->lTapTrigSamples = 0;
  pLS->lRampSamples = 0;
  pLS->bPreTap = 1; // first tap init
  pLS->fLastScratchVal = 0.0;
  pLS->fLastTapCtrl = -1;
  pLS->fCurrRate = 1.0;
  pLS->fNextCurrRate = 0.0;
  pLS->fQuantizeMode = 0;
  pLS->fRoundMode = 0;  
  pLS->bHoldMode = 0;
  pLS->fRedoTapMode = 1;
  pLS->bRateCtrlActive = 0;
  
  pLS->state = STATE_PLAY;

  clearLoopChunks(pLS);


  if (pLS->pfSecsTotal) {
     *pLS->pfSecsTotal = (LADSPA_Data) SAMPLE_MEMORY;
  }
}

/**********************************************************************************************************************************************************/

void SooperLooperPlugin::deactivate(LV2_Handle instance)
{
}

/**********************************************************************************************************************************************************/

void SooperLooperPlugin::connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    SooperLooperPlugin *plugin;
    plugin = (SooperLooperPlugin *) instance;

    switch (port)
    {
    case IN_0:
        plugin->in_0 = (float*) data;
        break;
    case OUT_0:
        plugin->out_0 = (float*) data;
        break;
    case PLAY_PAUSE:
        plugin->play_pause = (float*) data;
        break;
    case RECORD:
        plugin->record = (float*) data;
        break;
    case RESET:
        plugin->reset = (float*) data;
        break;
    case UNDO:
        plugin->undo = (float*) data;
        break;
    case REDO:
        plugin->redo = (float*) data;
        break;
    }
}

/**********************************************************************************************************************************************************/



/**********************************************************************************************************************************************************/

void SooperLooperPlugin::cleanup(LV2_Handle instance)
{
    delete ((SooperLooperPlugin *) instance);
}

/**********************************************************************************************************************************************************/

const void* SooperLooperPlugin::extension_data(const char* uri)
{
    return NULL;
}
