/* ------------------------------------------------------------------------
   Messaging.c
   College of Applied Science and Technology
   The University of Arizona
   CYBV 489

   Student Names:  Jovanni Blanco & Raul Cano

   ------------------------------------------------------------------------ */
#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <THREADSLib.h>
#include <Scheduler.h>
#include <Messaging.h>
#include <stdint.h>
#include "message.h"

/* ------------------------- Prototypes ----------------------------------- */
static void nullsys(system_call_arguments_t* args);

/* Note: interrupt_handler_t is already defined in THREADSLib.h with the signature:
 *   void (*)(char deviceId[32], uint8_t command, uint32_t status, void *pArgs)
 */

static void InitializeHandlers();
static int check_io_messaging(void);
extern int MessagingEntryPoint(void*);
static void checkKernelMode(const char* functionName);

struct psr_bits {
    unsigned int cur_int_enable : 1;
    unsigned int cur_mode : 1;
    unsigned int prev_int_enable : 1;
    unsigned int prev_mode : 1;
    unsigned int unused : 28;
};

union psr_values {
    struct psr_bits bits;
    unsigned int integer_part;
};


/* -------------------------- Globals ------------------------------------- */

/* Obtained from THREADS*/
interrupt_handler_t* handlers;

/* system call array of function pointers */
void (*systemCallVector[THREADS_MAX_SYSCALLS])(system_call_arguments_t* args);

/* the mail boxes */
MailBox mailboxes[MAXMBOX];
MailSlot mailSlots[MAXSLOTS];

typedef struct
{
    void* deviceHandle;
    int deviceMbox;
    int deviceType;
    char deviceName[16];
} DeviceManagementData;

static DeviceManagementData devices[THREADS_MAX_DEVICES];
static int nextMailboxId = 0;
static int waitingOnDevice = 0;


/* ------------------------------------------------------------------------
     Name - SchedulerEntryPoint
     Purpose - Initializes mailboxes and interrupt vector.
               Start the Messaging test process.
     Parameters - one, default arg passed by k_spawn that is not used here.
----------------------------------------------------------------------- */
int SchedulerEntryPoint(void* arg)
{
    // TODO: check for kernel mode
    checkKernelMode("SchedulerEntryPoint");

    /* Disable interrupts */
    disableInterrupts();

    /* set this to the real check_io function. */
    check_io = check_io_messaging;

    /* Initialize the mail box table, slots, & other data structures.
     * Initialize int_vec and sys_vec, allocate mailboxes for interrupt
     * handlers.  Etc... */
    //initialize device mboxes
    devices[THREADS_CLOCK_DEVICE_ID].deviceMbox = mailbox_create(0, sizeof(int)); //initialize clock
    for (int i = 0; i < THREADS_MAX_DEVICES; i++)
    {
        if (i != THREADS_CLOCK_DEVICE_ID)
        {
            devices[i].deviceMbox = mailbox_create(10, sizeof(int)); //other devices
        }
    }

    //after creating device mboxes, update nexMailboxId
    nextMailboxId = THREADS_MAX_DEVICES; //device mboxes should fill first THREADS_MAX_DEVICES slots

    /* Initialize the devices and their mailboxes. */
    /* Allocate mailboxes for use by the interrupt handlers.
     * Note: The clock device uses a zero-slot mailbox, while I/O devices
     * (disks, terminals) need slotted mailboxes since their interrupt
     * handlers use non-blocking sends.
     */
    // TODO: Create mailboxes for each device.
    //   devices[THREADS_CLOCK_DEVICE_ID].deviceMbox = mailbox_create(0, sizeof(int));
    //   devices[i].deviceMbox = mailbox_create(..., sizeof(int));

    /* TODO: Initialize the devices using device_initialize().
     * The devices are: disk0, disk1, term0, term1, term2, term3.
     * Store the device handle and name in the devices array.
     */

    InitializeHandlers();

    enableInterrupts();

    /* TODO: Create a process for Messaging, then block on a wait until messaging exits.*/
    int pid = k_spawn("MessagingTest00", MessagingEntryPoint, NULL, 4 * THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
    int exitCode;
    k_wait(&exitCode);

    k_exit(0);

    return 0;
} /* SchedulerEntryPoint */


/* ------------------------------------------------------------------------
   Name - mailbox_create
   Purpose - gets a free mailbox from the table of mailboxes and initializes it
   Parameters - maximum number of slots in the mailbox and the max size of a msg
                sent to the mailbox.
   Returns - -1 to indicate that no mailbox was created, or a value >= 0 as the
             mailbox id.
   ----------------------------------------------------------------------- */
int mailbox_create(int slots, int slot_size)
{
    checkKernelMode("mailbox_create");

    if (nextMailboxId >= MAXMBOX)
    {
        return -1; //returns if all mailboxes are in use or if no available mailboxes
    }

    MAILBOX_TYPE type;
    if (slots == 0)
    {
        type = MB_ZEROSLOT; //for 0slot mailboxes
    }
    else if (slots == 1)
    {
        type = MB_SINGLESLOT; //for single slot mailboexes
    }
    else
    {
        type = MB_MULTISLOT; //for multislot mailboxes
    }
    
    //initialize the mailbox
    int id = nextMailboxId;
    mailboxes[id].mbox_id = id;
    mailboxes[id].type = type;
    mailboxes[id].status = MBSTATUS_INUSE;
    mailboxes[id].slotSize = slot_size;
    mailboxes[id].slotCount = slots;
    mailboxes[id].pSlotListHead = NULL;

    
    //creates the slots for multi slot mboxes
    if (type == MB_MULTISLOT)
    {
        SlotPtr prevSlot = NULL;

        for (int i = 0; i < slots; i++)
        {
            SlotPtr newSlot = malloc(sizeof(MailSlot)); //alloc new slot
            if (!newSlot)
            {
                return -1; //occurs during malloc failure
            }

            newSlot->mbox_id = id;
            newSlot->messageSize = 0;
            newSlot->pNextSlot = NULL;
            newSlot->pPrevSlot = prevSlot;

            if (prevSlot)
            {
                prevSlot->pNextSlot = newSlot;
            }
            else
            {
                mailboxes[id].pSlotListHead = newSlot; //first slot initializes the head
            }
            
            prevSlot = newSlot; //move to next slot
        }
    }

    nextMailboxId++; //increments the mailbox id counter

    return id;
} /* mailbox_create */


/* ------------------------------------------------------------------------
   Name - mailbox_send
   Purpose - Put a message into a slot for the indicated mailbox.
             Block the sending process if no slot available.
   Parameters - mailbox id, pointer to data of msg, # of bytes in msg,
                block flag.
   Returns - zero if successful, -1 if invalid args, -2 if would block
             (non-blocking mode), -5 if signaled while waiting.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int mailbox_send(int mboxId, void* pMsg, int msg_size, int wait)
{
    // Validate mailbox ID
    if (mboxId < 0 || mboxId >= MAXMBOX)
        return -1;

    // Get the mailbox
    MailBox* mb = &mailboxes[mboxId];

    // Ensure the mailbox is active and not released
    if (mb->status == MBSTATUS_RELEASED)
        return -1;

    // Check if the message size exceeds the slot size
    if (msg_size > mb->slotSize)
        return -1;  // Message is too large for the slot

    // Try to find an empty slot to place the message
    for (SlotPtr slot = mb->pSlotListHead; slot != NULL; slot = slot->pNextSlot)
    {
        if (slot->messageSize == 0)  // Empty slot found
        {
            // Copy the message into the slot
            memcpy(slot->message, pMsg, msg_size);
            slot->messageSize = msg_size;  // Mark the slot with the message size
            return 0;  // Successfully sent the message
        }
    }

    // If no slot is available and not blocking, return -2
    if (!wait)
        return -2;

    // Block the sending process by adding it to the blocked list
    WaitingProcess* newProcess = malloc(sizeof(WaitingProcess));
    if (!newProcess)
        return -1;  // Memory allocation failed

    newProcess->pNextProcess = NULL;

    // Add the new process to the blocked send list
    if (mb->blockedSendList == NULL)
        mb->blockedSendList = newProcess;
    else
    {
        WaitingProcess* temp = mb->blockedSendList;
        while (temp->pNextProcess != NULL)
            temp = temp->pNextProcess;
        temp->pNextProcess = newProcess;
    }

    return -5;  // Blocked sending
}

/* ------------------------------------------------------------------------
   Name - mailbox_receive
   Purpose - Receive a message from the indicated mailbox.
             Block the receiving process if no message available.
   Parameters - mailbox id, pointer to buffer for msg, max size of buffer,
                block flag.
   Returns - size of received msg (>=0) if successful, -1 if invalid args,
             -2 if would block (non-blocking mode), -5 if signaled.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int mailbox_receive(int mboxId, void* pMsg, int msg_size, int wait)
{
    if (mboxId < 0 || mboxId >= MAXMBOX)
        return -1;
    
    MailBox* mb = &mailboxes[mboxId];
    if (mb->status == MBSTATUS_RELEASED)
        return -1;
    if (msg_size < mb->slotSize)
        return -1;

    SlotPtr slot = mb->pSlotListHead;
    while (slot != NULL)
    {
        if (slot->messageSize > 0)
        {
            memcpy(pMsg, slot->message, slot->messageSize);
            int msgSize = slot->messageSize;
            slot->messageSize = 0;

            if (mb->blockedReceiveList != NULL)
            {
                WaitingProcess* blockedProcess = mb->blockedReceiveList;
                mb->blockedReceiveList = blockedProcess->pNextProcess;
                free(blockedProcess);
            }
            return msgSize;
        }
        slot = slot->pNextSlot;
    }
    if (!wait)
        return -2;

    WaitingProcess* newProcess = malloc(sizeof(WaitingProcess));
    if (!newProcess)
        return -1;

    newProcess->pNextProcess = NULL;

    if (mb->blockedReceiveList == NULL)
    {
        mb->blockedReceiveList = newProcess;
    }
    else
    {
        WaitingProcess* temp = mb->blockedReceiveList;
        while (temp->pNextProcess != NULL)
        {
            temp = temp->pNextProcess;
        }
        temp->pNextProcess = newProcess;
    }
    return -5;
}
/* ------------------------------------------------------------------------
   Name - mailbox_free
   Purpose - Frees a previously created mailbox. Any process waiting on
             the mailbox should be signaled and unblocked.
   Parameters - mailbox id.
   Returns - zero if successful, -1 if invalid args, -5 if signaled
             while closing the mailbox.
   ----------------------------------------------------------------------- */
int mailbox_free(int mboxId)
{
    int result = -1;

    return result;
}

/* ------------------------------------------------------------------------
   Name - wait_device
   Purpose - Waits for a device interrupt by blocking on the device's
             mailbox. Returns the device status via the status pointer.
   Parameters - device name string, pointer to status output.
   Returns - 0 if successful, -1 if invalid parameter, -5 if signaled.
   ----------------------------------------------------------------------- */
int wait_device(char* deviceName, int* status)
{
    int result = 0;
    uint32_t deviceHandle = -1;
    checkKernelMode("waitdevice");

    enableInterrupts();

    if (strcmp(deviceName, "clock") == 0)
    {
        deviceHandle = THREADS_CLOCK_DEVICE_ID;
    }
    else
    {
        deviceHandle = device_handle(deviceName);

    }

    if (deviceHandle >= 0 && deviceHandle < THREADS_MAX_DEVICES)
    {
        /* set a flag that there is a process waiting on a device. */
        waitingOnDevice++;
        mailbox_receive(devices[deviceHandle].deviceMbox, status, sizeof(int), TRUE);

        disableInterrupts();

        waitingOnDevice--;
    }
    else
    {
        console_output(FALSE, "Unknown device type.");
        stop(-1);
    }

    /* spec says return -5 if signaled. */
    if (signaled())
    {
        result = -5;
    }

    return result;
}


int check_io_messaging(void)
{
    if (waitingOnDevice)
    {
        return 1;
    }
    return 0;
}

static void InitializeHandlers()
{
    handlers = get_interrupt_handlers();

    /* TODO: Register interrupt handlers in the handlers array.
     * Use the interrupt indices defined in THREADSLib.h:
     *   handlers[THREADS_TIMER_INTERRUPT]   = your_clock_handler;
     *   handlers[THREADS_IO_INTERRUPT]      = your_io_handler;
     *   handlers[THREADS_SYS_CALL_INTERRUPT] = your_syscall_handler;
     *
     * Also initialize the system call vector (systemCallVector).
     */

    for (int i = 0; i < THREADS_MAX_SYSCALLS; i++)
    {
        systemCallVector[i] = nullsys;
    }

}


/* an error method to handle invalid syscalls */
static void nullsys(system_call_arguments_t* args)
{
    console_output(FALSE,"nullsys(): Invalid syscall %d. Halting...\n", args->call_id);
    stop(1);
} /* nullsys */


/*****************************************************************************
   Name - checkKernelMode
   Purpose - Checks the PSR for kernel mode and halts if in user mode
   Parameters -
   Returns -
****************************************************************************/
static inline void checkKernelMode(const char* functionName)
{
    union psr_values psrValue;

    psrValue.integer_part = get_psr();
    if (psrValue.bits.cur_mode == 0)
    {
        console_output(FALSE, "Kernel mode expected, but function called in user mode.\n");
        stop(1);
    }
}
