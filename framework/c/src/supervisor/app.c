//--------------------------------------------------------------------------------------------------
/** @file supervisor/app.c
 *
 * This is the application class that references applications the Supervisor creates/starts/etc.
 * This application class contains all the processes that belong to this application.
 *
 * Copyright (C) Sierra Wireless Inc. Use of this work is subject to license.
 */

#include "legato.h"
#include "watchdogAction.h"
#include "app.h"
#include "limit.h"
#include "proc.h"
#include "user.h"
#include "le_cfg_interface.h"
#include "sandbox.h"
#include "resourceLimits.h"
#include "smack.h"
#include "cgroups.h"
#include "killProc.h"
#include "interfaces.h"


//--------------------------------------------------------------------------------------------------
/**
 * The location where all applications are installed.
 */
//--------------------------------------------------------------------------------------------------
#define APPS_INSTALL_DIR                                "/opt/legato/apps"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that specifies whether the app should be in a sandbox.
 *
 * If this entry in the config tree is missing or empty the application will be sandboxed.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_SANDBOXED                              "sandboxed"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains a process's supplementary groups list.
 *
 * Supplementary groups list is only available for non-sandboxed apps.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_GROUPS                                 "groups"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of processes for the application.
 *
 * If this entry in the config tree is missing or empty the application will not be launched.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_PROC_LIST                              "procs"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of bindings for the application.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_BINDINGS                               "bindings"


//--------------------------------------------------------------------------------------------------
/**
 * Timeout value for killing processes in an app.
 */
//--------------------------------------------------------------------------------------------------
static const le_clk_Time_t KillTimeout = {
        .sec = 0,
        .usec = 300000,
};


//--------------------------------------------------------------------------------------------------
/**
 * The application object.
 */
//--------------------------------------------------------------------------------------------------
typedef struct app_Ref
{
    char*           name;                               // The name of the application.
    char            cfgPathRoot[LIMIT_MAX_PATH_BYTES];  // Our path in the config tree.
    bool            sandboxed;                          // true if this is a sandboxed app.
    char            installPath[LIMIT_MAX_PATH_BYTES];  // The app's install directory path.
    char            sandboxPath[LIMIT_MAX_PATH_BYTES];  // The app's sandbox path.
    uid_t           uid;                // The user ID for this application.
    gid_t           gid;                // The group ID for this application.
    gid_t           supplementGids[LIMIT_MAX_NUM_SUPPLEMENTARY_GROUPS];  // List of supplementary
                                                                         // group IDs.
    size_t          numSupplementGids;  // The number of supplementary groups for this app.
    app_State_t     state;              // The applications current state.
    le_dls_List_t   procs;              // The list of processes in this application.
    le_timer_Ref_t  killTimer;          // Timeout timer for killing processes.
}
App_t;


//--------------------------------------------------------------------------------------------------
/**
 * The memory pool for application objects.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AppPool;

//--------------------------------------------------------------------------------------------------
/**
 * Application object reference.  Incomplete type so that we can have the application object
 * reference the AppStopHandler_t.
 */
//--------------------------------------------------------------------------------------------------
typedef struct _procObjRef* ProcObjRef_t;

//--------------------------------------------------------------------------------------------------
/**
 * Prototype for process stopped handler.
 */
//--------------------------------------------------------------------------------------------------
typedef le_result_t (*ProcStopHandler_t)
(
    app_Ref_t appRef,                   ///< [IN] The application containing the stopped process
    proc_Ref_t procRef                  ///< [IN] The stopped process
);

//--------------------------------------------------------------------------------------------------
/**
 * The process object.
 */
//--------------------------------------------------------------------------------------------------
typedef struct _procObjRef
{
    proc_Ref_t      procRef;        // The process reference.
    ProcStopHandler_t stopHandler;    // Handler function that gets called when this process stops.
    le_dls_Link_t   link;           // The link in the application's list of processes.
}
ProcObj_t;

//--------------------------------------------------------------------------------------------------
/**
 * The memory pool for process objects.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t ProcObjPool;


//--------------------------------------------------------------------------------------------------
/**
 * The file that stores the application reboot fault record.  When the system reboots due to an
 * application fault the applications and process names are stored here.
 */
//--------------------------------------------------------------------------------------------------
#define REBOOT_FAULT_RECORD                 "/opt/legato/appRebootFault"


//--------------------------------------------------------------------------------------------------
/**
 * The fault limits.
 *
 * @todo Put in the config tree so that it can be configured.
 */
//--------------------------------------------------------------------------------------------------
#define FAULT_LIMIT_INTERVAL_RESTART                10   // in seconds
#define FAULT_LIMIT_INTERVAL_RESTART_APP            10   // in seconds
#define FAULT_LIMIT_INTERVAL_REBOOT                 120  // in seconds


//--------------------------------------------------------------------------------------------------
/**
 * Application kill type.
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    KILL_SOFT,          ///< Requests the application to clean up and shutdown.
    KILL_HARD           ///< Kills the application ASAP.
}
KillType_t;


//--------------------------------------------------------------------------------------------------
/**
 * The reboot fault timer handler.  When this expires we delete the reboot fault record so that
 * reboot faults will reach the fault limit only if there is a fault that reboots the system before
 * this timer expires.
 */
//--------------------------------------------------------------------------------------------------
static void RebootFaultTimerHandler
(
    le_timer_Ref_t timerRef
)
{
    if ( (unlink(REBOOT_FAULT_RECORD) == -1) && (errno != ENOENT) )
    {
        LE_ERROR("Could not delete reboot fault record.  %m.  This could result in the fault limit \
being reached incorrectly when a process faults and resets the system.");
    }

    le_timer_Delete(timerRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Initialize the application system.
 */
//--------------------------------------------------------------------------------------------------
void app_Init
(
    void
)
{
    AppPool = le_mem_CreatePool("Apps", sizeof(App_t));
    ProcObjPool = le_mem_CreatePool("ProcObj", sizeof(ProcObj_t));

    // Start the reboot fault timer.
    le_timer_Ref_t rebootFaultTimer = le_timer_Create("RebootFault");
    le_clk_Time_t rebootFaultInterval = {.sec = FAULT_LIMIT_INTERVAL_REBOOT};

    if ( (le_timer_SetHandler(rebootFaultTimer, RebootFaultTimerHandler) != LE_OK) ||
         (le_timer_SetInterval(rebootFaultTimer, rebootFaultInterval) != LE_OK) ||
         (le_timer_Start(rebootFaultTimer) != LE_OK) )
    {
        LE_ERROR("Could not start the reboot fault timer.  This could result in the fault limit \
being reached incorrectly when a process faults and resets the system.");
    }

    proc_Init();
}


//--------------------------------------------------------------------------------------------------
/**
 * Create the supplementary groups for an application.
 *
 * @todo Move creation of the groups to the installer.  Make this function just read the groups
 *       list into the app object.
 **/
//--------------------------------------------------------------------------------------------------
static le_result_t CreateSupplementaryGroups
(
    app_Ref_t appRef        // The app to create groups for.
)
{
    // Get an iterator to the supplementary groups list in the config.
    le_cfg_IteratorRef_t cfgIter = le_cfg_CreateReadTxn(appRef->cfgPathRoot);

    le_cfg_GoToNode(cfgIter, CFG_NODE_GROUPS);

    if (le_cfg_GoToFirstChild(cfgIter) != LE_OK)
    {
        LE_DEBUG("No supplementary groups for app '%s'.", appRef->name);
        le_cfg_CancelTxn(cfgIter);

        return LE_OK;
    }

    // Read the supplementary group names from the config.
    size_t i;
    for (i = 0; i < LIMIT_MAX_NUM_SUPPLEMENTARY_GROUPS; i++)
    {
        // Read the supplementary group name from the config.
        char groupName[LIMIT_MAX_USER_NAME_BYTES];
        if (le_cfg_GetNodeName(cfgIter, "", groupName, sizeof(groupName)) != LE_OK)
        {
            LE_ERROR("Could not read supplementary group for app '%s'.", appRef->name);
            le_cfg_CancelTxn(cfgIter);
            return LE_FAULT;
        }

        // Create the group.
        gid_t gid;
        if (user_CreateGroup(groupName, &gid) == LE_FAULT)
        {
            LE_ERROR("Could not create supplementary group '%s'.", groupName);
            le_cfg_CancelTxn(cfgIter);
            return LE_FAULT;
        }

        // Store the group id in the user's buffer.
        appRef->supplementGids[i] = gid;

        // Go to the next group.
        if (le_cfg_GoToNextSibling(cfgIter) != LE_OK)
        {
            break;
        }
        else if (i >= LIMIT_MAX_NUM_SUPPLEMENTARY_GROUPS - 1)
        {
            LE_ERROR("Too many supplementary groups for app '%s'.", appRef->name);
            le_cfg_CancelTxn(cfgIter);
            return LE_FAULT;
        }
    }

    appRef->numSupplementGids = i + 1;

    le_cfg_CancelTxn(cfgIter);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates the user and groups in the /etc/passwd and /etc/groups files for an application.  This
 * function sets the uid and primary gid for the appRef and also populates the appRef's
 * supplementary groups list for non-sandboxed apps.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateUserAndGroups
(
    app_Ref_t appRef        // The app to create user and groups for.
)
{
    // For sandboxed apps,
    if (appRef->sandboxed)
    {
        // Compute the unique user name for the application.
        char username[LIMIT_MAX_USER_NAME_BYTES];

        if (user_AppNameToUserName(appRef->name, username, sizeof(username)) != LE_OK)
        {
            LE_ERROR("The user name '%s' is too long.", username);
            return LE_FAULT;
        }

        // Get the user ID and primary group ID for this app.
        if (user_GetIDs(username, &(appRef->uid), &(appRef->gid)) != LE_OK)
        {
            LE_ERROR("Could not get uid and gid for user '%s'.", username);
            return LE_FAULT;
        }

        // Create the supplementary groups...
        return CreateSupplementaryGroups(appRef);
    }
    // For unsandboxed apps,
    else
    {
        // The user and group will be "root" (0).
        appRef->uid = 0;
        appRef->gid = 0;

        return LE_OK;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates an application object.
 *
 * @note
 *      The name of the application is the node name (last part) of the cfgPathRootPtr.
 *
 * @return
 *      A reference to the application object if success.
 *      NULL if there was an error.
 */
//--------------------------------------------------------------------------------------------------
app_Ref_t app_Create
(
    const char* cfgPathRootPtr      ///< [IN] The path in the config tree for this application.
)
{
    // Create a new app object.
    App_t* appPtr = le_mem_ForceAlloc(AppPool);

    // Save the config path.
    if (le_utf8_Copy(appPtr->cfgPathRoot, cfgPathRootPtr, sizeof(appPtr->cfgPathRoot), NULL) != LE_OK)
    {
        LE_ERROR("Config path '%s' is too long.", cfgPathRootPtr);

        le_mem_Release(appPtr);
        return NULL;
    }

    // Store the app name.
    appPtr->name = le_path_GetBasenamePtr(appPtr->cfgPathRoot, "/");

    // Initialize the other parameters.
    appPtr->procs = LE_DLS_LIST_INIT;
    appPtr->state = APP_STATE_STOPPED;
    appPtr->killTimer = NULL;

    // Get a config iterator for this app.
    le_cfg_IteratorRef_t cfgIterator = le_cfg_CreateReadTxn(appPtr->cfgPathRoot);

    // See if this is a sandboxed app.
    appPtr->sandboxed = le_cfg_GetBool(cfgIterator, CFG_NODE_SANDBOXED, true);

    // @todo: Create the user and all the groups for this app.  This function has a side affect
    //        where it populates the app's supplementary groups list and sets the uid and the
    //        primary gid.  This behaviour will be changed when the create user functionality is
    //        moved to the app installer.
    if (CreateUserAndGroups(appPtr) != LE_OK)
    {
        le_mem_Release(appPtr);
        le_cfg_CancelTxn(cfgIterator);
        return NULL;
    }

    // Get the app's install directory path.
    appPtr->installPath[0] = '\0';
    if (LE_OK != le_path_Concat("/",
                                appPtr->installPath,
                                sizeof(appPtr->installPath),
                                APPS_INSTALL_DIR,
                                appPtr->name,
                                NULL))
    {
        LE_ERROR("Install directory path '%s' is too long.  Application '%s' cannot be started.",
                 appPtr->installPath,
                 appPtr->name);

        le_mem_Release(appPtr);
        le_cfg_CancelTxn(cfgIterator);
        return NULL;
    }

    // Get the app's sandbox path.
    if (appPtr->sandboxed)
    {
        if (sandbox_GetPath(appPtr->name, appPtr->sandboxPath, sizeof(appPtr->sandboxPath)) != LE_OK)
        {
            LE_ERROR("The application's sandbox path '%s' is too long.  Application '%s' cannot be started.",
                     appPtr->sandboxPath, appPtr->name);

            le_mem_Release(appPtr);
            le_cfg_CancelTxn(cfgIterator);
            return NULL;
        }
    }
    else
    {
        appPtr->sandboxPath[0] = '\0';
    }

    // Move the config iterator to the procs list for this app.
    le_cfg_GoToNode(cfgIterator, CFG_NODE_PROC_LIST);

    // Read the list of processes for this application from the config tree.
    if (le_cfg_GoToFirstChild(cfgIterator) == LE_OK)
    {
        do
        {
            // Get the process's config path.
            char procCfgPath[LIMIT_MAX_PATH_BYTES];

            if (le_cfg_GetPath(cfgIterator, "", procCfgPath, sizeof(procCfgPath)) == LE_OVERFLOW)
            {
                LE_ERROR("Internal path buffer too small.");
                app_Delete(appPtr);
                le_cfg_CancelTxn(cfgIterator);
                return NULL;
            }

            // Strip off the trailing '/'.
            size_t lastIndex = le_utf8_NumBytes(procCfgPath) - 1;

            if (procCfgPath[lastIndex] == '/')
            {
                procCfgPath[lastIndex] = '\0';
            }

            // Create the process.
            proc_Ref_t procPtr;
            if ((procPtr = proc_Create(procCfgPath, appPtr)) == NULL)
            {
                app_Delete(appPtr);
                le_cfg_CancelTxn(cfgIterator);
                return NULL;
            }

            // Add the process to the app's process list.
            ProcObj_t* procObjPtr = le_mem_ForceAlloc(ProcObjPool);
            procObjPtr->procRef = procPtr;
            procObjPtr->stopHandler = NULL;
            procObjPtr->link = LE_DLS_LINK_INIT;

            le_dls_Queue(&(appPtr->procs), &(procObjPtr->link));
        }
        while (le_cfg_GoToNextSibling(cfgIterator) == LE_OK);
    }

    le_cfg_CancelTxn(cfgIterator);

    return appPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes an application.  The application must be stopped before it is deleted.
 *
 * @note If this function fails it will kill the calling process.
 */
//--------------------------------------------------------------------------------------------------
void app_Delete
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to delete.
)
{
    // Pop all the processes off the app's list and free them.
    le_dls_Link_t* procLinkPtr = le_dls_Pop(&(appRef->procs));

    while (procLinkPtr != NULL)
    {
        ProcObj_t* procPtr = CONTAINER_OF(procLinkPtr, ProcObj_t, link);

        proc_Delete(procPtr->procRef);
        le_mem_Release(procPtr);

        procLinkPtr = le_dls_Pop(&(appRef->procs));
    }

    // Release the app timer.
    if (appRef->killTimer != NULL)
    {
        le_timer_Delete(appRef->killTimer);
    }

    // Relesase app.
    le_mem_Release(appRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets SMACK rules for an application based on its bindings.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t SetSmackRulesForBindings
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application.
    const char* appLabelPtr             ///< [IN] Smack label for the app.
)
{
    // Create a config read transaction to the bindings section for the application.
    le_cfg_IteratorRef_t bindCfg = le_cfg_CreateReadTxn(appRef->cfgPathRoot);
    le_cfg_GoToNode(bindCfg, CFG_NODE_BINDINGS);

    // Search the binding sections for server applications we need to set rules for.
    if (le_cfg_GoToFirstChild(bindCfg) != LE_OK)
    {
        // No bindings.
        le_cfg_CancelTxn(bindCfg);
        return LE_OK;
    }

    do
    {
        char serverName[LIMIT_MAX_APP_NAME_BYTES];

        if ( (le_cfg_GetString(bindCfg, "app", serverName, sizeof(serverName), "") == LE_OK) &&
             (strcmp(serverName, "") != 0) )
        {
            // Get the server's SMACK label.
            char serverLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
            appSmack_GetLabel(serverName, serverLabel, sizeof(serverLabel));

            // Set the SMACK label to/from the server.
            smack_SetRule(appLabelPtr, "rw", serverLabel);
            smack_SetRule(serverLabel, "rw", appLabelPtr);
        }
    } while (le_cfg_GoToNextSibling(bindCfg) == LE_OK);

    le_cfg_CancelTxn(bindCfg);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets SMACK rules for an application and its folders.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t SetDefaultSmackRules
(
    const char* appNamePtr,             ///< [IN] App name.
    const char* appLabelPtr             ///< [IN] Smack label for the app.
)
{
#define NUM_PERMISSONS      7

    const char* permissionStr[NUM_PERMISSONS] = {"x", "w", "wx", "r", "rx", "rw", "rwx"};

    // Set the rules for the app to access its own folders.
    int i;

    for (i = 0; i < NUM_PERMISSONS; i++)
    {
        // Create the mode from the permissions.
        appSmack_AccessFlags_t mode = 0;

        if (strstr(permissionStr[i], "r") != NULL)
        {
            mode |= APPSMACK_ACCESS_FLAG_READ;
        }
        if (strstr(permissionStr[i], "w") != NULL)
        {
            mode |= APPSMACK_ACCESS_FLAG_WRITE;
        }
        if (strstr(permissionStr[i], "x") != NULL)
        {
            mode |= APPSMACK_ACCESS_FLAG_EXECUTE;
        }

        char dirLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
        appSmack_GetAccessLabel(appNamePtr, mode, dirLabel, sizeof(dirLabel));

        smack_SetRule(appLabelPtr, permissionStr[i], dirLabel);
    }

    // Set default permissions between the app and the framework.
    smack_SetRule("framework", "w", appLabelPtr);
    smack_SetRule(appLabelPtr, "rw", "framework");

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets SMACK rules for an application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t SetSmackRules
(
    app_Ref_t appRef                    ///< [IN] Reference to the application.
)
{
    // Get the app label.
    char appLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
    appSmack_GetLabel(appRef->name, appLabel, sizeof(appLabel));

    if (SetDefaultSmackRules(appRef->name, appLabel) != LE_OK)
    {
        return LE_FAULT;
    }

    return SetSmackRulesForBindings(appRef, appLabel);
}


//--------------------------------------------------------------------------------------------------
/**
 * Starts a process in an application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t StartProc
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    proc_Ref_t procRef                  ///< [IN] The process to start.
)
{
    le_result_t result;

    if (appRef->sandboxed)
    {
        result = proc_StartInSandbox(procRef,
                                     "/",
                                     appRef->uid,
                                     appRef->gid,
                                     appRef->supplementGids,
                                     appRef->numSupplementGids,
                                     appRef->sandboxPath);
    }
    else
    {
        result = proc_Start(procRef, appRef->installPath);
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Starts an application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_Start
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to start.
)
{
    if (appRef->state == APP_STATE_RUNNING)
    {
        LE_ERROR("Application '%s' is already running.", appRef->name);

        return LE_FAULT;
    }

    // If a sandboxed app,
    if (appRef->sandboxed)
    {
        // Create the sandboxed area.
        if (sandbox_Setup(appRef) != LE_OK)
        {
            LE_ERROR("Could not create sandbox for application '%s'.  This application cannot be started.",
                     appRef->name);

            return LE_FAULT;
        }
    }

    // Set the resource limit for this application.
    if (resLim_SetAppLimits(appRef) != LE_OK)
    {
        LE_ERROR("Could not set application resource limits.  Application %s cannot be started.",
                 appRef->name);
        return LE_FAULT;
    }

    // Set default SMACK rules for this app.
    if (SetSmackRules(appRef) != LE_OK)
    {
        return LE_FAULT;
    }

    // Start all the processes in the application.
    le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

    while (procLinkPtr != NULL)
    {
        ProcObj_t* procPtr = CONTAINER_OF(procLinkPtr, ProcObj_t, link);

        le_result_t result = StartProc(appRef, procPtr->procRef);

        if (result != LE_OK)
        {
            LE_ERROR("Could not start all application processes.  Stopping the application '%s'.",
                     appRef->name);

            app_Stop(appRef);

            return LE_FAULT;
        }

        // Get the next process.
        procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
    }

    appRef->state = APP_STATE_RUNNING;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Kills all the processes in the specified application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_NOT_FOUND if there are no running processes in the app.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t KillAppProcs
(
    app_Ref_t appRef,       ///< [IN] Reference to the application whose processes should be killed.
    KillType_t  killType    ///< [IN] The type of kill to perform.
)
{
    // Freeze app procs.
    if (cgrp_frz_Freeze(appRef->name) == LE_OK)
    {
        // Wait till procs are frozen.
        while (1)
        {
            cgrp_FreezeState_t freezeState = cgrp_frz_GetState(appRef->name);

            if (freezeState == CGRP_FROZEN)
            {
                break;
            }
            else if ((le_result_t)freezeState == LE_FAULT)
            {
                LE_ERROR("Could not get freeze state of application '%s'.", appRef->name);
                break;
            }
        }

        LE_DEBUG("App '%s' frozen.", appRef->name);
    }
    else
    {
        LE_ERROR("Could not freeze processes for application '%s'.", appRef->name);
    }

    // Tell the child process objects we are going to kill them.
    le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

    while (procLinkPtr != NULL)
    {
        ProcObj_t* procObjPtr = CONTAINER_OF(procLinkPtr, ProcObj_t, link);

        if (proc_GetState(procObjPtr->procRef) != PROC_STATE_STOPPED)
        {
            procObjPtr->stopHandler = NULL;
            proc_Stopping(procObjPtr->procRef);
        }

        procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
    }

    // Kill all procs in the app including child processes and forked processes.
    int killSig = (killType == KILL_SOFT) ? SIGTERM: SIGKILL;

    ssize_t numProcs = cgrp_SendSig(CGRP_SUBSYS_FREEZE, appRef->name, killSig);

    if (numProcs == LE_FAULT)
    {
        LE_ERROR("Could not kill processes for application '%s'.", appRef->name);
        return LE_NOT_FOUND;
    }
    else if (numProcs == 0)
    {
        return LE_NOT_FOUND;
    }

    // Thaw app procs to allow them to run and process the signal we sent them.
    if (cgrp_frz_Thaw(appRef->name) != LE_OK)
    {
        LE_ERROR("Could not thaw processes for application '%s'.", appRef->name);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Cleans up a stopped application's resources ie. sandbox, resource limits, etc.
 */
//--------------------------------------------------------------------------------------------------
static void CleanupApp
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    // Clean up SMACK rules.
    char appLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
    appSmack_GetLabel(appRef->name, appLabel, sizeof(appLabel));

    smack_RevokeSubject(appLabel);

    // Remove the sanbox.
    if (appRef->sandboxed)
    {
        if (sandbox_Remove(appRef) != LE_OK)
        {
            LE_CRIT("Could not remove sandbox for application '%s'.", appRef->name);
        }
    }

    // Remove the resource limits.
    resLim_CleanupApp(appRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Performs a hard kill of all the processes in the specified application.  This function should be
 * called when the soft kill timeout expires.
 */
//--------------------------------------------------------------------------------------------------
static void HardKillApp
(
    le_timer_Ref_t timerRef
)
{
    app_Ref_t appRef = (app_Ref_t)le_timer_GetContextPtr(timerRef);

    LE_WARN("Hard killing app '%s'", appRef->name);

    KillAppProcs(appRef, KILL_HARD);
}


//--------------------------------------------------------------------------------------------------
/**
 * Stops an application.  This is an asynchronous function call that returns immediately but
 * the application may not stop right away.  Check the application's state with app_GetState() to
 * see when the application actually stops.
 */
//--------------------------------------------------------------------------------------------------
void app_Stop
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to stop.
)
{
    if (appRef->state == APP_STATE_STOPPED)
    {
        LE_ERROR("Application '%s' is already stopped.", appRef->name);
        return;
    }

    // Soft kill all the processes in the app.
    if (KillAppProcs(appRef, KILL_SOFT) == LE_NOT_FOUND)
    {
        // There are no more running processes in the app.
        LE_INFO("app '%s' has stopped.", appRef->name);

        // Note the application is cleaned up here so if the app is restarted it will apply the new
        // config settings if the config has changed.
        CleanupApp(appRef);

        appRef->state = APP_STATE_STOPPED;
    }
    else
    {
        // Start the kill timeout timer for this app.
        if (appRef->killTimer == NULL)
        {
            char timerName[LIMIT_MAX_PATH_BYTES];

            snprintf(timerName, sizeof(timerName), "%s_Killer", appRef->name);
            appRef->killTimer = le_timer_Create(timerName);

            LE_ASSERT(le_timer_SetInterval(appRef->killTimer, KillTimeout) == LE_OK);

            LE_ASSERT(le_timer_SetContextPtr(appRef->killTimer, (void*)appRef) == LE_OK);
            LE_ASSERT(le_timer_SetHandler(appRef->killTimer, HardKillApp) == LE_OK);
        }

        le_timer_Start(appRef->killTimer);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's state.
 *
 * @return
 *      The application's state.
 */
//--------------------------------------------------------------------------------------------------
app_State_t app_GetState
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to stop.
)
{
    return appRef->state;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the state of a process belonging to an application.
 *
 * @return
 *      The process's state.
 */
//--------------------------------------------------------------------------------------------------
app_ProcState_t app_GetProcState
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application to stop.
    const char* procName                ///< [IN] Name of the process.
)
{
    if (appRef->state == APP_STATE_RUNNING)
    {
        // Find the process in the app's list.
        le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

        while (procLinkPtr != NULL)
        {
            ProcObj_t* procObjPtr = CONTAINER_OF(procLinkPtr, ProcObj_t, link);

            if (strcmp(procName, proc_GetName(procObjPtr->procRef)) == 0)
            {
                switch (proc_GetState(procObjPtr->procRef))
                {
                    case PROC_STATE_STOPPED:
                        return APP_PROC_STATE_STOPPED;

                    case PROC_STATE_RUNNING:
                        return APP_PROC_STATE_RUNNING;

                    case PROC_STATE_PAUSED:
                        return APP_PROC_STATE_PAUSED;

                    default:
                        LE_FATAL("Unrecognized process state.");
                }
            }

            procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
        }
    }

    return APP_PROC_STATE_STOPPED;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's name.
 *
 * @return
 *      The application's name.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetName
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return (const char*)appRef->name;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's UID.
 *
 * @return
 *      The application's UID.
 */
//--------------------------------------------------------------------------------------------------
uid_t app_GetUid
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return appRef->uid;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's GID.
 *
 * @return
 *      The application's GID.
 */
//--------------------------------------------------------------------------------------------------
gid_t app_GetGid
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return appRef->gid;
}


//--------------------------------------------------------------------------------------------------
/**
 * Check to see if the application is sandboxed or not.
 *
 * @return
 *      True if the app is sandboxed, false if not.
 */
//--------------------------------------------------------------------------------------------------
bool app_GetIsSandboxed
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return appRef->sandboxed;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's installation directory path.
 *
 * @return
 *      The application's install directory path.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetInstallDirPath
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return (const char*)appRef->installPath;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's sandbox path.
 *
 * @return
 *      The application's sandbox path.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetSandboxPath
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return (const char*)appRef->sandboxPath;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's configuration path.
 *
 * @return
 *      The application's configuration path.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetConfigPath
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return (const char*)appRef->cfgPathRoot;
}


//--------------------------------------------------------------------------------------------------
/**
 * Finds a process object for the app.
 *
 * @return
 *      The process object reference if successful.
 *      NULL if the process could not be found.
 */
//--------------------------------------------------------------------------------------------------
static ProcObjRef_t FindProcObjectRef
(
    app_Ref_t appRef,               ///< [IN] The application to search in.
    pid_t pid                       ///< [IN] The pid to search for.
)
{
    // Find the process in the app's list.
    le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

    while (procLinkPtr != NULL)
    {
        ProcObj_t* procObjPtr = CONTAINER_OF(procLinkPtr, ProcObj_t, link);

        if (proc_GetPID(procObjPtr->procRef) == pid)
        {
            return procObjPtr;
        }

        procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
    }

    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Write the reboot fault record for the application/process that experienced the fault and requires
 * a system reboot.
 *
 * @todo Write the record fault into the config tree when it is available.  This is just a temporary
 *       solution because the current config tree is non-persistent.
 */
//--------------------------------------------------------------------------------------------------
static void WriteRebootFaultRec
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    proc_Ref_t procRef                  ///< [IN] The process reference.
)
{
    // @note Don't really need to lock this file as no-one else really uses.  Using the le_flock
    //       API just cause it's easier to use then open() and this is a temporary location for the
    //       record fault anyways.
    int fd = le_flock_Create(REBOOT_FAULT_RECORD, LE_FLOCK_WRITE, LE_FLOCK_REPLACE_IF_EXIST, S_IRWXU);

    if (fd < 0)
    {
        LE_ERROR("Could not create reboot fault record.  The reboot fault limit will not \
be enforced correctly.");
        return;
    }

    char faultStr[LIMIT_MAX_PATH_BYTES];

    int faultStrSize = snprintf(faultStr, sizeof(faultStr), "%s/%s", appRef->name,
                                proc_GetName(procRef)) + 1;
    LE_ASSERT(faultStrSize <= sizeof(faultStr));

    int result;
    do
    {
        result = write(fd, faultStr, faultStrSize);
    }
    while ( (result == -1) && (errno == EINTR) );

    if (result == -1)
    {
        LE_ERROR("Could not write reboot fault record.  %m.");
    }
    else if (result != faultStrSize)
    {
        LE_ERROR("Could not write reboot fault record.  The reboot fault limit will not \
be enforced correctly.");
    }

    le_flock_Close(fd);
}


//--------------------------------------------------------------------------------------------------
/**
 * Check if the reboot fault record was created by the specified application/process.
 *
 * @return
 *      true if the reboot fault record was created by the specified app/process.
 *      false otherwise.
 */
//--------------------------------------------------------------------------------------------------
static bool isRebootFaultRecFor
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    proc_Ref_t procRef                  ///< [IN] The process reference.
)
{
    // This file does not really need to be locked as no one else uses it.  Also this should go into
    // the config tree when the config tree is available.
    int fd = le_flock_Open(REBOOT_FAULT_RECORD, LE_FLOCK_READ);

    if (fd == LE_NOT_FOUND)
    {
        return false;
    }
    else if (fd == LE_FAULT)
    {
        LE_ERROR("Could not open reboot fault record.  The reboot fault limit will not \
be enforced correctly.");
        return false;
    }

    // Read the record.
    char faultRec[LIMIT_MAX_PATH_BYTES] = {0};
    ssize_t count = read(fd, faultRec, sizeof(faultRec));

    le_flock_Close(fd);

    if (count == -1)
    {
        LE_ERROR("Could not read reboot fault record.  %m.  The reboot fault limit will not \
be enforced correctly.");
    }
    else if (count >= sizeof(faultRec))
    {
        LE_ERROR("Could not read reboot fault record.  The reboot fault limit will not \
be enforced correctly.");
    }
    else
    {
        // Add a null to the string read.
        faultRec[count] = '\0';

        // See if the reboot record is for this app/process.
        char faultStr[LIMIT_MAX_PATH_BYTES];
        LE_ASSERT(snprintf(faultStr, sizeof(faultStr), "%s/%s", appRef->name, proc_GetName(procRef)) <
                  sizeof(faultStr));

        if (strcmp(faultRec, faultStr) == 0)
        {
            return true;
        }
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Checks to see if the fault limit for this process has been reached.  The fault limit is reached
 * when there is more than one fault within the fault limit interval.
 *
 * @return
 *      true if the fault limit has been reached.
 *      false if the fault limit has not been reached.
 */
//--------------------------------------------------------------------------------------------------
static bool ReachedFaultLimit
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    proc_Ref_t procRef,                 ///< [IN] The process reference.
    proc_FaultAction_t currFaultAction, ///< [IN] The process's current fault action.
    time_t prevFaultTime                ///< [IN] Time of the previous fault.
)
{
    switch (currFaultAction)
    {
        case PROC_FAULT_ACTION_RESTART:
            if ( (proc_GetFaultTime(procRef) != 0) &&
                 (proc_GetFaultTime(procRef) - prevFaultTime <= FAULT_LIMIT_INTERVAL_RESTART) )
            {
                return true;
            }
            break;

        case PROC_FAULT_ACTION_RESTART_APP:
            if ( (proc_GetFaultTime(procRef) != 0) &&
                 (proc_GetFaultTime(procRef) - prevFaultTime <= FAULT_LIMIT_INTERVAL_RESTART_APP) )
            {
                return true;
            }
            break;

        case PROC_FAULT_ACTION_REBOOT:
            return isRebootFaultRecFor(appRef, procRef);

        default:
            // Fault limits do not apply to the other fault actions.
            return false;
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Checks if the application has any processes running.
 *
 * @note This only applies to child processes.  Forked processes in the application are not
 *       monitored.
 *
 * @return
 *      true if there is atleast one running process for the application.
 *      false if there are no running processes for the application.
 */
//--------------------------------------------------------------------------------------------------
static bool HasRunningProc
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return !cgrp_IsEmpty(CGRP_SUBSYS_FREEZE, appRef->name);
}


//--------------------------------------------------------------------------------------------------
/**
 * Stops the specified process.
 */
//--------------------------------------------------------------------------------------------------
static void StopProc
(
    proc_Ref_t procRef
)
{
    proc_Stopping(procRef);

    pid_t pid = proc_GetPID(procRef);

    kill_Hard(pid);
}


//--------------------------------------------------------------------------------------------------
/**
 * This handler must be called when the watchdog expires for a process that belongs to the
 * specified application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_NOT_FOUND if the procPid was not found for the specified app.
 *
 *      The watchdog action passed in will be set to the action that should be taken for
 *      this process or one of the following:
 *          WATCHDOG_ACTION_NOT_FOUND - no action was configured for this process
 *          WATCHDOG_ACTION_ERROR     - the action could not be read or is unknown
 *          WATCHDOG_ACTION_HANDLED   - no further action is required, it is already handled.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_WatchdogTimeoutHandler
(
    app_Ref_t appRef,                                 ///< [IN] The application reference.
    pid_t procPid,                                    ///< [IN] The pid of the process that changed
                                                      ///< state.
    wdog_action_WatchdogAction_t* watchdogActionPtr   ///< [OUT] The fault action that should be
                                                      ///< taken.
)
{
    LE_FATAL_IF(appRef == NULL, "appRef is NULL");

    ProcObjRef_t procObjectRef = FindProcObjectRef(appRef, procPid);

    if (procObjectRef == NULL)
    {
        return LE_NOT_FOUND;
    }

    proc_Ref_t procRef = procObjectRef->procRef;

    // Get the current process fault action.
    wdog_action_WatchdogAction_t watchdogAction = proc_GetWatchdogAction(procRef);

    // If WATCHDOG_ACTION_ERROR, we have reported the error already in proc. Let's give ourselves a
    // second chance and see if we can find a good value at app level.
    if (WATCHDOG_ACTION_NOT_FOUND == watchdogAction || WATCHDOG_ACTION_ERROR == watchdogAction)
    {
        // No action was defined for the proc. See if there is one for the app.
        // Read the app's watchdog action from the config tree.
        le_cfg_IteratorRef_t appCfg = le_cfg_CreateReadTxn(appRef->cfgPathRoot);

        char watchdogActionStr[LIMIT_MAX_FAULT_ACTION_NAME_BYTES];
        le_result_t result = le_cfg_GetString(appCfg, wdog_action_GetConfigNode(),
                watchdogActionStr, sizeof(watchdogActionStr), "");

        le_cfg_CancelTxn(appCfg);

        // Set the watchdog action based on the watchdog action string.
        if (result == LE_OK)
        {
            LE_DEBUG("%s watchdogAction '%s' in app section", appRef->name, watchdogActionStr);
            watchdogAction = wdog_action_EnumFromString(watchdogActionStr);
            if (WATCHDOG_ACTION_ERROR == watchdogAction)
            {
                LE_WARN("%s watchdog Action %s unknown", appRef->name, watchdogActionStr);
            }
        }
        else
        {
            LE_CRIT("Watchdog action string for application '%s' is too long.",
                    appRef->name);
            watchdogAction = WATCHDOG_ACTION_ERROR;
        }
    }

    // Set the action pointer to error. If it's still error when we leave here something
    // has gone wrong!!
    *watchdogActionPtr = WATCHDOG_ACTION_ERROR;

    // TODO: do watchdog timeouts count toward this total?
    switch (watchdogAction)
    {
        case WATCHDOG_ACTION_NOT_FOUND:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out but there is no \
policy. The process will be restarted by default.", proc_GetName(procRef), appRef->name);

            // Set the process to restart when it stops then stop it.
            procObjectRef->stopHandler = StartProc;
            StopProc(procRef);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_IGNORE:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and will be ignored \
in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_STOP:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and will be terminated \
in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);
            StopProc(procRef);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_RESTART:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and will be restarted \
in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            // Set the process to restart when it stops then stop it.
            procObjectRef->stopHandler = StartProc;
            StopProc(procRef);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_RESTART_APP:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and the app will be \
restarted in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            *watchdogActionPtr = watchdogAction;
            break;

        case WATCHDOG_ACTION_STOP_APP:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and the app will \
be stopped in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            *watchdogActionPtr = watchdogAction;
            break;

        case WATCHDOG_ACTION_REBOOT:
            LE_EMERG("The watchdog for process '%s' in app '%s' has timed out and the system will \
now be rebooted in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            *watchdogActionPtr = watchdogAction;
            break;

        case WATCHDOG_ACTION_ERROR:
            // something went wrong reading the action.
            LE_CRIT("An error occurred trying to find the watchdog action for process '%s' in \
application '%s'. Restarting app by default.", proc_GetName(procRef), appRef->name);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_HANDLED:
            *watchdogActionPtr = watchdogAction;
            break;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * This handler must be called when a SIGCHILD is received for a process that belongs to the
 * specified application.
 */
//--------------------------------------------------------------------------------------------------
void app_SigChildHandler
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    pid_t procPid,                      ///< [IN] The pid of the process that changed state.
    int procExitStatus,                 ///< [IN] The return status of the process given by wait().
    app_FaultAction_t* faultActionPtr   ///< [OUT] The fault action that should be taken.
)
{
    *faultActionPtr = APP_FAULT_ACTION_IGNORE;

    ProcObjRef_t procObjRef = FindProcObjectRef(appRef, procPid);

    if (procObjRef != NULL)
    {
        proc_Ref_t procRef = procObjRef->procRef;

        // Remember the previous fault time.
        time_t prevFaultTime = proc_GetFaultTime(procRef);

        // Get the current process fault action.
        proc_FaultAction_t procFaultAction = proc_SigChildHandler(procRef, procExitStatus);

        // Determine the fault action for the application.
        if (ReachedFaultLimit(appRef, procRef, procFaultAction, prevFaultTime))
        {
            LE_CRIT("The process '%s' in application '%s' has reached the fault limit so the \
application will be stopped instead of performing the configured fault action.",
                    proc_GetName(procRef), appRef->name);

            *faultActionPtr = APP_FAULT_ACTION_STOP_APP;
        }
        else
        {
            switch (procFaultAction)
            {
                case PROC_FAULT_ACTION_NO_FAULT:
                    // This is something that happens if we have deliberately killed the proc or if we
                    // paused or resumed the proc. If the wdog stopped it then we may get here with a
                    // stop handler attached ( to call StartProc).
                    if (procObjRef->stopHandler)
                    {
                        if (procObjRef->stopHandler(appRef, procRef) != LE_OK)
                        {
                            LE_ERROR("Watchdog could not restart process '%s' in application '%s'.",
                                    proc_GetName(procRef), appRef->name);

                            *faultActionPtr = APP_FAULT_ACTION_STOP_APP;
                        }
                    }
                    break;

                case PROC_FAULT_ACTION_IGNORE:
                    LE_CRIT("The process '%s' in app '%s' has faulted and will be ignored in \
accordance with its fault policy.", proc_GetName(procRef), appRef->name);
                    break;

                case PROC_FAULT_ACTION_RESTART:
                    LE_CRIT("The process '%s' in app '%s' has faulted and will be restarted in \
accordance with its fault policy.", proc_GetName(procRef), appRef->name);

                    // Restart the process now.
                    if (StartProc(appRef, procRef) != LE_OK)
                    {
                        LE_ERROR("Could not restart process '%s' in application '%s'.",
                                 proc_GetName(procRef), appRef->name);

                        *faultActionPtr = APP_FAULT_ACTION_STOP_APP;
                    }
                    break;

                case PROC_FAULT_ACTION_RESTART_APP:
                    LE_CRIT("The process '%s' in app '%s' has faulted and the app will be restarted \
in accordance with its fault policy.", proc_GetName(procRef), appRef->name);

                    *faultActionPtr = APP_FAULT_ACTION_RESTART_APP;
                    break;

                case PROC_FAULT_ACTION_STOP_APP:
                    LE_CRIT("The process '%s' in app '%s' has faulted and the app will be stopped \
in accordance with its fault policy.", proc_GetName(procRef), appRef->name);

                    *faultActionPtr = APP_FAULT_ACTION_STOP_APP;
                    break;

                case PROC_FAULT_ACTION_REBOOT:
                    LE_EMERG("The process '%s' in app '%s' has faulted and the system will now be \
rebooted in accordance with its fault policy.", proc_GetName(procRef), appRef->name);

                    WriteRebootFaultRec(appRef, procRef);

                    *faultActionPtr = APP_FAULT_ACTION_REBOOT;
                    break;
            }
        }
    }

    if (!HasRunningProc(appRef))
    {
        if (appRef->killTimer != NULL)
        {
            le_timer_Stop(appRef->killTimer);
        }

        LE_INFO("app '%s' has stopped.", appRef->name);

        // Note the application is cleaned up here so if the app is restarted it will apply the new
        // config settings if the config has changed.
        CleanupApp(appRef);

        appRef->state = APP_STATE_STOPPED;
    }
}

