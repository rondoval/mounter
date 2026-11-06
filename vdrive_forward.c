/*
 * vdrive_forward.c — the requests a virtual unit sends to its parent unit, and
 * their completion.
 *
 * A client's request is never handed to the parent: that would mean rewriting
 * its device, unit, command, offset and reply port and restoring them afterwards.
 * The device sends a forwarded request of its own instead, wrapped with what
 * completes the client's (struct VDWrappedIO) and taken from a free list. The
 * host hands it back with VDriveComplete(), from its own task, and that
 * completes the client's request on the spot: no message, no soft interrupt, no
 * task of the device's own. Everything here runs in tasks, under Forbid() like
 * all the device's state (vdrive_internal.h).
 *
 * Part of the generic autoboot/automount mounter; see mounter.c for the
 * copyright and licence covering this file.
 */
#include "vdrive_internal.h"

#include <stddef.h>

/**
 * vd_forward_init() - Set up the forward port and the free and busy lists.
 * @device: the device base, not yet added
 *
 * The port is PA_IGNORE and no message is meant to arrive there: it marks the
 * device's own forwarded requests, for VDriveComplete() and the fence. It is a
 * real port, not just a unique address, so that a host which ReplyMsg()s one
 * anyway only queues it here, where VDriveDestroy() finds it, instead of having
 * exec write list pointers into whatever the address held.
 *
 * Up to VD_SEED_WRAPPED wrapped requests go on the free list, as many as
 * AllocMem() gives, so that many can be in flight when memory is short. Task
 * context.
 */
void vd_forward_init(struct VDrive *device)
{
	struct ExecBase *SysBase = device->SysBase;

	new_list(&device->freeWrapped);
	new_list(&device->busyWrapped);
	new_ignore_port(&device->forwardPort);

	for (int i = 0; i < VD_SEED_WRAPPED; i++)
	{
		struct VDWrappedIO *wrapped = AllocMem(sizeof(struct VDWrappedIO), MEMF_PUBLIC | MEMF_CLEAR);
		if (wrapped)
			AddTail((struct List *)&device->freeWrapped, (struct Node *)wrapped);
	}
}

/**
 * vd_forward_free() - Free the wrapped requests on the free list.
 * @device: the device base, with no wrapped request in flight
 */
void vd_forward_free(struct VDrive *device)
{
	struct ExecBase *SysBase = device->SysBase;
	struct Node *wrapped;
	while ((wrapped = RemHead((struct List *)&device->freeWrapped)))
		FreeMem(wrapped, sizeof(struct VDWrappedIO));
}

/**
 * wrapped_get() - Take a wrapped request from the free list.
 * @device: the device base; caller holds Forbid() (the free list)
 *
 * Task context: may allocate. From the free list, else a new MEMF_PUBLIC |
 * MEMF_CLEAR allocation, which joins the free list too once it is done.
 *
 * Return: the wrapped request, on no list; NULL when the free list is empty
 * and AllocMem() fails
 */
static struct VDWrappedIO *wrapped_get(struct VDrive *device)
{
	struct ExecBase *SysBase = device->SysBase;
	struct VDWrappedIO *wrapped = (struct VDWrappedIO *)RemHead((struct List *)&device->freeWrapped);
	if (!wrapped)
		wrapped = AllocMem(sizeof(struct VDWrappedIO), MEMF_PUBLIC | MEMF_CLEAR);
	return wrapped;
}

/**
 * wrapped_of() - Find the wrapped request a busy-list node belongs to.
 * @node: its busyNode
 *
 * Return: the wrapped request
 */
static struct VDWrappedIO *wrapped_of(struct MinNode *node)
{
	return (struct VDWrappedIO *)((UBYTE *)node - offsetof(struct VDWrappedIO, busyNode));
}

/**
 * vd_forward() - Send a client's request on to the parent unit.
 * @device: the device base
 * @draft:  the wrapped request as the caller drafted it: the forwarded
 *          request's io_Command, io_Data, io_Length, and its offset on the
 *          parent split into io_Offset (low 32 bits) and io_Actual (high 32
 *          bits), as NSCMD_TD_*64 takes it; the client's request, its unit and
 *          its HD_SCSICMD. The rest is zero
 *
 * Copies @draft into a wrapped request from wrapped_get() and completes the
 * forwarded request, a plain IOStdReq since nothing sent is an ETD_ command:
 * the forward port as its mn_ReplyPort, which is also how the fence tells it
 * from other requests, and io_Device and io_Unit from the unit's held open of
 * its parent. It joins the busy list in the same Forbid() as it leaves the
 * free list, with every field set: vd_forward_abort() may find it from then
 * on. The client's request, if any, has IOF_QUICK cleared, and
 * VDriveComplete() completes it. All of it is set before SendIO(): the parent
 * may hand it back (VDriveComplete()) before SendIO() returns. Without a
 * wrapped request the client's request fails at once: TDERR_NoMem and
 * io_Actual 0, or for an emulated HD_SCSICMD CHECK CONDITION with ABORTED
 * COMMAND, insufficient resources (0Bh/55h/03h).
 */
void vd_forward(struct VDrive *device, const struct VDWrappedIO *draft)
{
	struct ExecBase *SysBase = device->SysBase;
	struct IOStdReq *client = draft->client;
	const struct IOStdReq *parentOpen = &draft->unit->parent->openReq;

	Forbid();
	struct VDWrappedIO *wrapped = wrapped_get(device);
	if (wrapped)
	{
		*wrapped = *draft;
		struct IOStdReq *forwarded = &wrapped->forwarded;
		forwarded->io_Message.mn_ReplyPort = &device->forwardPort;
		forwarded->io_Message.mn_Length = sizeof(struct IOStdReq);
		forwarded->io_Device = parentOpen->io_Device;
		forwarded->io_Unit = parentOpen->io_Unit;
		forwarded->io_Flags = 0;
		AddTail((struct List *)&device->busyWrapped, (struct Node *)&wrapped->busyNode);
	}
	Permit();

	if (!wrapped)
	{
		if (!client)
			return;
		if (draft->scsi)
		{
			vd_scsi_fail(draft->unit, client, draft->scsi, VD_SENSE_NO_RESOURCES);
		}
		else
		{
			client->io_Actual = 0;
			client->io_Error = TDERR_NoMem;
		}
		vd_reply(device, client, client->io_Error);
		return;
	}

	if (client)
		client->io_Flags &= (UBYTE)~IOF_QUICK;   /* replied by VDriveComplete() */
	SendIO((struct IORequest *)&wrapped->forwarded);
}

/**
 * vd_forward_abort() - Pass a client's AbortIO() on to the parent unit.
 * @device: the device base
 * @client: the client's request
 *
 * Finds the wrapped request in flight for @client and AbortIO()s its forwarded
 * request, both under one Forbid(), so the parent cannot hand it back
 * (VDriveComplete()) and have it reused in between. The parent decides: one
 * still queued there comes back with IOERR_ABORTED, which VDriveComplete()
 * passes to @client, and one it is executing completes as usual. Task context.
 *
 * Return: 0 when the abort went to the parent; -1 when @client has nothing in
 * flight there
 */
LONG vd_forward_abort(struct VDrive *device, struct IOStdReq *client)
{
	struct ExecBase *SysBase = device->SysBase;
	LONG result = -1;
	Forbid();
	for (struct MinNode *node = device->busyWrapped.mlh_Head; node->mln_Succ; node = node->mln_Succ)
	{
		struct VDWrappedIO *wrapped = wrapped_of(node);
		if (wrapped->client == client)
		{
			AbortIO((struct IORequest *)&wrapped->forwarded);   /* may complete it at once */
			result = 0;
			break;
		}
	}
	Permit();
	return result;
}

/**
 * VDriveComplete() - Complete a finished request if it is the virtual device's.
 * @device: the device; NULL accepted
 * @io:     a request the host has finished, io_Error and io_Actual set
 *
 * Hosts call it wherever they would ReplyMsg() a request, and reply it only on
 * FALSE. One of the device's own forwarded requests (its mn_ReplyPort is the
 * forward port) completes the client request wrapped with it: a trackdisk
 * one gets the forwarded request's io_Error and io_Actual, an emulated
 * HD_SCSICMD is completed by vd_scsi_done(), and either is ReplyMsg()ed; one
 * with no client request (a last-close CMD_UPDATE) just ends. The wrapped
 * request leaves the busy list for the free list, and no further: the host
 * must not touch @io afterwards, as after ReplyMsg(). Nor is @device touched
 * after Permit(), from when VDriveDestroy() may free it.
 *
 * Task context only, never an interrupt: the lists are guarded by Forbid().
 * Every one of the device's requests must come back this way. One that is
 * ReplyMsg()ed lands on the forward port, a PA_IGNORE one, and stays there:
 * its client never completes, and VDriveDestroy() refuses for good.
 *
 * Return: TRUE when @io was the device's own and is completed; FALSE for any
 * other request, @device NULL included, which the host replies as usual
 */
BOOL VDriveComplete(struct VDrive *device, struct IORequest *io)
{
	if (!device || io->io_Message.mn_ReplyPort != &device->forwardPort)
		return FALSE;

	struct ExecBase *SysBase = device->SysBase;
	struct VDWrappedIO *wrapped = (struct VDWrappedIO *)io;
	const struct IOStdReq *forwarded = &wrapped->forwarded;
	struct IOStdReq *client = wrapped->client;
	if (client)
	{
		if (wrapped->scsi)
		{
			vd_scsi_done(wrapped->unit, client, wrapped->scsi, forwarded);
		}
		else
		{
			client->io_Error = forwarded->io_Error;
			client->io_Actual = forwarded->io_Actual;
		}
	}
	Forbid();
	if (client)
		ReplyMsg(&client->io_Message);
	Remove((struct Node *)&wrapped->busyNode);
	AddHead((struct List *)&device->freeWrapped, (struct Node *)wrapped);
	Permit();
	return TRUE;
}
