/*
 * Server-side socket management
 *
 * Copyright (C) 1999 Marcus Meissner, Ove K�ven
 *
 * FIXME: we use read|write access in all cases. Shouldn't we depend that
 * on the access of the current handle?
 */

#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#ifdef HAVE_SYS_ERRNO_H
# include <sys/errno.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif
#include <sys/ioctl.h>
#ifdef HAVE_SYS_FILIO_H
# include <sys/filio.h>
#endif
#include <time.h>
#include <unistd.h>

#include "winerror.h"
#include "winbase.h"
#include "process.h"
#include "handle.h"
#include "thread.h"
#include "request.h"
#include "async.h"

/* To avoid conflicts with the Unix socket headers. Plus we only need a few
 * macros anyway.
 */
#define USE_WS_PREFIX
#include "winsock2.h"

struct sock
{
    struct object       obj;         /* object header */
    unsigned int        state;       /* status bits */
    unsigned int        mask;        /* event mask */
    unsigned int        hmask;       /* held (blocked) events */
    unsigned int        pmask;       /* pending events */
    unsigned int        flags;       /* socket flags */
    struct event       *event;       /* event object */
    int                 errors[FD_MAX_EVENTS]; /* event errors */
    struct async_queue  read_q;      /* Queue for asynchronous reads */
    struct async_queue  write_q;     /* Queue for asynchronous writes */
};

static void sock_dump( struct object *obj, int verbose );
static int sock_signaled( struct object *obj, struct thread *thread );
static int sock_get_poll_events( struct object *obj );
static void sock_poll_event( struct object *obj, int event );
static int sock_get_fd( struct object *obj );
static int sock_get_info( struct object *obj, struct get_file_info_reply *reply, int *flags );
static void sock_destroy( struct object *obj );
static int sock_get_error( int err );
static void sock_set_error(void);

static const struct object_ops sock_ops =
{
    sizeof(struct sock),          /* size */
    sock_dump,                    /* dump */
    add_queue,                    /* add_queue */
    remove_queue,                 /* remove_queue */
    sock_signaled,                /* signaled */
    no_satisfied,                 /* satisfied */
    sock_get_poll_events,         /* get_poll_events */
    sock_poll_event,              /* poll_event */
    sock_get_fd,                  /* get_fd */
    no_flush,                     /* flush */
    sock_get_info,                /* get_file_info */
    NULL,                         /* queue_async */
    sock_destroy                  /* destroy */
};

static void sock_reselect( struct sock *sock )
{
    int ev = sock_get_poll_events( &sock->obj );
    struct pollfd pfd;

    if (debug_level)
        fprintf(stderr,"sock_reselect(%d): new mask %x\n", sock->obj.fd, ev);

    if (sock->obj.select == -1) {
        /* previously unconnected socket, is this reselect supposed to connect it? */
        if (!(sock->state & ~FD_WINE_NONBLOCKING)) return;
        /* ok, it is, attach it to the wineserver's main poll loop */
        add_select_user( &sock->obj );
    }
    /* update condition mask */
    set_select_events( &sock->obj, ev );

    /* check whether condition is satisfied already */
    pfd.fd = sock->obj.fd;
    pfd.events = ev;
    pfd.revents = 0;
    poll( &pfd, 1, 0 );
    if (pfd.revents)
        sock_poll_event( &sock->obj, pfd.revents);
}

inline static int sock_error(int s)
{
    unsigned int optval = 0, optlen;
    
    optlen = sizeof(optval);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (void *) &optval, &optlen);
    return optval ? sock_get_error(optval) : 0;
}

static void sock_poll_event( struct object *obj, int event )
{
    struct sock *sock = (struct sock *)obj;
    unsigned int emask;
    assert( sock->obj.ops == &sock_ops );
    if (debug_level)
        fprintf(stderr, "socket %d select event: %x\n", sock->obj.fd, event);
    if (sock->state & FD_CONNECT)
    {
        /* connecting */
        if (event & POLLOUT)
        {
            /* we got connected */
            sock->state |= FD_WINE_CONNECTED|FD_READ|FD_WRITE;
            sock->state &= ~FD_CONNECT;
            sock->pmask |= FD_CONNECT;
            sock->errors[FD_CONNECT_BIT] = 0;
            if (debug_level)
                fprintf(stderr, "socket %d connection success\n", sock->obj.fd);
        }
        else if (event & (POLLERR|POLLHUP))
        {
            /* we didn't get connected? */
            sock->state &= ~FD_CONNECT;
            sock->pmask |= FD_CONNECT;
            sock->errors[FD_CONNECT_BIT] = sock_error( sock->obj.fd );
            if (debug_level)
                fprintf(stderr, "socket %d connection failure\n", sock->obj.fd);
        }
    } else
    if (sock->state & FD_WINE_LISTENING)
    {
        /* listening */
        if (event & POLLIN)
        {
            /* incoming connection */
            sock->pmask |= FD_ACCEPT;
            sock->errors[FD_ACCEPT_BIT] = 0;
            sock->hmask |= FD_ACCEPT;
        }
        else if (event & (POLLERR|POLLHUP))
        {
            /* failed incoming connection? */
            sock->pmask |= FD_ACCEPT;
            sock->errors[FD_ACCEPT_BIT] = sock_error( sock->obj.fd );
            sock->hmask |= FD_ACCEPT;
        }
    } else
    {
        /* normal data flow */
        if (event & POLLIN)
        {
            char dummy;

            /* Linux 2.4 doesn't report POLLHUP if only one side of the socket
             * has been closed, so we need to check for it explicitly here */
            if (!recv( sock->obj.fd, &dummy, 1, MSG_PEEK )) event = POLLHUP;
            else
            {
                /* incoming data */
                sock->pmask |= FD_READ;
                sock->hmask |= FD_READ;
                sock->errors[FD_READ_BIT] = 0;
                if (debug_level)
                    fprintf(stderr, "socket %d is readable\n", sock->obj.fd );
            }
        }
        if (event & POLLOUT)
        {
            sock->pmask |= FD_WRITE;
            sock->hmask |= FD_WRITE;
            sock->errors[FD_WRITE_BIT] = 0;
            if (debug_level)
                fprintf(stderr, "socket %d is writable\n", sock->obj.fd);
        }
        if (event & POLLPRI)
        {
            sock->pmask |= FD_OOB;
            sock->hmask |= FD_OOB;
            sock->errors[FD_OOB_BIT] = 0;
            if (debug_level)
                fprintf(stderr, "socket %d got OOB data\n", sock->obj.fd);
        }
        if (((event & POLLERR) || ((event & (POLLIN|POLLHUP)) == POLLHUP))
            && (sock->state & (FD_READ|FD_WRITE))) {
            /* socket closing */
            sock->errors[FD_CLOSE_BIT] = sock_error( sock->obj.fd );
            sock->state &= ~(FD_WINE_CONNECTED|FD_READ|FD_WRITE);
            sock->pmask |= FD_CLOSE;
            if (debug_level)
                fprintf(stderr, "socket %d aborted by error %d\n",
                        sock->obj.fd, sock->errors[FD_CLOSE_BIT]);
        }
    }

    if (event & (POLLERR|POLLHUP))
        set_select_events( &sock->obj, -1 );
    else
        sock_reselect( sock );
    /* wake up anyone waiting for whatever just happened */
    emask = sock->pmask & sock->mask;
    if (debug_level && emask)
        fprintf(stderr, "socket %d pending events: %x\n", sock->obj.fd, emask);
    if (emask && sock->event) {
        if (debug_level) fprintf(stderr, "signalling event ptr %p\n", sock->event);
        set_event(sock->event);
    }

    /* if anyone is stupid enough to wait on the socket object itself,
     * maybe we should wake them up too, just in case? */
    wake_up( &sock->obj, 0 );
}

static void sock_dump( struct object *obj, int verbose )
{
    struct sock *sock = (struct sock *)obj;
    assert( obj->ops == &sock_ops );
    printf( "Socket fd=%d, state=%x, mask=%x, pending=%x, held=%x\n",
            sock->obj.fd, sock->state,
            sock->mask, sock->pmask, sock->hmask );
}

static int sock_signaled( struct object *obj, struct thread *thread )
{
    struct sock *sock = (struct sock *)obj;
    assert( obj->ops == &sock_ops );

    return check_select_events( sock->obj.fd, sock_get_poll_events( &sock->obj ) );
}

static int sock_get_poll_events( struct object *obj )
{
    struct sock *sock = (struct sock *)obj;
    unsigned int mask = sock->mask & sock->state & ~sock->hmask;
    int ev = 0;

    assert( obj->ops == &sock_ops );

    if (sock->state & FD_CONNECT)
        /* connecting, wait for writable */
        return POLLOUT;
    if (sock->state & FD_WINE_LISTENING)
        /* listening, wait for readable */
        return (sock->hmask & FD_ACCEPT) ? 0 : POLLIN;

    if (mask & FD_READ)  ev |= POLLIN | POLLPRI;
    if (mask & FD_WRITE) ev |= POLLOUT;
    return ev;
}

static int sock_get_fd( struct object *obj )
{
    struct sock *sock = (struct sock *)obj;
    assert( obj->ops == &sock_ops );
    return sock->obj.fd;
}

static int sock_get_info( struct object *obj, struct get_file_info_reply *reply, int *flags )
{
    struct sock *sock = (struct sock*) obj;
    assert ( obj->ops == &sock_ops );

    if (reply)
    {
        reply->type        = FILE_TYPE_PIPE;
        reply->attr        = 0;
        reply->access_time = 0;
        reply->write_time  = 0;
        reply->size_high   = 0;
        reply->size_low    = 0;
        reply->links       = 0;
        reply->index_high  = 0;
        reply->index_low   = 0;
        reply->serial      = 0;
    }
    *flags = 0;
    if (sock->flags & WSA_FLAG_OVERLAPPED) *flags |= FD_FLAG_OVERLAPPED;
    return FD_TYPE_DEFAULT;
}

static void sock_destroy( struct object *obj )
{
    struct sock *sock = (struct sock *)obj;
    assert( obj->ops == &sock_ops );

    /* FIXME: special socket shutdown stuff? */

    if ( sock->flags & WSA_FLAG_OVERLAPPED )
    {
        destroy_async_queue ( &sock->read_q );
        destroy_async_queue ( &sock->write_q );
    }

    if (sock->event)
    {
        /* if the service thread was waiting for the event object,
         * we should now signal it, to let the service thread
         * object detect that it is now orphaned... */
        if (sock->mask & FD_WINE_SERVEVENT)
            set_event( sock->event );
        /* we're through with it */
        release_object( sock->event );
    }
}

/* create a new and unconnected socket */
static struct object *create_socket( int family, int type, int protocol, unsigned int flags )
{
    struct sock *sock;
    int sockfd;

    sockfd = socket( family, type, protocol );
    if (debug_level)
        fprintf(stderr,"socket(%d,%d,%d)=%d\n",family,type,protocol,sockfd);
    if (sockfd == -1) {
        sock_set_error();
        return NULL;
    }
    fcntl(sockfd, F_SETFL, O_NONBLOCK); /* make socket nonblocking */
    if (!(sock = alloc_object( &sock_ops, -1 ))) return NULL;
    sock->obj.fd = sockfd;
    sock->state = (type != SOCK_STREAM) ? (FD_READ|FD_WRITE) : 0;
    sock->mask  = 0;
    sock->hmask = 0;
    sock->pmask = 0;
    sock->flags = flags;
    sock->event = NULL;
    sock_reselect( sock );
    clear_error();
    if (sock->flags & WSA_FLAG_OVERLAPPED)
    {
        init_async_queue (&sock->read_q);
        init_async_queue (&sock->write_q);
    }
    return &sock->obj;
}

/* accept a socket (creates a new fd) */
static struct object *accept_socket( handle_t handle )
{
    struct sock *acceptsock;
    struct sock *sock;
    int	acceptfd;
    struct sockaddr	saddr;
    int			slen;

    sock=(struct sock*)get_handle_obj(current->process,handle,
                                      GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&sock_ops);
    if (!sock)
    	return NULL;
    /* Try to accept(2). We can't be safe that this an already connected socket 
     * or that accept() is allowed on it. In those cases we will get -1/errno
     * return.
     */
    slen = sizeof(saddr);
    acceptfd = accept(sock->obj.fd,&saddr,&slen);
    if (acceptfd==-1) {
    	sock_set_error();
        release_object( sock );
	return NULL;
    }
    if (!(acceptsock = alloc_object( &sock_ops, -1 )))
    {
        release_object( sock );
        return NULL;
    }

    /* newly created socket gets the same properties of the listening socket */
    fcntl(acceptfd, F_SETFL, O_NONBLOCK); /* make socket nonblocking */
    acceptsock->obj.fd = acceptfd;
    acceptsock->state  = FD_WINE_CONNECTED|FD_READ|FD_WRITE;
    if (sock->state & FD_WINE_NONBLOCKING)
        acceptsock->state |= FD_WINE_NONBLOCKING;
    acceptsock->mask   = sock->mask;
    acceptsock->hmask  = 0;
    acceptsock->pmask  = 0;
    acceptsock->event  = NULL;
    if (sock->event && !(sock->mask & FD_WINE_SERVEVENT))
        acceptsock->event = (struct event *)grab_object( sock->event );
    acceptsock->flags = sock->flags;
    if ( acceptsock->flags & WSA_FLAG_OVERLAPPED )
    {
	init_async_queue ( &acceptsock->read_q );
	init_async_queue ( &acceptsock->write_q );
    }

    sock_reselect( acceptsock );
    clear_error();
    sock->pmask &= ~FD_ACCEPT;
    sock->hmask &= ~FD_ACCEPT;
    sock_reselect( sock );
    release_object( sock );
    return &acceptsock->obj;
}

/* set the last error depending on errno */
static int sock_get_error( int err )
{
    switch (err)
    {
        case EINTR:             return WSAEINTR; break;
        case EBADF:             return WSAEBADF; break;
        case EPERM:
        case EACCES:            return WSAEACCES; break;
        case EFAULT:            return WSAEFAULT; break;
        case EINVAL:            return WSAEINVAL; break;
        case EMFILE:            return WSAEMFILE; break;
        case EWOULDBLOCK:       return WSAEWOULDBLOCK; break;
        case EINPROGRESS:       return WSAEINPROGRESS; break;
        case EALREADY:          return WSAEALREADY; break;
        case ENOTSOCK:          return WSAENOTSOCK; break;
        case EDESTADDRREQ:      return WSAEDESTADDRREQ; break;
        case EMSGSIZE:          return WSAEMSGSIZE; break;
        case EPROTOTYPE:        return WSAEPROTOTYPE; break;
        case ENOPROTOOPT:       return WSAENOPROTOOPT; break;
        case EPROTONOSUPPORT:   return WSAEPROTONOSUPPORT; break;
        case ESOCKTNOSUPPORT:   return WSAESOCKTNOSUPPORT; break;
        case EOPNOTSUPP:        return WSAEOPNOTSUPP; break;
        case EPFNOSUPPORT:      return WSAEPFNOSUPPORT; break;
        case EAFNOSUPPORT:      return WSAEAFNOSUPPORT; break;
        case EADDRINUSE:        return WSAEADDRINUSE; break;
        case EADDRNOTAVAIL:     return WSAEADDRNOTAVAIL; break;
        case ENETDOWN:          return WSAENETDOWN; break;
        case ENETUNREACH:       return WSAENETUNREACH; break;
        case ENETRESET:         return WSAENETRESET; break;
        case ECONNABORTED:      return WSAECONNABORTED; break;
        case EPIPE:
        case ECONNRESET:        return WSAECONNRESET; break;
        case ENOBUFS:           return WSAENOBUFS; break;
        case EISCONN:           return WSAEISCONN; break;
        case ENOTCONN:          return WSAENOTCONN; break;
        case ESHUTDOWN:         return WSAESHUTDOWN; break;
        case ETOOMANYREFS:      return WSAETOOMANYREFS; break;
        case ETIMEDOUT:         return WSAETIMEDOUT; break;
        case ECONNREFUSED:      return WSAECONNREFUSED; break;
        case ELOOP:             return WSAELOOP; break;
        case ENAMETOOLONG:      return WSAENAMETOOLONG; break;
        case EHOSTDOWN:         return WSAEHOSTDOWN; break;
        case EHOSTUNREACH:      return WSAEHOSTUNREACH; break;
        case ENOTEMPTY:         return WSAENOTEMPTY; break;
#ifdef EPROCLIM
        case EPROCLIM:          return WSAEPROCLIM; break;
#endif
#ifdef EUSERS
        case EUSERS:            return WSAEUSERS; break;
#endif
#ifdef EDQUOT
        case EDQUOT:            return WSAEDQUOT; break;
#endif
#ifdef ESTALE
        case ESTALE:            return WSAESTALE; break;
#endif
#ifdef EREMOTE
        case EREMOTE:           return WSAEREMOTE; break;
#endif
    default: errno=err; perror("sock_set_error"); return ERROR_UNKNOWN; break;
    }
}

/* set the last error depending on errno */
static void sock_set_error(void)
{
    set_error( sock_get_error( errno ) );
}

/* create a socket */
DECL_HANDLER(create_socket)
{
    struct object *obj;

    reply->handle = 0;
    if ((obj = create_socket( req->family, req->type, req->protocol, req->flags )) != NULL)
    {
        reply->handle = alloc_handle( current->process, obj, req->access, req->inherit );
        release_object( obj );
    }
}

/* accept a socket */
DECL_HANDLER(accept_socket)
{
    struct object *obj;

    reply->handle = 0;
    if ((obj = accept_socket( req->lhandle )) != NULL)
    {
        reply->handle = alloc_handle( current->process, obj, req->access, req->inherit );
        release_object( obj );
    }
}

/* set socket event parameters */
DECL_HANDLER(set_socket_event)
{
    struct sock *sock;
    struct event *oevent;
    unsigned int omask;

    sock=(struct sock*)get_handle_obj(current->process,req->handle,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&sock_ops);
    if (!sock)
	return;
    oevent = sock->event;
    omask  = sock->mask;
    sock->mask    = req->mask;
    sock->event   = get_event_obj( current->process, req->event, EVENT_MODIFY_STATE );
    if (debug_level && sock->event) fprintf(stderr, "event ptr: %p\n", sock->event);
    sock_reselect( sock );
    if (sock->mask)
        sock->state |= FD_WINE_NONBLOCKING;

    /* if a network event is pending, signal the event object 
       it is possible that FD_CONNECT or FD_ACCEPT network events has happened
       before a WSAEventSelect() was done on it. 
       (when dealing with Asynchronous socket)  */
    if (sock->pmask & sock->mask)
        set_event(sock->event);
    
    if (oevent)
    {
        if ((oevent != sock->event) && (omask & FD_WINE_SERVEVENT))
            /* if the service thread was waiting for the old event object,
             * we should now signal it, to let the service thread
             * object detect that it is now orphaned... */
            set_event( oevent );
        /* we're through with it */
        release_object( oevent );
    }
    release_object( &sock->obj );
}

/* get socket event parameters */
DECL_HANDLER(get_socket_event)
{
    struct sock *sock;

    sock=(struct sock*)get_handle_obj(current->process,req->handle,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&sock_ops);
    if (!sock)
    {
        reply->mask  = 0;
        reply->pmask = 0;
        reply->state = 0;
        set_error( WSAENOTSOCK );
        return;
    }
    reply->mask  = sock->mask;
    reply->pmask = sock->pmask;
    reply->state = sock->state;
    set_reply_data( sock->errors, min( get_reply_max_size(), sizeof(sock->errors) ));

    if (req->service)
    {
        handle_t s_event = req->s_event;
        if (s_event)
        {
            struct event *sevent = get_event_obj(current->process, req->s_event, 0);
            if (sevent == sock->event) s_event = 0;
            release_object( sevent );
        }
        if (!s_event)
        {
            if (req->c_event)
            {
                struct event *cevent = get_event_obj(current->process, req->c_event, EVENT_MODIFY_STATE);
                reset_event( cevent );
                release_object( cevent );
            }
            sock->pmask = 0;
            sock_reselect( sock );
        }
        else set_error(WSAEINVAL);
    }
    release_object( &sock->obj );
}

/* re-enable pending socket events */
DECL_HANDLER(enable_socket_event)
{
    struct sock *sock;

    sock=(struct sock*)get_handle_obj(current->process,req->handle,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&sock_ops);
    if (!sock)
    	return;
    sock->pmask &= ~req->mask; /* is this safe? */
    sock->hmask &= ~req->mask;
    sock->state |= req->sstate;
    sock->state &= ~req->cstate;
    sock_reselect( sock );

    /* service trigger */
    if (req->mask & FD_WINE_SERVEVENT)
    {
        sock->pmask |= FD_WINE_SERVEVENT;
        if (sock->event) {
            if (debug_level) fprintf(stderr, "signalling service event ptr %p\n", sock->event);
            set_event(sock->event);
        }
    }

    release_object( &sock->obj );
}
