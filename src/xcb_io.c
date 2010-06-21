/* Copyright (C) 2003-2006 Jamey Sharp, Josh Triplett
 * This file is licensed under the MIT license. See the file COPYING. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "Xlibint.h"
#include "locking.h"
#include "Xprivate.h"
#include "Xxcbint.h"
#include <xcb/xcbext.h>

#include <assert.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

static void return_socket(void *closure)
{
	Display *dpy = closure;
	InternalLockDisplay(dpy, /* don't skip user locks */ 0);
	_XSend(dpy, NULL, 0);
	dpy->bufmax = dpy->buffer;
	UnlockDisplay(dpy);
}

static void require_socket(Display *dpy)
{
	if(dpy->bufmax == dpy->buffer)
	{
		uint64_t sent;
		int flags = 0;
		/* if we don't own the event queue, we have to ask XCB
		 * to set our errors aside for us. */
		if(dpy->xcb->event_owner != XlibOwnsEventQueue)
			flags = XCB_REQUEST_CHECKED;
		if(!xcb_take_socket(dpy->xcb->connection, return_socket, dpy,
		                    flags, &sent))
			_XIOError(dpy);
		/* Xlib uses unsigned long for sequence numbers.  XCB
		 * uses 64-bit internally, but currently exposes an
		 * unsigned int API.  If these differ, Xlib cannot track
		 * the full 64-bit sequence number if 32-bit wrap
		 * happens while Xlib does not own the socket.  A
		 * complete fix would be to make XCB's public API use
		 * 64-bit sequence numbers. */
		assert(!(sizeof(unsigned long) > sizeof(unsigned int)
		         && dpy->xcb->event_owner == XlibOwnsEventQueue
		         && (sent - dpy->last_request_read >= (UINT64_C(1) << 32))));
		dpy->xcb->last_flushed = dpy->request = sent;
		dpy->bufmax = dpy->xcb->real_bufmax;
	}
}

/* Call internal connection callbacks for any fds that are currently
 * ready to read. This function will not block unless one of the
 * callbacks blocks.
 *
 * This code borrowed from _XWaitForReadable. Inverse call tree:
 * _XRead
 *  _XWaitForWritable
 *   _XFlush
 *   _XSend
 *  _XEventsQueued
 *  _XReadEvents
 *  _XRead[0-9]+
 *   _XAllocIDs
 *  _XReply
 *  _XEatData
 * _XReadPad
 */
static void check_internal_connections(Display *dpy)
{
	struct _XConnectionInfo *ilist;
	fd_set r_mask;
	struct timeval tv;
	int result;
	int highest_fd = -1;

	if(dpy->flags & XlibDisplayProcConni || !dpy->im_fd_info)
		return;

	FD_ZERO(&r_mask);
	for(ilist = dpy->im_fd_info; ilist; ilist = ilist->next)
	{
		assert(ilist->fd >= 0);
		FD_SET(ilist->fd, &r_mask);
		if(ilist->fd > highest_fd)
			highest_fd = ilist->fd;
	}
	assert(highest_fd >= 0);

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	result = select(highest_fd + 1, &r_mask, NULL, NULL, &tv);

	if(result == -1)
	{
		if(errno == EINTR)
			return;
		_XIOError(dpy);
	}

	for(ilist = dpy->im_fd_info; result && ilist; ilist = ilist->next)
		if(FD_ISSET(ilist->fd, &r_mask))
		{
			_XProcessInternalConnection(dpy, ilist);
			--result;
		}
}

static PendingRequest *append_pending_request(Display *dpy, unsigned long sequence)
{
	PendingRequest *node = malloc(sizeof(PendingRequest));
	assert(node);
	node->next = NULL;
	node->sequence = sequence;
	node->reply_waiter = 0;
	if(dpy->xcb->pending_requests_tail)
	{
		assert(XLIB_SEQUENCE_COMPARE(dpy->xcb->pending_requests_tail->sequence, <, node->sequence));
		assert(dpy->xcb->pending_requests_tail->next == NULL);
		dpy->xcb->pending_requests_tail->next = node;
	}
	else
		dpy->xcb->pending_requests = node;
	dpy->xcb->pending_requests_tail = node;
	return node;
}

static void dequeue_pending_request(Display *dpy, PendingRequest *req)
{
	assert(req == dpy->xcb->pending_requests);
	dpy->xcb->pending_requests = req->next;
	if(!dpy->xcb->pending_requests)
	{
		assert(req == dpy->xcb->pending_requests_tail);
		dpy->xcb->pending_requests_tail = NULL;
	}
	else
		assert(XLIB_SEQUENCE_COMPARE(req->sequence, <, dpy->xcb->pending_requests->sequence));
	free(req);
}

static int handle_error(Display *dpy, xError *err, Bool in_XReply)
{
	_XExtension *ext;
	int ret_code;
	/* Oddly, Xlib only allows extensions to suppress errors when
	 * those errors were seen by _XReply. */
	if(in_XReply)
		/*
		 * we better see if there is an extension who may
		 * want to suppress the error.
		 */
		for(ext = dpy->ext_procs; ext; ext = ext->next)
			if(ext->error && (*ext->error)(dpy, err, &ext->codes, &ret_code))
				return ret_code;
	_XError(dpy, err);
	return 0;
}

/* Widen a 32-bit sequence number into a native-word-size (unsigned long)
 * sequence number.  Treating the comparison as a 1 and shifting it avoids a
 * conditional branch, and shifting by 16 twice avoids a compiler warning when
 * sizeof(unsigned long) == 4. */
static void widen(unsigned long *wide, unsigned int narrow)
{
	unsigned long new = (*wide & ~0xFFFFFFFFUL) | narrow;
	*wide = new + ((unsigned long) (new < *wide) << 16 << 16);
}

/* Thread-safety rules:
 *
 * At most one thread can be reading from XCB's event queue at a time.
 * If you are not the current event-reading thread and you need to find
 * out if an event is available, you must wait.
 *
 * The same rule applies for reading replies.
 *
 * A single thread cannot be both the the event-reading and the
 * reply-reading thread at the same time.
 *
 * We always look at both the current event and the first pending reply
 * to decide which to process next.
 *
 * We always process all responses in sequence-number order, which may
 * mean waiting for another thread (either the event_waiter or the
 * reply_waiter) to handle an earlier response before we can process or
 * return a later one. If so, we wait on the corresponding condition
 * variable for that thread to process the response and wake us up.
 */

static xcb_generic_reply_t *poll_for_event(Display *dpy)
{
	/* Make sure the Display's sequence numbers are valid */
	require_socket(dpy);

	/* Precondition: This thread can safely get events from XCB. */
	assert(dpy->xcb->event_owner == XlibOwnsEventQueue && !dpy->xcb->event_waiter);

	if(!dpy->xcb->next_event)
		dpy->xcb->next_event = xcb_poll_for_event(dpy->xcb->connection);

	if(dpy->xcb->next_event)
	{
		PendingRequest *req = dpy->xcb->pending_requests;
		xcb_generic_event_t *event = dpy->xcb->next_event;
		unsigned long event_sequence = dpy->last_request_read;
		widen(&event_sequence, event->full_sequence);
		if(!req || XLIB_SEQUENCE_COMPARE(event_sequence, <, req->sequence)
		        || (event->response_type != X_Error && event_sequence == req->sequence))
		{
			assert(XLIB_SEQUENCE_COMPARE(event_sequence, <=, dpy->request));
			dpy->last_request_read = event_sequence;
			dpy->xcb->next_event = NULL;
			return (xcb_generic_reply_t *) event;
		}
	}
	return NULL;
}

static xcb_generic_reply_t *poll_for_response(Display *dpy)
{
	void *response;
	xcb_generic_error_t *error;
	PendingRequest *req;

	response = poll_for_event(dpy);
	if (response)
		return response;

	while((req = dpy->xcb->pending_requests) &&
	      !req->reply_waiter &&
	      xcb_poll_for_reply(dpy->xcb->connection, req->sequence, &response, &error))
	{
		assert(XLIB_SEQUENCE_COMPARE(req->sequence, <=, dpy->request));
		dpy->last_request_read = req->sequence;
		if(response)
			return response;
		dequeue_pending_request(dpy, req);
		if(error)
			return (xcb_generic_reply_t *) error;
	}

	return NULL;
}

static void handle_response(Display *dpy, xcb_generic_reply_t *response, Bool in_XReply)
{
	_XAsyncHandler *async, *next;
	switch(response->response_type)
	{
	case X_Reply:
		for(async = dpy->async_handlers; async; async = next)
		{
			next = async->next;
			if(async->handler(dpy, (xReply *) response, (char *) response, sizeof(xReply) + (response->length << 2), async->data))
				break;
		}
		break;

	case X_Error:
		handle_error(dpy, (xError *) response, in_XReply);
		break;

	default: /* event */
		/* GenericEvents may be > 32 bytes. In this case, the
		 * event struct is trailed by the additional bytes. the
		 * xcb_generic_event_t struct uses 4 bytes for internal
		 * numbering, so we need to shift the trailing data to
		 * be after the first 32 bytes. */
		if(response->response_type == GenericEvent && ((xcb_ge_event_t *) response)->length)
		{
			xcb_ge_event_t *event = (xcb_ge_event_t *) response;
			memmove(&event->full_sequence, &event[1], event->length * 4);
		}
		_XEnq(dpy, (xEvent *) response);
		break;
	}
	free(response);
}

int _XEventsQueued(Display *dpy, int mode)
{
	xcb_generic_reply_t *response;
	if(dpy->flags & XlibDisplayIOError)
		return 0;
	if(dpy->xcb->event_owner != XlibOwnsEventQueue)
		return 0;

	if(mode == QueuedAfterFlush)
		_XSend(dpy, NULL, 0);
	else
		check_internal_connections(dpy);

	/* If another thread is blocked waiting for events, then we must
	 * let that thread pick up the next event. Since it blocked, we
	 * can reasonably claim there are no new events right now. */
	if(!dpy->xcb->event_waiter)
	{
		while((response = poll_for_response(dpy)))
			handle_response(dpy, response, False);
		if(xcb_connection_has_error(dpy->xcb->connection))
			_XIOError(dpy);
	}
	return dpy->qlen;
}

/* _XReadEvents - Flush the output queue,
 * then read as many events as possible (but at least 1) and enqueue them
 */
void _XReadEvents(Display *dpy)
{
	xcb_generic_reply_t *response;
	unsigned long serial;

	if(dpy->flags & XlibDisplayIOError)
		return;
	_XSend(dpy, NULL, 0);
	if(dpy->xcb->event_owner != XlibOwnsEventQueue)
		return;
	check_internal_connections(dpy);

	serial = dpy->next_event_serial_num;
	while(serial == dpy->next_event_serial_num || dpy->qlen == 0)
	{
		if(dpy->xcb->event_waiter)
		{
			ConditionWait(dpy, dpy->xcb->event_notify);
			/* Maybe the other thread got us an event. */
			continue;
		}

		if(!dpy->xcb->next_event)
		{
			xcb_generic_event_t *event;
			dpy->xcb->event_waiter = 1;
			UnlockDisplay(dpy);
			event = xcb_wait_for_event(dpy->xcb->connection);
			InternalLockDisplay(dpy, /* don't skip user locks */ 0);
			dpy->xcb->event_waiter = 0;
			ConditionBroadcast(dpy, dpy->xcb->event_notify);
			if(!event)
				_XIOError(dpy);
			dpy->xcb->next_event = event;
		}

		/* We've established most of the conditions for
		 * poll_for_response to return non-NULL. The exceptions
		 * are connection shutdown, and finding that another
		 * thread is waiting for the next reply we'd like to
		 * process. */

		response = poll_for_response(dpy);
		if(response)
			handle_response(dpy, response, False);
		else if(dpy->xcb->pending_requests && dpy->xcb->pending_requests->reply_waiter)
		{ /* need braces around ConditionWait */
			ConditionWait(dpy, dpy->xcb->reply_notify);
		}
		else
			_XIOError(dpy);
	}

	/* The preceding loop established that there is no
	 * event_waiter--unless we just called ConditionWait because of
	 * a reply_waiter, in which case another thread may have become
	 * the event_waiter while we slept unlocked. */
	if(!dpy->xcb->event_waiter)
		while((response = poll_for_response(dpy)))
			handle_response(dpy, response, False);
	if(xcb_connection_has_error(dpy->xcb->connection))
		_XIOError(dpy);
}

/*
 * _XSend - Flush the buffer and send the client data. 32 bit word aligned
 * transmission is used, if size is not 0 mod 4, extra bytes are transmitted.
 *
 * Note that the connection must not be read from once the data currently
 * in the buffer has been written.
 */
void _XSend(Display *dpy, const char *data, long size)
{
	static const xReq dummy_request;
	static char const pad[3];
	struct iovec vec[3];
	uint64_t requests;
	_XExtension *ext;
	xcb_connection_t *c = dpy->xcb->connection;
	if(dpy->flags & XlibDisplayIOError)
		return;

	if(dpy->bufptr == dpy->buffer && !size)
		return;

	/* iff we asked XCB to set aside errors, we must pick those up
	 * eventually. iff there are async handlers, we may have just
	 * issued requests that will generate replies. in either case,
	 * we need to remember to check later. */
	if(dpy->xcb->event_owner != XlibOwnsEventQueue || dpy->async_handlers)
	{
		uint64_t sequence;
		for(sequence = dpy->xcb->last_flushed + 1; sequence <= dpy->request; ++sequence)
			append_pending_request(dpy, sequence);
	}
	requests = dpy->request - dpy->xcb->last_flushed;
	dpy->xcb->last_flushed = dpy->request;

	vec[0].iov_base = dpy->buffer;
	vec[0].iov_len = dpy->bufptr - dpy->buffer;
	vec[1].iov_base = (caddr_t) data;
	vec[1].iov_len = size;
	vec[2].iov_base = (caddr_t) pad;
	vec[2].iov_len = -size & 3;

	for(ext = dpy->flushes; ext; ext = ext->next_flush)
	{
		int i;
		for(i = 0; i < 3; ++i)
			if(vec[i].iov_len)
				ext->before_flush(dpy, &ext->codes, vec[i].iov_base, vec[i].iov_len);
	}

	if(xcb_writev(c, vec, 3, requests) < 0)
		_XIOError(dpy);
	dpy->bufptr = dpy->buffer;
	dpy->last_req = (char *) &dummy_request;

	check_internal_connections(dpy);

	_XSetSeqSyncFunction(dpy);
}

/*
 * _XFlush - Flush the X request buffer.  If the buffer is empty, no
 * action is taken.
 */
void _XFlush(Display *dpy)
{
	require_socket(dpy);
	_XSend(dpy, NULL, 0);

	_XEventsQueued(dpy, QueuedAfterReading);
}

static const XID inval_id = ~0UL;

void _XIDHandler(Display *dpy)
{
	if (dpy->xcb->next_xid == inval_id)
		_XAllocIDs(dpy, &dpy->xcb->next_xid, 1);
}

/* _XAllocID - resource ID allocation routine. */
XID _XAllocID(Display *dpy)
{
	XID ret = dpy->xcb->next_xid;
	assert (ret != inval_id);
	dpy->xcb->next_xid = inval_id;
	_XSetPrivSyncFunction(dpy);
	return ret;
}

/* _XAllocIDs - multiple resource ID allocation routine. */
void _XAllocIDs(Display *dpy, XID *ids, int count)
{
	int i;
#ifdef XTHREADS
	if (dpy->lock)
		(*dpy->lock->user_lock_display)(dpy);
	UnlockDisplay(dpy);
#endif
	for (i = 0; i < count; i++)
		ids[i] = xcb_generate_id(dpy->xcb->connection);
#ifdef XTHREADS
	InternalLockDisplay(dpy, /* don't skip user locks */ 0);
	if (dpy->lock)
		(*dpy->lock->user_unlock_display)(dpy);
#endif
}

static void _XFreeReplyData(Display *dpy, Bool force)
{
	if(!force && dpy->xcb->reply_consumed < dpy->xcb->reply_length)
		return;
	free(dpy->xcb->reply_data);
	dpy->xcb->reply_data = NULL;
}

/*
 * _XReply - Wait for a reply packet and copy its contents into the
 * specified rep.
 * extra: number of 32-bit words expected after the reply
 * discard: should I discard data following "extra" words?
 */
Status _XReply(Display *dpy, xReply *rep, int extra, Bool discard)
{
	xcb_generic_error_t *error;
	xcb_connection_t *c = dpy->xcb->connection;
	char *reply;
	PendingRequest *current;

	assert(!dpy->xcb->reply_data);

	if(dpy->flags & XlibDisplayIOError)
		return 0;

	_XSend(dpy, NULL, 0);
	if(dpy->xcb->pending_requests_tail && dpy->xcb->pending_requests_tail->sequence == dpy->request)
		current = dpy->xcb->pending_requests_tail;
	else
		current = append_pending_request(dpy, dpy->request);
	/* Don't let any other thread get this reply. */
	current->reply_waiter = 1;

	while(1)
	{
		PendingRequest *req = dpy->xcb->pending_requests;
		xcb_generic_reply_t *response;

		if(req != current && req->reply_waiter)
		{
			ConditionWait(dpy, dpy->xcb->reply_notify);
			/* Another thread got this reply. */
			continue;
		}
		req->reply_waiter = 1;
		UnlockDisplay(dpy);
		response = xcb_wait_for_reply(c, req->sequence, &error);
		InternalLockDisplay(dpy, /* don't skip user locks */ 0);

		/* We have the response we're looking for. Now, before
		 * letting anyone else process this sequence number, we
		 * need to process any events that should have come
		 * earlier. */

		if(dpy->xcb->event_owner == XlibOwnsEventQueue)
		{
			xcb_generic_reply_t *event;
			/* If some thread is already waiting for events,
			 * it will get the first one. That thread must
			 * process that event before we can continue. */
			/* FIXME: That event might be after this reply,
			 * and might never even come--or there might be
			 * multiple threads trying to get events. */
			while(dpy->xcb->event_waiter)
			{ /* need braces around ConditionWait */
				ConditionWait(dpy, dpy->xcb->event_notify);
			}
			while((event = poll_for_event(dpy)))
				handle_response(dpy, event, True);
		}

		req->reply_waiter = 0;
		ConditionBroadcast(dpy, dpy->xcb->reply_notify);
		assert(XLIB_SEQUENCE_COMPARE(req->sequence, <=, dpy->request));
		dpy->last_request_read = req->sequence;
		if(!response)
			dequeue_pending_request(dpy, req);

		if(req == current)
		{
			reply = (char *) response;
			break;
		}

		if(error)
			handle_response(dpy, (xcb_generic_reply_t *) error, True);
		else if(response)
			handle_response(dpy, response, True);
	}
	check_internal_connections(dpy);

	if(dpy->xcb->next_event && dpy->xcb->next_event->response_type == X_Error)
	{
		xcb_generic_event_t *event = dpy->xcb->next_event;
		unsigned long event_sequence = dpy->last_request_read;
		widen(&event_sequence, event->full_sequence);
		if(event_sequence == current->sequence)
		{
			error = (xcb_generic_error_t *) event;
			dpy->xcb->next_event = NULL;
		}
	}

	if(error)
	{
		int ret_code;

		/* Xlib is evil and assumes that even errors will be
		 * copied into rep. */
		memcpy(rep, error, 32);

		/* do not die on "no such font", "can't allocate",
		   "can't grab" failures */
		switch(error->error_code)
		{
			case BadName:
				switch(error->major_code)
				{
					case X_LookupColor:
					case X_AllocNamedColor:
						free(error);
						return 0;
				}
				break;
			case BadFont:
				if(error->major_code == X_QueryFont) {
					free(error);
					return 0;
				}
				break;
			case BadAlloc:
			case BadAccess:
				free(error);
				return 0;
		}

		ret_code = handle_error(dpy, (xError *) error, True);
		free(error);
		return ret_code;
	}

	/* it's not an error, but we don't have a reply, so it's an I/O
	 * error. */
	if(!reply)
	{
		_XIOError(dpy);
		return 0;
	}

	/* there's no error and we have a reply. */
	dpy->xcb->reply_data = reply;
	dpy->xcb->reply_consumed = sizeof(xReply) + (extra * 4);
	dpy->xcb->reply_length = sizeof(xReply);
	if(dpy->xcb->reply_data[0] == 1)
		dpy->xcb->reply_length += (((xcb_generic_reply_t *) dpy->xcb->reply_data)->length * 4);

	/* error: Xlib asks too much. give them what we can anyway. */
	if(dpy->xcb->reply_length < dpy->xcb->reply_consumed)
		dpy->xcb->reply_consumed = dpy->xcb->reply_length;

	memcpy(rep, dpy->xcb->reply_data, dpy->xcb->reply_consumed);
	_XFreeReplyData(dpy, discard);
	return 1;
}

int _XRead(Display *dpy, char *data, long size)
{
	assert(size >= 0);
	if(size == 0)
		return 0;
	assert(dpy->xcb->reply_data != NULL);
	assert(dpy->xcb->reply_consumed + size <= dpy->xcb->reply_length);
	memcpy(data, dpy->xcb->reply_data + dpy->xcb->reply_consumed, size);
	dpy->xcb->reply_consumed += size;
	_XFreeReplyData(dpy, False);
	return 0;
}

/*
 * _XReadPad - Read bytes from the socket taking into account incomplete
 * reads.  If the number of bytes is not 0 mod 4, read additional pad
 * bytes.
 */
void _XReadPad(Display *dpy, char *data, long size)
{
	_XRead(dpy, data, size);
	dpy->xcb->reply_consumed += -size & 3;
	_XFreeReplyData(dpy, False);
}

/* Read and discard "n" 8-bit bytes of data */
void _XEatData(Display *dpy, unsigned long n)
{
	dpy->xcb->reply_consumed += n;
	_XFreeReplyData(dpy, False);
}
