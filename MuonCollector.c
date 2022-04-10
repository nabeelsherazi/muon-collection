//==============================================================================
//
// Title:		MuonCollector
// Purpose:		A short description of the application.
//
// Created on:	4/8/2022 at 2:23:52 PM by .
// Copyright:	. All Rights Reserved.
//
//==============================================================================

//==============================================================================
// Include files

#include <NIDAQmx.h>
#include <ansi_c.h>
#include <cvirte.h>		
#include <userint.h>
#include "MuonCollector.h"
#include "toolbox.h"
#include <stdbool.h>

//==============================================================================
// Constants

#define SAMPLING_RATE 50000
#define BUFFER_SIZE 50000
#define TIMEOUT 120
#define MIN_EXPECTED_EDGE_SEP 5E-8 // sec
#define MAX_EXPECTED_EDGE_SEP 1E-5 // sec



//==============================================================================
// Types

//==============================================================================
// Static global variables

static int panelHandle = 0;

static TaskHandle collectionTask;
static bool isInitialized = false;

static CmtThreadFunctionID collectionThreadFnId;
static bool isRunning = false;

static uint32_t dataBuffer[BUFFER_SIZE];

static int plotHandle;

static CmtThreadLockHandle stopLockHandle;
static bool requestStopRunning = false;

//==============================================================================
// Static functions

//==============================================================================
// Global variables

//==============================================================================
// Global functions

/// HIFN The main entry-point function.
int main (int argc, char *argv[])
{
	int error = 0;
	
	/* initialize and load resources */
	nullChk (InitCVIRTE (0, argv, 0));
	errChk (panelHandle = LoadPanel (0, "MuonCollector.uir", PANEL));
	CmtNewLock (NULL, OPT_TL_PROCESS_EVENTS_WHILE_WAITING, &stopLockHandle);
	
	/* display the panel and run the user interface */
	errChk (DisplayPanel (panelHandle));
	errChk (RunUserInterface ());

Error:
	/* clean up */
	if (panelHandle > 0)
		DiscardPanel (panelHandle);
	return 0;
}

//==============================================================================
// UI callback function prototypes

/// HIFN Exit when the user dismisses the panel.
int CVICALLBACK panelCB (int panel, int event, void *callbackData,
		int eventData1, int eventData2)
{
	if (event == EVENT_CLOSE)
		QuitUserInterface (0);
	return 0;
}

// Clear tasks and quit
int CVICALLBACK bye (int panel, int control, int event,
					 void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
			DAQmxClearTask(collectionTask);
			QuitUserInterface(0);
			break;
	}
	return 0;
}

int initializeDAQ() {
	int error = 0;
	// Initialize task handle
	error = error || DAQmxCreateTask("Muon Collection", &collectionTask);
	// Create channel to count number of rising edges
	error = error || DAQmxCreateCICountEdgesChan(collectionTask, "Dev1/ctr1", "Rising Edge Counter", DAQmx_Val_Rising, 0, DAQmx_Val_CountUp);
	// Create channel to count seperation between rising edges
	// error = error || DAQmxCreateCITwoEdgeSepChan (collectionTask, "Dev1/ctr0", "Edge Seperation", MIN_EXPECTED_EDGE_SEP, MAX_EXPECTED_EDGE_SEP, DAQmx_Val_Seconds, DAQmx_Val_Rising, DAQmx_Val_Rising, NULL);
	// Configure sample clock timing
	error = error || DAQmxCfgSampClkTiming(collectionTask, "/Dev1/PFI9", SAMPLING_RATE, DAQmx_Val_Rising, DAQmx_Val_ContSamps, BUFFER_SIZE);
	if (!error) {
		isInitialized = true;
	}
	return error;
}

// Do plot
void plot() {
	// Plot
	if (!plotHandle) {
		plotHandle = PlotY(panelHandle, PANEL_GRAPH, dataBuffer, BUFFER_SIZE, VAL_SHORT_INTEGER, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
	}
	else {
		SetPlotAttribute(panelHandle, PANEL_GRAPH, plotHandle, ATTR_PLOT_YDATA, (void*)dataBuffer);
	}
}

void collect(uint32_t *dataBuffer) {
	// Start task
	DebugPrintf("Running collection in seperate thread\n");
	DAQmxStartTask(collectionTask);
	int read = 0;
	bool done = false;
	while(!done) {
		DAQmxReadCounterU32(collectionTask, BUFFER_SIZE, 1, dataBuffer, BUFFER_SIZE, &read, NULL);
		plot();
	 	// DAQmxReadCounterF64(taskHandle,5000,Timespan,data,5000,&read,NULL);
		
		// Check if we should stop
		int lockObtained = 0;
		CmtGetLockEx(stopLockHandle, 1, CMT_WAIT_FOREVER, &lockObtained);
		if (lockObtained) {
			done = requestStopRunning;
			if (done) {
				DebugPrintf("Received request to stop running early\n");
			}
			CmtReleaseLock(stopLockHandle);
		}
	}
	DebugPrintf("Stopping collection task\n");
	DAQmxStopTask(collectionTask);
	
}


// Starts collection
int CVICALLBACK doRun (int panel, int control, int event,
					   void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
			
			// Not running -> start running
			if (!isRunning) {
				// Initialize the task if needed
				if (!isInitialized) {
					int error = initializeDAQ();
					if (!error) {
						SetCtrlAttribute (panel, PANEL_STATUS, ATTR_CTRL_VAL, "Status: Initialized");
						SetCtrlAttribute (panel, PANEL_STATUS, ATTR_TEXT_COLOR, VAL_GREEN);
					}
					else {
						SetCtrlAttribute (panel, PANEL_STATUS, ATTR_CTRL_VAL, "Status: Failed to Initialize");
						SetCtrlAttribute (panel, PANEL_STATUS, ATTR_TEXT_COLOR, VAL_RED);
					}
				}
				
				// Start the task
				DebugPrintf("Requesting thread function from pool\n");
				int error = CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, collect, dataBuffer, &collectionThreadFnId);
				DebugPrintf("Thread function requested, continuing\n");
				if (!error) {
					// Change button text
					isRunning = true;
					SetCtrlAttribute(panel, PANEL_RUN, ATTR_LABEL_TEXT, "Stop Running");
				}
			}
			
			// Running -> stop running
			else {
				// Set stop running request under lock
				int lockObtained = 0;
				CmtGetLockEx(stopLockHandle, 1, CMT_WAIT_FOREVER, &lockObtained);
				if (lockObtained) {
					requestStopRunning = true;
					CmtReleaseLock(stopLockHandle);
				}
				
				// Change button text back
				isRunning = false;
				SetCtrlAttribute(panel, PANEL_RUN, ATTR_LABEL_TEXT, "Run");
			}

			break;
	}
	return 0;
}
