#include <assert.h>
#include <malloc.h>
#include <stdlib.h>

#include "epoll-socket.h"
#include "epoll.h"
#include "error.h"
#include "poll-group.h"
#include "port.h"
#include "queue.h"
#include "tree.h"
#include "util.h"
#include "win.h"

static ep_port_t* _ep_port_alloc(void) {
  ep_port_t* port_info = malloc(sizeof *port_info);
  if (port_info == NULL)
    return_error(NULL, ERROR_NOT_ENOUGH_MEMORY);

  return port_info;
}

static void _ep_port_free(ep_port_t* port) {
  assert(port != NULL);
  free(port);
}

ep_port_t* ep_port_new(HANDLE iocp) {
  ep_port_t* port_info;

  port_info = _ep_port_alloc();
  if (port_info == NULL)
    return NULL;

  memset(port_info, 0, sizeof *port_info);

  port_info->iocp = iocp;
  queue_init(&port_info->update_queue);
  tree_init(&port_info->sock_tree);
  reflock_tree_node_init(&port_info->handle_tree_node);

  return port_info;
}

static int _ep_port_close_iocp(ep_port_t* port_info) {
  HANDLE iocp = port_info->iocp;
  port_info->iocp = NULL;

  if (!CloseHandle(iocp))
    return_error(-1);

  return 0;
}

int ep_port_close(ep_port_t* port_info) {
  int result;

  result = _ep_port_close_iocp(port_info);

  return result;
}

int ep_port_delete(ep_port_t* port_info) {
  tree_node_t* tree_node;

  if (port_info->iocp != NULL)
    _ep_port_close_iocp(port_info);

  while ((tree_node = tree_root(&port_info->sock_tree)) != NULL) {
    ep_sock_t* sock_info = container_of(tree_node, ep_sock_t, tree_node);
    ep_sock_force_delete(port_info, sock_info);
  }

  for (size_t i = 0; i < array_count(port_info->poll_group_allocators); i++) {
    poll_group_allocator_t* pga = port_info->poll_group_allocators[i];
    if (pga != NULL)
      poll_group_allocator_delete(pga);
  }

  _ep_port_free(port_info);

  return 0;
}

int ep_port_update_events(ep_port_t* port_info) {
  queue_t* update_queue = &port_info->update_queue;

  /* Walk the queue, submitting new poll requests for every socket that needs
   * it. */
  while (!queue_empty(update_queue)) {
    queue_node_t* queue_node = queue_first(update_queue);
    ep_sock_t* sock_info = container_of(queue_node, ep_sock_t, queue_node);

    if (ep_sock_update(port_info, sock_info) < 0)
      return -1;

    /* ep_sock_update() removes the socket from the update list if
     * successfull. */
  }

  return 0;
}

size_t ep_port_feed_events(ep_port_t* port_info,
                           OVERLAPPED_ENTRY* completion_list,
                           size_t completion_count,
                           struct epoll_event* event_list,
                           size_t max_event_count) {
  if (completion_count > max_event_count)
    abort();

  size_t event_count = 0;

  for (size_t i = 0; i < completion_count; i++) {
    OVERLAPPED* overlapped = completion_list[i].lpOverlapped;
    ep_sock_t* sock_info = ep_sock_from_overlapped(overlapped);
    struct epoll_event* ev = &event_list[event_count];

    event_count += ep_sock_feed_event(port_info, sock_info, ev);
  }

  return event_count;
}

int ep_port_add_socket(ep_port_t* port_info,
                       ep_sock_t* sock_info,
                       SOCKET socket) {
  return tree_add(&port_info->sock_tree, &sock_info->tree_node, socket);
}

int ep_port_del_socket(ep_port_t* port_info, ep_sock_t* sock_info) {
  return tree_del(&port_info->sock_tree, &sock_info->tree_node);
}

ep_sock_t* ep_port_find_socket(ep_port_t* port_info, SOCKET socket) {
  return ep_sock_find_in_tree(&port_info->sock_tree, socket);
}

static poll_group_allocator_t* _ep_port_get_poll_group_allocator(
    ep_port_t* port_info,
    size_t protocol_id,
    const WSAPROTOCOL_INFOW* protocol_info) {
  poll_group_allocator_t** pga;

  assert(protocol_id < array_count(port_info->poll_group_allocators));

  pga = &port_info->poll_group_allocators[protocol_id];
  if (*pga == NULL)
    *pga = poll_group_allocator_new(port_info, protocol_info);

  return *pga;
}

poll_group_t* ep_port_acquire_poll_group(
    ep_port_t* port_info,
    size_t protocol_id,
    const WSAPROTOCOL_INFOW* protocol_info) {
  poll_group_allocator_t* pga =
      _ep_port_get_poll_group_allocator(port_info, protocol_id, protocol_info);
  return poll_group_acquire(pga);
}

void ep_port_release_poll_group(poll_group_t* poll_group) {
  poll_group_release(poll_group);
}

void ep_port_request_socket_update(ep_port_t* port_info,
                                   ep_sock_t* sock_info) {
  if (ep_port_is_socket_update_pending(port_info, sock_info))
    return;
  queue_append(&port_info->update_queue, &sock_info->queue_node);
  assert(ep_port_is_socket_update_pending(port_info, sock_info));
}

void ep_port_clear_socket_update(ep_port_t* port_info, ep_sock_t* sock_info) {
  if (!ep_port_is_socket_update_pending(port_info, sock_info))
    return;
  queue_remove(&sock_info->queue_node);
}

bool ep_port_is_socket_update_pending(ep_port_t* port_info,
                                      ep_sock_t* sock_info) {
  unused(port_info);
  return queue_enqueued(&sock_info->queue_node);
}
