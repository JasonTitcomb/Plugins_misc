/*
  add_to_rt_report.c - grblHAL plugin to augment the realtime report
  Jason Titcomb, 2026  
  If you are using IO sender, and change the machine units using G20 or G21 for example,
  this plugin will broadcast the updated units modes to all connected streams. 
  Place this file in the plugins/ directory and enable it in your build.

  Part of grblHAL

  Public domain
  This code is is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

  Add this to src\grbl\plugins_init.h
  #if ADD_TO_RT_REPORT_ENABLE
    extern void add_to_rt_report_init (void);
    add_to_rt_report_init();
  #endif


*/


#include "grbl/hal.h"
#include "grbl/stream.h"
#include "grbl/report.h"
#include "driver.h"

#if ADD_TO_RT_REPORT_ENABLE == 1


static on_report_options_ptr on_report_options;
static on_realtime_report_ptr on_realtime_report;

static int8_t curUnits = -1;


static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report)
{
     if(on_realtime_report)
        on_realtime_report(stream_write, report);

     //only output if this is the initial report, or if the units have changed
     if(report.all || curUnits == -1 || curUnits != (gc_state.modal.units_imperial ? 0 : 1))
        gc_state.modal.units_imperial ? stream_write("|Units:0") : stream_write("|Units:1");

    curUnits = gc_state.modal.units_imperial ? 0 : 1;
}

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(!newopt){
        report_plugin("AddToRealtimeReport plugin", "0.1");
        
    }
}

// Plugin initialization
void add_to_rt_report_init(void) {

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;

    on_realtime_report = grbl.on_realtime_report;
    grbl.on_realtime_report = onRealtimeReport;
}

#endif
