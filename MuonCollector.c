//==============================================================================
//
// Title:		MuonCollector
// Purpose:		Collects muons.
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
#include <time.h>

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

static uint32_t edgeCount = 0;
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

/// Clear tasks and quit
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

//==============================================================================
// UI setters

/// Sets panel control with ID `controlId` to text value
void setText(int controlId, char text[]) {
	SetCtrlAttribute(panelHandle, controlId, ATTR_CTRL_VAL, text);
}

/// Sets panel button control with ID `controlId` to text value
void setButtonText(int controlId, char text[]) {
	SetCtrlAttribute(panelHandle, controlId, ATTR_LABEL_TEXT, text);
}

/// Sets panel control with ID `controlId` to color value
void setColor(int controlId, int color) {
	SetCtrlAttribute(panelHandle, PANEL_STATUS, ATTR_TEXT_COLOR, color);
}

/// Update plot from buffer
void updatePlot(int dataBuffer[]) {
	// Plot
	if (!plotHandle) {
		plotHandle = PlotY(panelHandle, PANEL_GRAPH, dataBuffer, BUFFER_SIZE, VAL_SHORT_INTEGER, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
	}
	else {
		SetPlotAttribute(panelHandle, PANEL_GRAPH, plotHandle, ATTR_PLOT_YDATA, (void*)dataBuffer);
	}
}

//==============================================================================
// UI getters

int getCountType() {
	int countType = 0;
	GetCtrlVal(panelHandle, PANEL_COUNT_TYPE, &countType);
	return countType;
}

//==============================================================================
// Data saving functions

void writeDataToFile() {
	// Write data to file
}



//==============================================================================
// Data functions

int initializeDAQ() {
	int error = 0;
	// Initialize task handle
	error = error || DAQmxCreateTask("Muon Collection", &collectionTask);
	// Create channel to count number of rising edges
	error = error || DAQmxCreateCICountEdgesChan(collectionTask, "/Dev1/ctr1", "Rising Edge Counter", DAQmx_Val_Rising, 0, DAQmx_Val_CountUp);
	// Create channel to count seperation between rising edges
	error = error || DAQmxCreateCITwoEdgeSepChan (collectionTask, "/Dev1/ctr0", "Edge Seperation", MIN_EXPECTED_EDGE_SEP, MAX_EXPECTED_EDGE_SEP, DAQmx_Val_Seconds, DAQmx_Val_Rising, DAQmx_Val_Rising, NULL);
	// Configure sample clock timing. PFI3 is default for CTR1.
	error = error || DAQmxCfgSampClkTiming(collectionTask, "/Dev1/PFI3", SAMPLING_RATE, DAQmx_Val_Rising, DAQmx_Val_ContSamps, BUFFER_SIZE);
	if (!error) {
		isInitialized = true;
	}
	return error;
}

void collectEdgeSeperations(int dataBuffer[]) {
	// Start task
	DebugPrintf("Starting collection in thread %i\n", CmtGetCurrentThreadID());
	DAQmxStartTask(collectionTask);
	int read = 0;
	bool done = false;
	float64 count = 0;
	while(!done) {
		DAQmxReadCounterScalarF64(collectionTask, 1200, &count, NULL);
		DebugPrintf("Read %i samples\n", read);
		plot(dataBuffer);
		
		// Check if we should stop
		int lockObtained = 0;
		CmtGetLockEx(stopLockHandle, 1, CMT_WAIT_FOREVER, &lockObtained);
		if (lockObtained) {
			DebugPrintf("Obtained lock to check stop request\n");
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

void recordCountsPerMin(void *_) {
	// Start task
	DebugPrintf("Starting collection in thread %i\n", CmtGetCurrentThreadID());
	DAQmxStartTask(collectionTask);
	bool done = false;
	
	uint32_t count = 0;
	
	unsigned long startTime = time(NULL);
	unsigned long currentTime = time(NULL);
	
	double countsPerMinute = 0.00;
	
	while (!done) {
		// Get counter count
		DAQmxReadCounterScalarU32(collectionTask, 60, &count, NULL);
		
		// Get current time
		currentTime = time(NULL);

		// Reset and update every minute
		if (currentTime - startTime > 60) {
			unsigned long delta = currentTime - startTime;
			if (delta != 0) {
				countsPerMinute = count / (long double)delta;
			}
			// Update count display
			char displayText[1000];
			sprintf(displayText, "COUNTS/MIN: %d", count);
			SetCtrlAttribute(panelHandle, PANEL_COUNT_DISPLAY, ATTR_CTRL_VAL, displayText);
			// Reset counter
			count = 0;
			startTime = time(NULL);
		}
		
		// Check if we should stop
		int lockObtained = 0;
		CmtGetLockEx(stopLockHandle, 1, CMT_WAIT_FOREVER, &lockObtained);
		if (lockObtained) {
			done = requestStopRunning;
			if (done) {DebugPrintf("Received request to stop running early\n");}
			CmtReleaseLock(stopLockHandle);
		}
	}

	// Stop task
	DebugPrintf("Stopping collection task\n");
	DAQmxStopTask(collectionTask);
}


//==============================================================================
// UI callback functions

/// Starts collection
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
					// Set status indicator
					if (!error) {
						setText(PANEL_STATUS, "Status: Initialized");
						setColor(PANEL_STATUS, VAL_GREEN);
					}
					else {
						setText(PANEL_STATUS, "Status: Failed to Initialize");
						setColor(PANEL_STATUS, VAL_RED);
					}
				}
				
				// Schedule the task to begin in a thread pool
				int error = CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, recordCountsPerMin, NULL, &collectionThreadFnId);
				
				// Change button text
				if (!error) {
					isRunning = true;
					setButtonText(PANEL_RUN, "Stop Running");
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
				setButtonText(PANEL_RUN, "Run");
			}

			break;
	}
	return 0;
}
