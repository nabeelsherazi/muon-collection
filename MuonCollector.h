/**************************************************************************/
/* LabWindows/CVI User Interface Resource (UIR) Include File              */
/*                                                                        */
/* WARNING: Do not add to, delete from, or otherwise modify the contents  */
/*          of this include file.                                         */
/**************************************************************************/

#include <userint.h>

#ifdef __cplusplus
    extern "C" {
#endif

     /* Panels and Controls: */

#define  PANEL                            1       /* callback function: panelCB */
#define  PANEL_QUIT                       2       /* control type: command, callback function: bye */
#define  PANEL_RUN                        3       /* control type: command, callback function: doRun */
#define  PANEL_SOURCE_SELECTOR            4       /* control type: slide, callback function: (none) */
#define  PANEL_STATUS                     5       /* control type: textMsg, callback function: (none) */
#define  PANEL_GRAPH                      6       /* control type: graph, callback function: (none) */
#define  PANEL_COUNT_DISPLAY              7       /* control type: textMsg, callback function: (none) */
#define  PANEL_TIMER                      8       /* control type: textMsg, callback function: (none) */


     /* Control Arrays: */

          /* (no control arrays in the resource file) */


     /* Menu Bars, Menus, and Menu Items: */

          /* (no menu bars in the resource file) */


     /* Callback Prototypes: */

int  CVICALLBACK bye(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK doRun(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK panelCB(int panel, int event, void *callbackData, int eventData1, int eventData2);


#ifdef __cplusplus
    }
#endif