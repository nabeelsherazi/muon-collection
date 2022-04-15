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

#include <formatio.h>
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
#define BUFFER_SIZE 1024
#define TIMEOUT 120
#define DEFAULT_MIN_EXPECTED_EDGE_SEP 5E-7 // sec
#define DEFAULT_MAX_EXPECTED_EDGE_SEP 1E-5 // sec
#define CHECKPOINT_FREQUENCY 1 // decays
#define DECAYS_TO_COLLECT 1000 // decays

//==============================================================================
// Types

struct DecayRecord {
	double Timestamp;
	float64 Lifetime;
};

enum TaskType {RecordMuons = 0, CalibrateScintillators = 1};

//==============================================================================
// Static global variables

// Handles
static int panelHandle = 0;
static TaskHandle collectionTask;
static int plotHandle;

// State
static bool isInitialized = false;
static bool isRunning = false;

// Thread handle
static CmtThreadFunctionID collectionThreadFnId;

// Thread control variable (and its lock)
static CmtThreadLockHandle stopLockHandle;
static bool requestStopRunning = false;

// Collection parameters (updated by initialize)
double minSeperation = DEFAULT_MIN_EXPECTED_EDGE_SEP;
double maxSeperation = DEFAULT_MAX_EXPECTED_EDGE_SEP;

// Data to be collected
static struct DecayRecord dataBuffer[BUFFER_SIZE];
static int numDecays = 0;
static int numCoincidentPulses = 0;

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
	Timer();
	
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

/// Sets panel text control with ID `controlId` to text value
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

/// Sets LED
void setLed(int controlId, bool on) {
	
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

/// Get the desired task
int getTaskSelection() {
	int taskType = 0;
	GetCtrlVal(panelHandle, PANEL_TASK_SELECTOR, &taskType);
	switch (taskType) {
		case 0:
			return RecordMuons;
		case 1:
			return CalibrateScintillators;
	}
}

double getDouble(int controlId) {
	double value = 0.00;
	GetCtrlVal(panelHandle, controlId, &value);
	return value;
}

void setDim(int controlId, bool dimmed) {
	SetCtrlAttribute(panelHandle, controlId, ATTR_DIMMED, dimmed);
}

//==============================================================================
// Data saving functions

/// Writes decay data recorded to file
void writeDataToFile() {
	// Generate filename
	static int checkpoint_num = 0;
	char pathname[MAX_FILENAME_LEN];
	sprintf(pathname, "checkpoint_%i.txt", checkpoint_num++);
	
	// Open file handle
	int fh = OpenFile(pathname, VAL_READ_WRITE, VAL_TRUNCATE, VAL_ASCII);
	int numBytesWritten = 0;
	
	// Line buffer
	char lineBuffer[1024];
	
	// Write preamble
	sprintf(lineBuffer, "Checkpoint written at: %lf", Timer());
	numBytesWritten += WriteLine(fh, lineBuffer, -1);
	FillBytes(lineBuffer, 0, 1024, 0);
	
	sprintf(lineBuffer, "Number of coincident pulses: %i", numCoincidentPulses);
	numBytesWritten += WriteLine(fh, lineBuffer, -1);
	FillBytes(lineBuffer, 0, 1024, 0);
	
	sprintf(lineBuffer, "Number of decays: %i", numDecays);
	numBytesWritten += WriteLine(fh, lineBuffer, -1);
	FillBytes(lineBuffer, 0, 1024, 0);
	
	// Blank line
	numBytesWritten += WriteLine(fh, "", -1);
	
	// Data header
	sprintf(lineBuffer, "timestamp,seperation\n");
	numBytesWritten += WriteLine(fh, lineBuffer, -1);
	FillBytes(lineBuffer, 0, 1024, 0);

	for (int i = 0; i < numDecays; i++) {
		struct DecayRecord record = dataBuffer[i];
		sprintf(lineBuffer, "%.*e,%.*e", DECIMAL_DIG, record.Timestamp, record.Lifetime);
		numBytesWritten += WriteLine(fh, lineBuffer, -1);
		FillBytes(lineBuffer, 0, 1024, 0);
	}
	
	DebugPrintf("Wrote %i lines to file\n", numBytesWritten);
	// Close file handle
	CloseFile(fh);
	return;
}



//==============================================================================
// Data functions

int initializeDAQ(enum TaskType task) {
	int error = 0;
	// Initialize task handle
	error = error || DAQmxCreateTask("Muon Collection", &collectionTask);
	// Create channel to count number of rising edges
	if (task == CalibrateScintillators) {
		DebugPrintf("Initializing calibration task\n");
		error = error || DAQmxCreateCICountEdgesChan(collectionTask, "/Dev1/ctr0", "Rising Edge Counter", DAQmx_Val_Rising, 0, DAQmx_Val_CountUp);
	}
	// Create channel to count seperation between rising edges
	else if (task == RecordMuons) {
		// Get params
		minSeperation = getDouble(PANEL_MUON_MIN_THRESHOLD);
		maxSeperation = getDouble(PANEL_MUON_MAX_THRESHOLD);
		// Dim out to prevent changes
		setDim(PANEL_MUON_MIN_THRESHOLD, true);
		setDim(PANEL_MUON_MAX_THRESHOLD, true);
		DebugPrintf("Initializing decay recording task\n");
		error = error || DAQmxCreateCITwoEdgeSepChan (collectionTask, "/Dev1/ctr0", "Edge Seperation", minSeperation, maxSeperation, DAQmx_Val_Seconds, DAQmx_Val_Rising, DAQmx_Val_Rising, NULL);
	}
	if (!error) {
		isInitialized = true;
	}
	return error;
}

void recordMuonDecays() {
	DebugPrintf("Starting collection in thread %i\n", CmtGetCurrentThreadID());
	
	// Start task
	DAQmxStartTask(collectionTask);
	
	bool done = false;
	// Data written to here
	float64 seperation = 0.00;
	double timeStamp = 0.00;
	
	while (!done) {
		DAQmxReadCounterScalarF64(collectionTask, TIMEOUT, &seperation, NULL);

		// Mark coincident pulse
		numCoincidentPulses++;
		
		// Check if this seperation is a decay
		if (seperation < maxSeperation && seperation > minSeperation) {
			timeStamp = Timer();
			DebugPrintf("(%.2lf) Detected decay of seperation: %lf", timeStamp, seperation);
			struct DecayRecord record;
			record.Lifetime = seperation;
			record.Timestamp = timeStamp;
			dataBuffer[numDecays] = record;
			numDecays++;
			// If checkpoint, write out data
			if (numDecays % CHECKPOINT_FREQUENCY == 0) {
				writeDataToFile();
			}
			// If collected required number of decays, done
			if (numDecays >= NUM
		}
		
		// Update plot
		//updatePlot(dataBuffer);
		
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
	
	// Dim back on
	setDim(PANEL_MUON_MIN_THRESHOLD, false);
	setDim(PANEL_MUON_MAX_THRESHOLD, false);
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
			//char displayText[1000];
			//sprintf(displayText, "COUNTS/MIN: %d", count);
			//SetCtrlAttribute(panelHandle, PANEL_COUNT_DISPLAY, ATTR_CTRL_VAL, displayText);
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

void updateTimeDisplay() {
	DebugPrintf("Running timer in thread %i\n", CmtGetCurrentThreadID());
	bool done = false;
	while (!done) {
		char time[128];
		sprintf(time, "%.1lf", Timer());
		setText(PANEL_RUN_TIME_DISPLAY, time);
		Delay(0.1);
		// Check if we should stop
		int lockObtained = 0;
		CmtGetLockEx(stopLockHandle, 1, CMT_WAIT_FOREVER, &lockObtained);
		if (lockObtained) {
			done = requestStopRunning;
			CmtReleaseLock(stopLockHandle);
		}
	}
}



//==============================================================================
// UI callback functions

/// Starts collection
int CVICALLBACK doRun(int panel, int control, int event,
					   void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
			
			// Not running -> start running
			if (!isRunning) {
				enum TaskType requestedTask = getTaskSelection();
				// Initialize the task if needed
				if (!isInitialized) {
					int error = initializeDAQ(requestedTask);
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
				int error = 0;
				requestStopRunning = false;
				if (requestedTask == CalibrateScintillators) {
					error = error | CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, recordCountsPerMin, NULL, &collectionThreadFnId);
				}
				else if (requestedTask == RecordMuons) {
					error = error | CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, recordMuonDecays, NULL, &collectionThreadFnId);
				}
				
				// Start timer update task
				error = error | CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, updateTimeDisplay, NULL, NULL);
				
				// Change button text and prevent changes to parameters
				if (!error) {
					isRunning = true;
					setDim(PANEL_TASK_SELECTOR, true);
					setDim(PANEL_MUON_MIN_THRESHOLD, true);
					setDim(PANEL_MUON_MAX_THRESHOLD, true);
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
				
				// Wait until the task is done
				DAQmxWaitUntilTaskDone (collectionTask, 10.0);
				
				// Change button text back and reenable changes
				isRunning = false;
				setDim(PANEL_TASK_SELECTOR, false);
				setDim(PANEL_MUON_MIN_THRESHOLD, false);
				setDim(PANEL_MUON_MAX_THRESHOLD, false);
				setButtonText(PANEL_RUN, "Run");
			}

			break;
	}
	return 0;
}

int CVICALLBACK onTaskChange (int panel, int control, int event,
							  void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
			if (isInitialized) {
				DAQmxClearTask(collectionTask);
				isInitialized = false;
			}
			break;
	}
	return 0;
}
