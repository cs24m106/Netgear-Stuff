# M4350 - Create VLAN flow via CLI
> Command: vlan "number";

Flow:
## 1. CLI Command Registration:
- ewsCliAddNode(): Create a new parse tree, or add a node to an existing parse tree
```c
    // args: multiple overloaded fns present, but taking the common args that seems to required primarly by all...
    EwsCliCommandP parent       --> // pointer to parent node, or NULL to create a new root node
    const char     *command     --> // pointer to string containing command word in parse node
    const char     *description --> // pointer to string containing additional description, or NULL
    EwaCli_f       *action      --> // application function to call, or NULL. (def: root node's action)

    // return type:
    EwsCliCommandP --> "new node's pointer"
    
    where
    "EwsCliCommandP": almost certainly a pointer to a structure (likely `typedef struct EwsCliCommand *EwsCliCommandP`);
    "EwaCli_f *"    : Function Pointer, defines the "callback" when user runs that cmd
```
- Important Inputs:
    - callbackfn --> `commandVlanSwDev()` --> cli cmd str: `vlan`
    ```c
    /* @purpose  create/delete a vlan based on the VLAN ID
        * @returns cliPrompt(ewsContext) --> @returntype const L7_char8  *
        * @cmdsyntax  vlan <2-4094>
        * @cmdhelp Create/Delete a VLAN.                                   */
    const L7_char8 *commandVlanSwDev(
        EwsContext ewsContext, L7_uint32 argc,
        const L7_char8 ** argv, L7_uint32 index);
    ```

## 2. CLI Handler (Management Layer - Broadcom):
- cliPrompt(ewsContext) --> internally --> cliParseRangeInput()
- function acts as a "tokenizer." It scans the string for commas and hyphens to expand the list.

```c
//@purpose: Parses Input and prepares a list of VLANs to be created.
L7_RC_t cliParseRangeInput(
     const L7_char8 *buf, L7_uint32 *count,
     L7_uint32 *list, L7_uint32 listSize);
```

|Argument|Type|Direction|Description|
|---|---|---|---|
|buf|const L7_char8 *|Input|"The raw string typed by the user (e.g., ""1,5,10-15"")."|
|count|L7_uint32 *|Output|A pointer where the function stores the total number of items successfully parsed.|
|list|L7_uint32 *|Output|"An array (buffer) where the actual integers will be stored (e.g., [1, 5, 10, 11, 12, 13, 14, 15])."|
|listSize|L7_uint32|Input|The maximum capacity of the list array to prevent a buffer overflow.|

- Creates a mask from the list: `L7_VLAN_MASK_t cliVlanList;` where L7_VLAN_MASK_t is a broad_comm struct with def:
```c
/* Indices values for vlan masks */
/* Number of bytes in mask */
#define L7_VLAN_INDICES   ((L7_DOT1Q_MAX_VLAN_ID) / (sizeof(L7_uchar8) * 8) + 1)
#define L7_VLAN_MAX_MASK_BIT    L7_DOT1Q_MAX_VLAN_ID


/* structure definition for the vlan Mask : bitmask for all vlans */
/* Interface storage */
typedef struct
{
L7_uchar8   value[L7_VLAN_INDICES];
} L7_VLAN_MASK_t;
```
- Calls `dot1qVlanCreateMask(L7_VLAN_MASK_t *vidMask);` where vlan mask `cliVlanList` is the arg passed

## 3. USMDB (Unified Shared Memory Data Base):
- When you use the CLI or an API to configure a VLAN or an IP route, the SDK doesn't just write directly to the hardware registers and forget about it. It needs to keep a copy of that configuration in System RAM.
- USMDB is the middleware layer that manages this data. Its primary roles are: 
    - State Synchronization: hardware & config in ram matches, 
    - Warm Boot Support: switch software crashes or restarts, the USMDB is stored in a protected area of memory is restored onto hardware states
- `usmDbVlanCreateMaskSet(cliVlanList)` --> internally handles DB (current no logic is present in vanilla coda i think?) and calls `dot1qVlanCreateMask(cliVlanList)`
```c
L7_RC_t usmDbVlanCreateMaskSet(L7_uint32 unitIndex, L7_VLAN_MASK_t *vidMask)
{
return dot1qVlanCreateMask(vidMask);
}
```

## 4. Application Layer:
- `dot1qVlanCreateMask()`: Inside a OSAPI write lock, for each VLAN set in mask:
    - Checks if VLAN already exists - `dot1qVlanCheckValid()`
    - Check is VLAN is configurable - `dot1qVlanIsConfigurable()`
    ```c
    // @purpose  Check to see if a VLAN exists based on a VLAN ID.
    // @param    vid         vlan ID
    L7_RC_t   dot1qVlanCheckValid(L7_uint32 vid)
    // @returns  L7_SUCCESS, if success; 
    //           L7_NOT_EXIST, if VLAN does not exist
    //           L7_FAILURE, if other failure

    // @purpose  Request admin VLAN configuration
    // @param    vid      VLAN ID being requested
    // @param    *pVCfg    pointer to dot1qVlanCfgData_t structure containing in which to the configuration
    L7_BOOL dot1qVlanIsConfigurable(L7_uint32 vid, dot1qVlanCfgData_t **pVCfg);
    // @returns  L7_TRUE or L7_FALSE

    /* DOT1Q vlan-only configuration data */
    typedef struct dot1qVlanCfgData_s
    {
    L7_uint32   vlanId;
    dot1q_vlan_cfg_t vlanCfg;

    } dot1qVlanCfgData_t;

    /* static port information */
    typedef struct dot1q_vlan_cfg_s
    {
    L7_uchar8         name[L7_MAX_VLAN_NAME];     /* User-defined name     */
    NIM_INTF_MASK_t   staticEgressPorts;
    NIM_INTF_MASK_t   forbiddenEgressPorts;
    NIM_INTF_MASK_t   taggedSet;
    DOT1Q_PVLAN_TYPE_t pvlanType;
    dot1qPrivateVlan_t         *ptrPvDomain;
    #if DOT1Q_FUTURE_FUNC_GROUP_FILTER
    groupFilterSet_t  groupFilter;
    #endif
    L7_uint32         dot1cb_mode;
    L7_uint32         dot1cbFwdArpNdpDisabled; /* 1-Disabled, 0-Enabled */
    } dot1q_vlan_cfg_t;
    ```

    - If both fail, create configuration entry for the VLAN.
    - Notify registered users with VLAN_NAME_CHANGE_NOTIFY event.
    ```c
    // def:
    typedef enum{ 
        VLAN_ADD_NOTIFY = 0x00000001,     /* Create a new VLAN */
        VLAN_DELETE_NOTIFY = 0x00000004,      /* Delete a VLAN */
        VLAN_ADD_PORT_NOTIFY = 0x00000008,    /* Add a port to a VLAN */
        VLAN_DELETE_PORT_NOTIFY = 0x00000010,  /* Delete a port from a VLAN */
        ...
        VLAN_SWITCHPORT_MODE_CHANGE_NOTIFY = 0x00002000,  /* Switchport mode change on port notification */
        VLAN_NAME_CHANGE_NOTIFY = 0x00008000, /* Change in VLAN name on a VLAN */
        ... 
    }

    // Usage:
    vlanNotifyRegisteredUsers(&vlanData, 0, VLAN_NAME_CHANGE_NOTIFY);

    where:
    "vlanData" type --> dot1qNotifyData_t 

    /* VLAN Notification Structure */
    typedef struct dot1qNotifyData_s
    {
        L7_uint32 numVlans; /* If num Vlan is 1 use vlanId member of the union, else use vlanMask of the union*/
        union
        {
            L7_uint32 vlanId;
            L7_VLAN_MASK_t vlanMask;
        }data;
        L7_uint32   NumTrafficClasses;      /* 1-8 */
        L7_uint32   DefaultUserPriority;    /* 0-7 */
        L7_ushort16 Mapping[L7_DOT1P_MAX_PRIORITY+1];             /* priority to traffic class */ /* dot1dUserPriorityRegenTable */
        dot1qSwitchportNotifyData_t   switchportModeData;       /* switchport mode data */
        L7_char8 vlanName[L7_MAX_VLAN_NAME];
    }dot1qNotifyData_t;
    ```
    - Increment VLAN count.

- `dot1qIssueCmd:`
    - Posts the VLAN mask in a message format to dot1q task.
    - Event: dot1q_vlan_create_static_mask 
    - Uses osapiMessageSend for asynchronous processing.
    ```c
    /*@purpose  Place a command on the dot1q message queue
    * @param    *msg    pointer to DOT1Q_MSG_t
    * @returns  L7_SUCCESS or L7_FAILURE                */
    L7_RC_t dot1qIssueCmd(DOT1Q_MSG_t *msg);
    ```

```c
// msg structure
typedef struct DOT1Q_MSG_s 
{
L7_uint32            vlanId;
L7_uint32            mode; /* data for the event (untagged/tagged or fixed/aut/forbidden)*/
DOT1Q_SWPORT_MODE_t  swport_mode; /* Access | Trunk | General|None  mode through which the cmd is issued */
DOT1Q_EVENTS_t    event;
union 
{
    L7_uint32         intIfNum;
    L7_uchar8         name[L7_MAX_VLAN_NAME];
    NIM_INTF_MASK_t   intfMask;
    L7_CNFGR_CMD_DATA_t CmdData;
    NIM_EVENT_COMPLETE_INFO_t status;
    dot1q_msg_prio_t  prio;  /* NOTE: When using prio struct, use its intIfNum field*/ 
                            /*       instead of the one in the 'data' union        */
    dot1q_msg_intf_vlan_mask_t intfVlanMask;  /* NOTE: When using this struct, use its intIfNum field*/ 
                                            /*       instead of the one in the 'data' union        */
    dot1qNimStartup_t nimStartup;
} data;
vlanRequestor_t    requestor;
L7_COMPONENT_IDS_t acquirer;
/* vlan data*/
}DOT1Q_MSG_t;
```

- `dot1qDispatch():` Dot1q event processor.
    ```c
    /*@purpose  Process a command received on the dot1q message queue
    * @param    *msg    pointer to DOT1Q_MSG_t
    * @returns  L7_SUCCESS or L7_FAILURE                            */
    L7_RC_t dot1qDispatch(DOT1Q_MSG_t *msg);
    ```

- `dot1qVlanCreateMaskProcess()`
    - Inside a OSAPI write lock, Loops through VLAN mask and creates entry(vlanEntryAdd) in the VLAN tree.
    - The function starts by acquiring an OSAPI (Operating System Abstraction Layer API) Write Lock.
        - Why? The VLAN Database is a shared resource. If one thread is deleting a VLAN while another is creating 100 new ones, the database could become corrupted.
        - The "Write Lock" ensures mutual exclusion, meaning no other process can read or modify the VLAN tree until this function finishes.
    ```c
    /*@purpose  Create a range of vlans
    * @param    vlanMask     Vlan ID Mask
    * @returns  L7_SUCCESS, if success or L7_FAILURE, if other/failure
    * @notes    The vlan mask need not be contiguous                */
    void dot1qVlanCreateMaskProcess(L7_VLAN_MASK_t *vlanMask);
    ```
    - Calls `dtlDot1qCreateMask().`
    - Notify registered users with VLAN_ADD_NOTIFY using vlanNotifyRegisteredUsers()
    - Enable VLAN statistics - `dtlEnableVlanStats()`
    

## 5. Device Transformation Layer (DTL):
It acts as a critical "middleman" or translation bridge. Its job is to take generic networking concepts from the upper management software (like "Create a VLAN") and transform them into specific, hardware-readable instructions for the underlying chip drivers.

### The Role of DTL in the Architecture
To understand DTL, you have to look at where it sits in the software stack. It sits between the Application/Protocol Layer (where dot1qVlanCreateMaskProcess lives) and the DAPI (Device API) / SDK Layer.

- Platform Independence: DTL allows the same CLI and Protocol code to run on different types of switch chips. The CLI doesn't need to know if the chip is a "Tomahawk" or a "Trident"; it just tells DTL to create a VLAN, and DTL "transforms" that request for the specific hardware.
- Abstraction: It hides the complexity of hardware registers.

### Analysis of the DTL Functions
how the DTL handles a VLAN creation request:

`dtlDot1qCreateMask()`: 
```c
/*@purpose  Creates VLANs with no members
* @param    vlanMask      @b{(input)} VLAN ID Mask
* @param    numVlans      @b{(input)} Num of vlans set in the mask
* @param    *vlanMaskFailure @b{(output)} Vlan mask of vlans that were not created
* @param    *vlanFailureCount @b{(output)} Num of vlan that were not created
* @returns  L7_SUCCESS  if success or L7_FAILURE  if failure        */
L7_RC_t dtlDot1qCreateMask(
    L7_VLAN_MASK_t *vlanMask, L7_uint32 numVlans, 
    L7_VLAN_MASK_t* vlanMaskFailure, L7_uint32 *vlanFailureCount);
```
This is the "Batch Processor." Instead of calling the driver 100 times for 100 VLANs, it passes a VLAN Mask.
- Calls `dapiCtl()` with **DAPI** command **DAPI_CMD_QVLAN_VLAN_LIST_CREATE**.
- Transformation: It takes the high-level L7_VLAN_MASK_t and prepares a DAPI Command (DAPI_CMD_QVLAN_VLAN_LIST_CREATE).
- Error Tracking: Notice the parameters vlanMaskFailure and vlanFailureCount. Because hardware resources (like TCAM or VLAN IDs) are limited, some VLANs might fail to be created while others succeed. DTL tracks exactly which bits in the mask failed so it can report them back to the user.

`dtlEnableVlanStats()`:
```c
/*@purpose  Enable driver vlan statistics for this interface
*
* @param    intfNum     @b{(input)} Internal interface number
* @param    vlanID      @b{(input)} vlan id
* @param    enable      @b{(input)} Enable or Disable
*
* @returns  L7_SUCCESS, if statistics are enabled/disabled
* @returns  L7_NOT_SUPPORTED, if trying to enable/disable per port per VLAN counts
* @returns  L7_FAILURE, otherwise                           */
L7_RC_t dtlEnableVlanStats(L7_uint32 intIfNum, L7_uint32 vlanID, L7_BOOL enable);
```
Hardware chips have "Counters" (special memory slots that increment when a packet passes). There aren't enough counters for everything, so they must be explicitly enabled.
- Logic: It maps a logical VLAN ID to a physical counter resource in the chip.
- Constraints: It returns L7_NOT_SUPPORTED if the user asks for something the hardware physically cannot do (like counting every single packet on every port and every VLAN simultaneously, which is very memory-intensive).

## 6. DAPI: `dapiCtl()`
- DAPI's database dapi_g, has a list of port specific function pointers(cmdTable) for each DAPI command.
- hapiBroadQvlanVlanListCreate is the function pointer for DAPI_CMD_QVLAN_VLAN_LIST_CREATE
- Init for this is done in hapiBroadL2VlanPortInit()

## 7. HAPI:
```
hapiBroadQvlanVlanListCreate
        ↓
usl_bcmx_vlan_bulk_configure
        ↓
l7_rpc_client_vlan_bulk_configure
        ↓
l7_rpc_vlan_bulk_configure
        ↓
hpcHardwareRpc
        ↓
hpcHardwareRpcDispatch
        ↓
l7_rpc_server_vlan_bulk_update (RPC Server/Handler - Registered)
        ↓
usl_bcm_vlan_update
        ↓
bcm_vlan_create() (Broadcom SDK API)
```